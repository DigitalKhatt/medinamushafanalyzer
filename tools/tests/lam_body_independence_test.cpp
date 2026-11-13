// Neighbor changes may reject an assembly, never remove intrinsic lām evidence.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static QByteArray snapshot(const LamMedialBodyEvidence& evidence) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    const auto point = [&](const PathExtrema& p) {
        out << p.pathIndex << p.time << p.point << p.isX << p.isY << p.isSharp;
    };
    const auto optionalPoint = [&](const auto& p) { out << bool(p); if (p) point(*p); };
    const auto route = [&](const auto& r) {
        out << bool(r);
        if (!r) return;
        point(r->start); point(r->origin); point(r->anchor); point(r->topBefore); point(r->topEnd);
        out << r->forwardPrevious << r->topPrevious << r->endpointTolerance;
    };
    out << evidence.source << quint64(evidence.features.crests.size());
    for (const auto& crest : evidence.features.crests) point(crest);
    out << quint64(evidence.features.forks.size());
    for (const auto& fork : evidence.features.forks) {
        point(fork.tip);
        out << fork.forwardFlank << fork.backwardFlank << fork.rightForward << fork.fork << fork.positiveTurn;
    }
    out << quint64(evidence.stems.size());
    for (const auto& stem : evidence.stems) {
        point(stem.crest);
        for (const auto& foot : stem.descendingFlanks) optionalPoint(foot);
        for (const auto& r : stem.entryRoutes) route(r);
        out << quint64(stem.exitRoutes.size());
        for (const auto& alternatives : stem.exitRoutes) for (const auto& r : alternatives) route(r);
        out << quint64(stem.forks.size());
        for (const auto index : stem.forks) out << quint64(index);
    }
    out << quint64(evidence.turningShafts.size());
    for (const auto& shaft : evidence.turningShafts) {
        point(shaft.crest); out << shaft.stroke;
        for (const auto& turn : shaft.turns) optionalPoint(turn);
    }
    return bytes;
}

static void checkConnectedShafts() {
    // Two ascenders in one continuous outline, one with a flat nib.
    // Each fork must be associated only with the shaft above its own ink.
    QPainterPath ink;
    ink.moveTo(0, 7);
    for (const QPointF& point : {QPointF{2, 7}, {2, 1}, {2.5, 0}, {3, 1}, {3, 7},
                                {5, 7}, {5, 1}, {5.45, 0}, {5.55, 0}, {6, 1}, {6, 7}, {9, 7},
                                {9, 9}, {6, 9}, {5.5, 8}, {5, 9}, {3, 9}, {2.5, 8},
                                {2, 9}, {0, 9}})
        ink.lineTo(point);
    ink.closeSubpath();
    WordResultInfo::SubWordInfo empty;
    SearchContext context{.transformedPath = ink, .subword = empty};
    check(readTerminalOutline(context), "Two-shaft fixture outline missing");
    const auto evidence = recognizeMedialLamBodies(inkOutline(context),
        QJsonObject{{"shape", "forked-stem"}, {"minFlankSpan", .25}});
    int shafts = 0;
    for (const auto& stem : evidence.stems) {
        if (stem.crest.point.y() != 0) continue;
        ++shafts;
        check(stem.forks.size() == 1, "Shaft did not claim exactly its own fork");
        const auto tip = evidence.features.forks[stem.forks[0]].tip.point;
        check(tip == QPointF(stem.crest.point.x() < 4 ? 2.5 : 5.5, 8),
              "Shaft claimed the fork of another letter");
    }
    check(shafts == 2, "The two shaft crests were not found");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    checkConnectedShafts();
    const auto config = PageAnalysisResult::letterForms();
    struct Fixture { int page, line, word, sub; const char* form; };
    const std::array fixtures{
        Fixture{3, 1, 5, 1, "lam-medi"},       // Ordinary stem.
        Fixture{415, 11, 6, 1, "lam-medi"},   // Folded fork beside a thick seen.
        Fixture{296, 15, 4, 1, "lam-medi"},   // Closed-loop kāf attachment.
        Fixture{600, 2, 4, 2, "lam-medi"},    // Lām–alef shared return.
        Fixture{360, 6, 3, 3, "lam-turning-flanks-medi"},
        Fixture{135, 10, 5, 3, "lam-turning-flanks-medi"}, // Former rounded variant.
    };
    int projectedRoutes = 0, recognizedTurns = 0;
    for (const auto& f : fixtures) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Lām fixture missing");
        PageAnalysisResult analysis;
        QDataStream in(&file); in >> analysis.page;
        const auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        const auto& sub = word.wordResultInfo.subWords[f.sub - 1];
        const auto shape = word.bodyShape(sub);
        WordResultInfo::SubWordInfo empty;
        SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos), .subword = empty};
        check(readTerminalOutline(context), "Lām source outline missing");
        context.nextIter = context.xSorted.begin();
        const auto form = resolvedBodyForm(config, namedLetterForm(config, f.form));
        const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
        const auto evidence = recognizeMedialLamBodies(inkOutline(context), body);
        check(!evidence.stems.empty() || !evidence.turningShafts.empty(), "No body evidence without joins");
        const auto original = snapshot(evidence);

        // Exercise freshly computed evidence, not merely a cache hit.
        context.pairedStrokeWidth = 10000;
        context.nextIter = context.xSorted.end();
        context.form = QJsonObject{{"bottomProbe", "invalid"}, {"minRise", 1e9}};
        context.joinDefaults = QJsonObject{{"enabled", false}};
        context.baseText = QStringLiteral("unrelated");
        context.res.topBaseLine.point = {-10000, 10000};
        empty.joins.insert(context.res);
        check(snapshot(recognizeMedialLamBodies(inkOutline(context), body)) == original,
              "Neighbor joins, widths or assembly state changed body recognition");

        OutlineSearchCache cache;
        context.outlineCache = &cache;
        const auto cached = recognizedMedialLamBodies(context, body);
        auto edited = form;
        auto policy = cuts;
        policy.insert("forkOffset", .25);
        policy.insert("placement", QJsonObject{{"entryOverlap", 0}, {"turnAngle", 75}});
        edited.insert("cuts", policy);
        check(recognizedMedialLamBodies(context, edited.value("body").toObject()) == cached,
              "A cut-only edit invalidated intrinsic body evidence");

        // Every saved route must project identically to recognizing that
        // same route directly. This also catches stale temporary iterators.
        context.form = {}; context.res = {}; context.pairedStrokeWidth = .5;
        context.nextIter = context.xSorted.begin(); empty.joins.clear();
        const bool forked = body.value("shape") == "forked-stem";
        const auto routes = forked ? body.value("exitRoutes").toArray() : QJsonArray{};
        const auto policies = cuts.value("exitRoutes").toArray();
        for (const auto& stem : evidence.stems) {
            for (int r = -1; r < routes.size(); ++r) {
                if (r < 0 && (!forked || !body.value("entryRoute").isObject())) continue;
                const auto route = r < 0 ? body.value("entryRoute").toObject() : routes[r].toObject();
                const auto policy = r < 0 ? cuts.value("entryRoute").toObject() : policies[r].toObject();
                for (int reverse = 0; reverse < 2; ++reverse) {
                    context.res = {};
                    const bool direct = declaredContourRouteCut(context, route, policy, &stem.crest, reverse);
                    const auto expected = context.res;
                    context.res = {};
                    const bool stored = projectSavedRoute(context,
                        r < 0 ? stem.entryRoutes[reverse] : stem.exitRoutes[r][reverse], route, policy);
                    check(direct == stored, "Saved route changed projection success");
                    if (!direct) continue;
                    ++projectedRoutes;
                    check(expected.topBaseLine.point == context.res.topBaseLine.point &&
                          expected.bottomBaseLine.point == context.res.bottomBaseLine.point &&
                          expected.topBaseLine.time == context.res.topBaseLine.time &&
                          expected.bottomBaseLine.time == context.res.bottomBaseLine.time,
                          "Saved route changed exact cut endpoints");
                }
            }
        }
        for (const auto& shaft : evidence.turningShafts)
            for (const auto& turn : shaft.turns) recognizedTurns += bool(turn);

        if (forked && !evidence.stems.empty()) {
            // An impossible neighbor placement is rejected by assembly,
            // without removing any of the intrinsic candidates.
            const QPointF impossible(context.bbox.left() - 100, context.bbox.top() - 100);
            LetterFormInspection report; report.formId = f.form; report.ink = context.transformedPath;
            formInspection = &report;
            const auto rejected = assembleMedialLamCuts(context, config, form, evidence, {},
                impossible, impossible, .5, false, false, true, {});
            formInspection = nullptr;
            check(rejected.empty(), "Assembly accepted an impossible neighboring slot");
            check(std::any_of(report.reasons.cbegin(), report.reasons.cend(), [](int n) { return n > 0; }),
                  "Assembly did not explain a neighbor rejection");
            check(snapshot(evidence) == original, "Assembly modified reusable body evidence");
        }
        qInfo("page%d:l%d-w%d-s%d: independent %s evidence, cached policy edit and assembly rejection passed",
              f.page, f.line, f.word, f.sub, f.form);
        if (f.page == 3) {
            auto blocked = config;
            blocked.insert("initial", QJsonArray{});
            blocked.insert("final", QJsonArray{});
            blocked.insert("medial", QJsonArray{form});
            const auto inspected = inspectLetterForm(analysis, f.page - 1, f.line - 1, f.word - 1,
                f.sub - 1, nullptr, blocked, f.form);
            check(!inspected.complete && inspected.attempted && !inspected.landmarks.empty(),
                  "A failed preceding join hid the independently recognized lām landmarks");
            check(inspected.candidates.empty(), "A missing preceding boundary produced an assembled lām cut");
        }
    }
    check(projectedRoutes > 0 && recognizedTurns > 0, "Tests did not exercise routes and turning shafts");
    qInfo("Lām independence passed: %d exact route projections; %d intrinsic shaft turns.", projectedRoutes, recognizedTurns);
}
