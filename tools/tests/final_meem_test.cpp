// A low final meem is bounded by its own loop valley and inward corner.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static SearchContext geometry(QPainterPath path, WordResultInfo::SubWordInfo& dummy,
                              const QJsonObject& form) {
    path.setFillRule(Qt::WindingFill);
    SearchContext context{.form = form, .transformedPath = path, .subword = dummy};
    context.bbox = path.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(path, context.subPaths, extrema);
    for (const auto& point : extrema) if (point.pathIndex == 0) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    check(context.timeSorted.size() >= 3, "Missing final meem outline");
    context.setOrientation();
    return context;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject form;
    for (const auto& value : config.value("final").toArray())
        if (value.toObject().value("id") == "meem-loop-tail-fina") form = resolvedBodyForm(config, value.toObject());
    check(form.value("body").toObject().value("shape") == "contour-route" && !form.contains("method"), "Final meem should declare its route independently of the cuts");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 fixture is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page 600 fixture");
    auto& word = analysis.page.lines[1].words[4];
    auto& sub = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[sub.paths.front()];
    auto source = shape.transform.map(shape.path);
    source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(source, dummy, form);
    check(declaredSharedBoundary(context, form), "Meem loop and tail route did not match");
    const auto cut = context.res;
    check(std::abs(cut.topBaseLine.time - 4.797530577555296) < 1e-5 && cut.bottomBaseLine.time == 16,
          "Meem boundary moved from its reviewed valley and corner");
    check(interiorChord(source, cut.topBaseLine, cut.bottomBaseLine), "Meem cut crosses background");
    const auto pieces = cutRegions(source, {}, {cut}, context.orientation, true);
    check(pieces.size() == 2 && toSubpaths(pieces[1].path).size() == 2,
          "Meem lost its complete loop and counter");
    const auto body = pieces[1].path.boundingRect();
    check(std::abs(body.left() - context.bbox.left()) < 1e-5 &&
          std::abs(body.bottom() - context.bbox.bottom()) < 1e-5 &&
          body.top() > context.bbox.top() + context.bbox.height() / 2,
          "Meem lost its tail or acquired the lam ascender");
    const auto same = [&](const Stretching& actual, const QTransform& mapping = {}) {
        return QLineF(mapping.map(cut.topBaseLine.point), actual.topBaseLine.point).length() < 1e-5 &&
               QLineF(mapping.map(cut.bottomBaseLine.point), actual.bottomBaseLine.point).length() < 1e-5;
    };
    Stretching unrelated{};
    unrelated.topBaseLine.point = {-200, 300}; unrelated.bottomBaseLine.point = {-100, 500};
    dummy.joins.insert(unrelated);
    context.baseText = QStringLiteral("اختبار");
    context.pairedStrokeWidth = 1000;
    check(declaredSharedBoundary(context, form) && same(context.res), "Meem cut depends on preceding text or landmarks");
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy, form);
        check(declaredSharedBoundary(scaled, form) && same(scaled.res, mapping), "Meem cut changed under scale or translation");
    }
    check(analysis.segmentSubword(599, 1, 4, 0, nullptr, true), "Yeh-ain-lam-meem remains partial");
    check(sub.baseGlyphs.size() == 4 && sub.joins.size() == 3 &&
          sub.matchedTemplates.contains("meem-loop-tail-fina") && sub.kashidaPaths.size() == 3 &&
          std::all_of(sub.kashidaPaths.begin(), sub.kashidaPaths.end(), [](const auto& p) { return !p.isEmpty(); }),
          "Expected four bodies with three separate connecting spans");
    const auto& finalJoin = *sub.joins.rbegin();
    check(finalJoin.fixedLeftBoundary && std::abs(finalJoin.topLeft.time - cut.topBaseLine.time) < 1e-5 &&
          finalJoin.bottomLeft.time == 16, "Final pipeline replaced the meem's independent boundary");
    const auto bodies = sub.baseGlyphs;
    const auto spans = sub.kashidaPaths;
    check(analysis.segmentSubword(599, 1, 4, 0, nullptr, true) && spans == sub.kashidaPaths,
          "Repeated segmentation changed the connecting spans");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(bodies[i].path == sub.baseGlyphs[i].path, "Repeated segmentation changed a body mask");
    auto missingForm = config;
    QJsonArray remaining;
    for (const auto& value : config.value("final").toArray())
        if (!value.toObject().value("current").toString().contains(QChar(0x0645))) remaining.append(value);
    missingForm.insert("final", remaining);
    check(!analysis.segmentSubword(599, 1, 4, 0, nullptr, true, &missingForm),
          "Missing meem form was concealed by the number of body regions");
    // The projection of a folded rim must stop at uninterrupted ink. Also
    // exercise the assembler's independent final-shoulder proposal beside lām.
    for (const auto& loc : std::vector<std::array<int, 4>>{{435,3,9,1}, {184,14,10,1}}) {
        QFile fixture(QString("pages/page%1.dat").arg(loc[0]));
        check(fixture.open(QIODevice::ReadOnly), "Folded-rim fixture missing");
        PageAnalysisResult a; QDataStream input(&fixture); input >> a.page;
        check(a.segmentSubword(loc[0]-1, loc[1]-1, loc[2]-1, loc[3]-1, nullptr, true),
              "Folded rim lost its visible cut or terminal shoulder");
        const auto& result = a.page.lines[loc[1]-1].words[loc[2]-1].wordResultInfo.subWords[loc[3]-1];
        if (loc[0] == 184)
            check(result.letterDiagnostics.last().toObject().value("template") == "meem-shoulder-fina",
                  "Assembly did not retain the final body's own shoulder");
    }
    const auto counterForm = resolvedLetterForm(config, "meem-counter-fina");
    QFile steps("pages/page2.dat");
    check(steps.open(QIODevice::ReadOnly), "Final meem step fixtures missing");
    PageAnalysisResult stepped;
    QDataStream stepInput(&steps);
    stepInput >> stepped.page;
    for (const auto& [line, word, corner] : {std::array{6, 7, 33}, std::array{8, 1, 26}}) {
        const auto& item = stepped.page.lines[line-1].words[word-1];
        const auto& result = item.wordResultInfo.subWords.front();
        const auto shape = item.bodyShape(result);
        auto uncut = geometry(shape.transform.map(shape.path).translated(shape.pos), dummy, counterForm);
        check(declaredSharedBoundary(uncut, counterForm) && uncut.res.bottomBaseLine.time == corner,
              "Final meem missed the inward corner above its stepped rim");
        const auto cut = uncut.res;
        check(stepped.segmentSubword(1, line-1, word-1, 0, nullptr, true) &&
              result.baseGlyphs.size() == 2 && result.kashidaPaths.size() == 1 && !result.kashidaPaths.front().isEmpty(),
              "Final meem step lost a letter or its incoming connector");
        const auto receiving = result.kashidaCuts.front().second;
        check(QLineF(receiving.p1(), cut.topBaseLine.point).length() < 1e-5 &&
              QLineF(receiving.p2(), cut.bottomBaseLine.point).length() < 1e-5,
              "The assembled cut clipped the stepped rim");
    }
    qInfo("Final meem passed: own valley/corner 4.7975/16, retained loop/counter/tail, no lam ascender, independence from neighboring landmarks, scale/translation invariance, four bodies/three spans and deterministic masks.");
}
