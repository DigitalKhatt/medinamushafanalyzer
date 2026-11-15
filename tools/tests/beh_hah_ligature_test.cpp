// Private geometry checks run without the surrounding letter templates.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool passed, const QString& message) {
    if (!passed) qFatal("%s", qPrintable(message));
}

static SearchContext geometry(QPainterPath path, WordResultInfo::SubWordInfo& dummy) {
    path.setFillRule(Qt::WindingFill);
    SearchContext context{.transformedPath = path, .subword = dummy};
    context.bbox = path.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(path, context.subPaths, extrema);
    for (const auto& point : extrema) if (point.pathIndex == 0) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    context.setOrientation();
    return context;
}

static bool same(const Stretching& first, const Stretching& second, const QTransform& mapping = {}) {
    return QLineF(mapping.map(first.topBaseLine.point), second.topBaseLine.point).length() < 1e-4 &&
        QLineF(mapping.map(first.bottomBaseLine.point), second.bottomBaseLine.point).length() < 1e-4;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject hahRule, noonRule;
    for (const auto& value : config.value("medial").toArray())
        if (value.toObject().value("id") == "hah-stacked-medi") hahRule = value.toObject();
    for (const auto& value : config.value("final").toArray())
        if (value.toObject().value("id") == "noon-fina") noonRule = value.toObject();
    check(hahRule.value("sharedEntry").toBool(), "Stacked hah must have a shared ligature entry");
    struct StackedWord {
        int page, line, word;
        double cornerTime, oppositeTime;
        QPointF risingStroke;
    };
    for (const auto& example : {StackedWord{315,9,8,40,2.61523,{126.3,299.8}},
                                StackedWord{131,10,5,40,1.75399,{167.7,326.2}},
                                StackedWord{479,13,10,41,1.75404,{100.8,407.25}}}) {
        QFile source(QString("pages/page%1.dat").arg(example.page));
        check(source.open(QIODevice::ReadOnly), "Missing stacked yeh–hah fixture");
        QDataStream input(&source);
        PageAnalysisResult sample;
        input >> sample.page;
        auto& word = sample.page.lines[example.line-1].words[example.word-1];
        const auto& sub = word.wordResultInfo.subWords[0];
        check(sample.segmentSubword(example.page-1,example.line-1,example.word-1,0,nullptr,true) &&
              sub.baseGlyphs.size() == 4, "Stacked yeh–hah fixture became partial");
        const auto& entry = *sub.joins.begin();
        check(entry.bottomBaseLine.time == example.cornerTime && std::abs(entry.topBaseLine.time - example.oppositeTime) < .0001 &&
              std::abs(entry.topBaseLine.point.y() - entry.bottomBaseLine.point.y()) < 1e-7 &&
              entry.topBaseLine.point.x() < entry.bottomBaseLine.point.x() && sub.kashidaPaths[0].isEmpty(),
              "The shared cut must extend horizontally left from the attachment corner");
        const auto shape = word.bodyShape(sub);
        const auto yeh = shape.transform.map(sub.baseGlyphs[0].path).translated(shape.pos);
        check(std::abs(yeh.boundingRect().bottom() - entry.bottomBaseLine.point.y()) < 1e-6 &&
              yeh.contains(example.risingStroke), "The rising stroke above the attachment must stay in the yeh body");
    }
    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 cache is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page cache");

    for (const auto& location : {std::array{13, 3, 1, 24}, std::array{5, 1, 1, 20}, std::array{5, 6, 1, 21}}) {
        const int li = location[0] - 1, wi = location[1] - 1, si = location[2] - 1;
        auto& word = analysis.page.lines[li].words[wi];
        auto& sub = word.wordResultInfo.subWords[si];
        const auto& shape = word.paths[sub.paths.front()];
        auto source = shape.transform.map(shape.path);
        source.translate(shape.pos);
        WordResultInfo::SubWordInfo dummy;
        auto context = geometry(source, dummy);
        const auto pairs = contourBodyBoundaries(context, pairedRouteSettings(hahRule));
        check(pairs.size() == 1, "Expected one stacked-hah body");
        const auto& cut = pairs.front().first;
        check(cut.bottomBaseLine.time == location[3] && cut.bottomBaseLine.isSharp &&
              cut.topBaseLine.point.x() < cut.bottomBaseLine.point.x() &&
              std::abs(cut.topBaseLine.point.y() - cut.bottomBaseLine.point.y()) < 1e-7,
              "Ligature cut did not extend left from its inward corner");

        Stretching unrelated{};
        unrelated.topBaseLine.point = {-10, -100};
        unrelated.bottomBaseLine.point = {500, 200};
        dummy.joins.insert(unrelated);
        context.pairedStrokeWidth = 1000;
        context.baseText = QStringLiteral("اختبار");
        const auto independent = contourBodyBoundaries(context, pairedRouteSettings(hahRule));
        check(independent.size() == 1 && same(cut, independent.front().first),
              "Neighboring landmarks changed the ligature cut");
        for (double scale : {.7, 1.6}) {
            const QTransform mapping(scale, 0, 0, scale, 37, -21);
            auto scaled = geometry(mapping.map(source), dummy);
            const auto result = contourBodyBoundaries(scaled, pairedRouteSettings(hahRule));
            check(result.size() == 1 && same(cut, result.front().first, mapping),
                  "Ligature cut is not scale/translation invariant");
        }
        check(analysis.segmentSubword(2, li, wi, si, nullptr, true), "Ligature became partial");
        const auto& shared = *sub.joins.begin();
        check(sub.baseGlyphs.size() == 3 && sub.kashidaPaths.size() == 2 &&
              sub.kashidaPaths[0].isEmpty() && !sub.kashidaPaths[1].isEmpty() && same(cut, shared) &&
              shared.topRight.point == shared.topBaseLine.point && shared.bottomRight.point == shared.bottomBaseLine.point,
              "The ligature retained a kashida or stale displayed boundary");

        if (location[0] != 13) continue;
        check(sub.matchedTemplates.contains("noon-fina"), "Final noon form was not used");
        auto noonContext = geometry(source, dummy);
        auto body = noonRule.value("body").toObject();
        auto settings = body.value("bowl").toObject();
        const auto bowl = recognizeOpenBowl(inkOutline(noonContext), settings);
        check(bowl && bowl->shoulder.time == 18, "Noon did not find its own shoulder at corner 18");
        const auto& last = *sub.joins.rbegin();
        check(QLineF(last.bottomLeft.point, bowl->shoulder.point).length() < 1e-4,
              "Noon cut was displaced from the bowl shoulder");
        check(declaredSharedBoundary(noonContext, noonRule), "The bowl shoulder did not produce a cut");
        const auto noonCut = noonContext.res;
        settings.insert("minCornerAngle", 179.9);
        body.insert("bowl", settings);
        auto withoutShoulder = noonRule;
        withoutShoulder.insert("body", body);
        check(!declaredSharedBoundary(noonContext, withoutShoulder) &&
              !matchesBoundaryBody(noonContext, withoutShoulder, noonCut, false),
              "Noon accepted a cut without recognizing a bowl shoulder");
        const auto noon = shape.transform.map(sub.baseGlyphs.back().path).boundingRect();
        const auto ink = shape.transform.map(shape.path).boundingRect();
        check(std::abs(noon.left() - ink.left()) < 1e-6 && std::abs(noon.bottom() - ink.bottom()) < 1e-6,
              "Noon lost part of its bowl");
    }
    qInfo("Beh-hah ligature passed: shared cuts at corners 24/20/21, no intervening kashida, independent geometry, scale/translation invariance and required noon shoulder ownership.");
}
