#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}

static QByteArray readFixture(const QString& name) {
    QFile file(name);
    check(file.open(QIODevice::ReadOnly), "Missing native fixture");
    return file.readAll();
}

constexpr double hookTurnAngle = 11.2;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Invalid letter forms");
    const QStringList toothRules{"beh-init", "beh-shallow-step-init", "beh-upright-medi", "beh-tooth-medi"};
    int checkedSelectors = 0;
    for (auto phase : {"initial", "medial"}) {
        for (const auto& value : config.value(phase).toArray()) {
            const auto rule = value.toObject();
            if (!toothRules.contains(rule.value("id").toString())) continue;
            const auto letters = rule.value("current").toString();
            check(letters.count(QChar(u'ئ')) == 1 && letters.count(QChar(u'ى')) == 1,
                  "Both yeh variants must be eligible for each tooth form");
            ++checkedSelectors;
        }
    }
    check(checkedSelectors == toothRules.size(), "Missing tooth selector");
    for (const auto& value : config.value("final").toArray()) {
        auto rule = value.toObject();
        if (rule.value("id") != "hah-fina") continue;
        rule = shapeSettings(rule, "route");
        check(rule.value("penWidth").toDouble() == 0.5 &&
              rule.value("descendingTurn").toDouble() == hookTurnAngle,
              "The route should use the manuscript pen width and cut where the hook levels out");
        for (const auto& invalid : QJsonArray{0, -0.5, "half"}) {
            rule.insert("penWidth", invalid);
            check(!invalidWalkSettings(rule, "hah-fina").isEmpty(), "Invalid pen width accepted");
        }
    }

    AFont font;
    QDataStream fontStream(readFixture("input/font.dat"));
    fontStream >> font;
    PageAnalysisResult::initQuranText();
    // Numbered corners identify landmarks in these native fixtures.
    struct Fixture { int page, line, word, subword; const char* form; int corner; };
    const Fixture fixtures[] = {
        {6, 7, 2, 3, "beh-init", -1}, {22, 1, 7, 2, "beh-tooth-medi", -1},
        {24, 7, 2, 1, "hah-fina", 18}, {25, 4, 5, 3, "hah-fina", 19},
        {56, 6, 9, 2, "hah-fina", 32}, {130, 6, 8, 1, "hah-fina", 23},
        {211, 4, 8, 2, "hah-stacked-fina", 18}, {566, 13, 11, 2, "hah-stacked-fina", 17},
        {395, 9, 7, 3, "hah-fina", 27}, {417, 13, 4, 2, "hah-fina", 34}
    };
    for (const auto& f : fixtures) {
        PageAnalysisResult analysis;
        QDataStream stream(readFixture(QString("pages/page%1.dat").arg(f.page)));
        stream >> analysis.page;
        analysis.resolveWordBoundaries(f.page, &font);
        auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        if (word.wordResultInfo.subWords.empty()) analysis.detectSubWords(f.page - 1, f.line - 1, f.word - 1, &font);
        check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true),
              "Native subword remains partial");
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        check(sub.matchedTemplates.contains(f.form), "Expected body form did not match");
        if (f.corner < 0) continue;

        const auto& outline = word.paths[sub.paths.front()];
        const auto mapped = [&](const QPainterPath& path) {
            auto ink = outline.transform.map(path);
            ink.translate(outline.pos);
            ink.setFillRule(Qt::WindingFill);
            return ink;
        };
        const auto ink = mapped(outline.path);
        const auto finalBody = mapped(sub.baseGlyphs.back().path).boundingRect();
        check(std::abs(finalBody.bottom() - ink.boundingRect().bottom()) < 0.001, "Final letter lost its bowl");
        const auto& join = *sub.joins.rbegin();
        if (QString(f.form) == "hah-stacked-fina") {
            check(sub.kashidaPaths.front().isEmpty(), "Stacked junction generated a kashida");
            check(join.bottomBaseLine.time == f.corner &&
                  join.topBaseLine.point.x() < join.bottomBaseLine.point.x() &&
                  std::abs(join.topBaseLine.point.y() - join.bottomBaseLine.point.y()) < 1e-6,
                  "Shared cut must start at the entry corner and cross left");
            const auto upperBody = mapped(sub.baseGlyphs.front().path).boundingRect();
            check(std::abs(upperBody.bottom() - join.bottomBaseLine.point.y()) < 0.001 &&
                  upperBody.top() < finalBody.top(), "Upper tooth lost its attaching stroke");
            continue;
        }
        check(!sub.kashidaPaths.back().isEmpty(), "Ordinary connector was lost");
        const auto curves = baseContourCurves(ink);
        const auto bottom = join.bottomLeft;
        const auto corner = curves.at(f.corner).pt1();
        check(bottom.point.y() > corner.y() + 0.5 && bottom.point.x() > corner.x(),
              "Final cut stopped before the hook entered the connector");
        const int segment = int(std::floor(bottom.time));
        const auto& flank = curves.at(segment);
        const double t = bottom.time - segment;
        const auto chord = join.topLeft.point - bottom.point;
        const double angle = std::atan2(std::abs(chord.x()), std::abs(chord.y())) * 180 / M_PI;
        if (std::abs(angle - hookTurnAngle) >= 0.001)
            qFatal("Page %d: final cut reached %.3f degrees at contour time %.5f instead of the declared turn",
                   f.page, angle, bottom.time);
        if (t > 1e-5 && t < 1 - 1e-5) {
            const auto tangent = flank.pointAt(t + 1e-5) - flank.pointAt(t - 1e-5);
            check(std::abs(QPointF::dotProduct(tangent, chord)) <
                  1e-4 * std::hypot(tangent.x(), tangent.y()) * std::hypot(chord.x(), chord.y()),
                  "Final cut must cross the descending stroke along its normal");
        }
        if (f.page == 25) {
            auto preview = config;
            auto finals = preview.value("final").toArray();
            for (int i = 0; i < finals.size(); ++i) {
                auto rule = finals[i].toObject();
                if (rule.value("id") != "hah-fina") continue;
                auto cuts=rule.value("cuts").toObject(), route=cuts.value("route").toObject();
                route.insert("descendingTurn", 30); cuts.insert("route",route); rule.insert("cuts",cuts);
                finals[i] = rule;
            }
            preview.insert("final", finals);
            check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &preview),
                  "Turn-angle preview failed");
            const auto changed = sub.joins.rbegin()->bottomLeft;
            check(changed.point.x() < bottom.point.x() && changed.point.y() < bottom.point.y(),
                  "A steeper turn angle should move the cut toward the start of the hook");
        }
    }
}
