// Exercise the private geometry detector without the segmentation dispatcher
// or any neighboring letter templates. The test runner omits its normal object.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
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
    check(context.timeSorted.size() >= 3, "Missing source outline");
    context.setOrientation();
    return context;
}

static std::vector<std::pair<Stretching, Stretching>> hehCuts(SearchContext& context, QJsonObject settings) {
    QJsonObject policy{{"normalCut", settings.take("normalCut")}};
    const auto evidence = recognizeStackedPairs(inkOutline(context), settings);
    return cutStackedPairs(context, evidence, policy);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject settings;
    for (const auto& value : config.value("medial").toArray())
        if (value.toObject().value("id") == "heh-body-medi") settings = shapeSettings(value.toObject(), "pair");
    check(!settings.isEmpty(), "Missing intrinsic heh corner settings");
    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 cache is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page cache");
    auto& word = analysis.page.lines[0].words[5];
    auto& subword = word.wordResultInfo.subWords[4];
    const auto& shape = word.paths[subword.paths.front()];
    auto source = shape.transform.map(shape.path);
    source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(source, dummy);
    const auto pairs = hehCuts(context, settings);
    check(pairs.size() == 1, "Expected one heh counter pair");
    const auto& [right, left] = pairs.front();
    check(right.bottomBaseLine.time == 21 && left.bottomBaseLine.time == 17,
          "Heh did not keep the outer corners 21 and 17");
    const auto curves = baseContourCurves(source);
    const auto normalTo = [](const Stretching& cut, const QPointF& tangent) {
        const auto direction = cut.topBaseLine.point - cut.bottomBaseLine.point;
        return std::abs(QPointF::dotProduct(direction, tangent)) <
            1e-7 * std::hypot(direction.x(), direction.y()) * std::hypot(tangent.x(), tangent.y());
    };
    check(normalTo(right, curves[21].pt2() - curves[21].pt1()) &&
          normalTo(left, curves[16].pt3() - curves[16].pt4()), "Cuts are not normal to the outgoing stroke branches");
    const auto same = [](const Stretching& expected, const Stretching& actual, const QTransform& mapping = {}) {
        return QLineF(mapping.map(expected.topBaseLine.point), actual.topBaseLine.point).length() < 1e-4 &&
            QLineF(mapping.map(expected.bottomBaseLine.point), actual.bottomBaseLine.point).length() < 1e-4;
    };
    // Deliberately unrelated join positions, widths and transcript must have
    // no effect on geometry proposed by the heh itself.
    Stretching unrelated{};
    unrelated.topBaseLine.point = {-100, 200};
    unrelated.bottomBaseLine.point = {-80, 400};
    unrelated.referenceWidth = 1000;
    dummy.joins.insert(unrelated);
    context.baseText = QStringLiteral("اختبار");
    context.pairedStrokeWidth = 1000;
    const auto independent = hehCuts(context, settings);
    check(independent.size() == 1 && same(right, independent[0].first) && same(left, independent[0].second),
          "Neighboring landmarks or text changed the heh proposal");
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy);
        const auto proposed = hehCuts(scaled, settings);
        check(proposed.size() == 1 && same(right, proposed[0].first, mapping) && same(left, proposed[0].second, mapping),
              "Heh proposal is not scale/translation invariant");
    }
    // With only the outer outline there is no two-counter heh to recognize.
    auto solid = geometry(context.subPaths.front(), dummy);
    check(hehCuts(solid, settings).empty(), "Missing counters triggered a generic cut");

    const auto verifyAnchors = [&] {
        check(subword.matchedTemplates.contains("heh-body-medi"), "Integrated heh template was not matched");
        const std::vector<Stretching> joins(subword.joins.begin(), subword.joins.end());
        check(joins.size() == 2 && joins[0].bottomLeft.time == 21 && joins[1].bottomBaseLine.time == 17,
              "Dispatcher changed the heh corner anchors");
    };
    check(analysis.segmentSubword(2, 0, 5, 4, nullptr, true), "Ta-heh-meem is still partial");
    verifyAnchors();
    check(subword.baseGlyphs.size() == 3 && subword.kashidaPaths.size() == 2 &&
          !subword.kashidaPaths[0].isEmpty() && !subword.kashidaPaths[1].isEmpty(), "Expected three bodies and two kashidas");
    auto withoutFinal = config;
    withoutFinal.insert("final", QJsonArray{});
    analysis.segmentSubword(2, 0, 5, 4, nullptr, true, &withoutFinal);
    verifyAnchors();

    // The outer foot of the heh in muhtadi leans inward by about 5.4 degrees.
    // It must stay with the body, rather than selecting the closer inner corner.
    auto& targetWord = analysis.page.lines[14].words[6];
    auto& target = targetWord.wordResultInfo.subWords[0];
    const auto& targetShape = targetWord.paths[target.paths.front()];
    auto targetInk = targetShape.transform.map(targetShape.path);
    targetInk.translate(targetShape.pos);
    auto targetContext = geometry(targetInk, dummy);
    const auto targetPairs = hehCuts(targetContext, settings);
    check(targetPairs.size() == 1, "Expected one heh counter pair in muhtadi");
    const auto& [targetRight, targetLeft] = targetPairs.front();
    check(targetRight.bottomBaseLine.time == 16 && targetLeft.bottomBaseLine.time == 10,
          "Muhtadi heh did not retain its outer step at corner 10");
    const auto targetCurves = baseContourCurves(targetInk);
    check(normalTo(targetLeft, targetCurves[9].pt3() - targetCurves[9].pt4()),
          "Corner 10 cut is not normal to the outgoing branch");
    auto strict = settings;
    strict.insert("maxFootInwardAngle", 0);
    const auto strictPairs = hehCuts(targetContext, strict);
    check(strictPairs.size() == 1 && strictPairs[0].second.bottomBaseLine.time == 13,
          "Zero inward-angle tolerance did not reproduce the previous inner-corner selection");
    targetContext.baseText = QStringLiteral("اختبار");
    targetContext.pairedStrokeWidth = 1000;
    const auto targetIndependent = hehCuts(targetContext, settings);
    check(targetIndependent.size() == 1 && same(targetRight, targetIndependent[0].first) &&
          same(targetLeft, targetIndependent[0].second), "Corner 10 depends on neighboring landmarks or text");
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(targetInk), dummy);
        const auto proposed = hehCuts(scaled, settings);
        check(proposed.size() == 1 && same(targetRight, proposed[0].first, mapping) &&
              same(targetLeft, proposed[0].second, mapping), "Stepped heh is not scale/translation invariant");
    }
    check(analysis.segmentSubword(2, 14, 6, 0, nullptr, true), "Muhtadi became partial");
    const std::vector<Stretching> targetJoins(target.joins.begin(), target.joins.end());
    check(targetJoins.size() == 3 && targetJoins[0].bottomLeft.time == 16 &&
          targetJoins[1].bottomBaseLine.time == 10, "Dispatcher moved the heh step away from corner 10");
    check(target.baseGlyphs.size() == 4 && target.kashidaPaths.size() == 3 &&
          std::all_of(target.kashidaPaths.begin(), target.kashidaPaths.end(), [](const auto& p) { return !p.isEmpty(); }),
          "Muhtadi should retain four letter bodies and three kashida spans");

    // In bihim both corners satisfy the angle tolerance. The outer step must
    // still win, even though the inner attachment is nearer the lower counter.
    auto& stepWord = analysis.page.lines[5].words[4];
    auto& stepSubword = stepWord.wordResultInfo.subWords[1];
    const auto& stepShape = stepWord.paths[stepSubword.paths.front()];
    auto stepInk = stepShape.transform.map(stepShape.path);
    stepInk.translate(stepShape.pos);
    auto stepContext = geometry(stepInk, dummy);
    const auto stepPairs = hehCuts(stepContext, settings);
    check(stepPairs.size() == 1 && stepPairs[0].second.bottomBaseLine.time == 22,
          "The inner attachment 24 displaced the outer foot 22");
    check(analysis.segmentSubword(2, 5, 4, 1, nullptr, true), "Bihim became partial");
    check(std::next(stepSubword.joins.begin())->bottomBaseLine.time == 22,
          "Dispatcher dropped the outer foot of bihim");
    qInfo("Medial heh passed: corners 17/21 and 10/16, outer-step priority over inner corner 24, configurable step-angle tolerance, stroke normals, independent landmarks/text, affine invariance, missing-counter abstention and operation without a final template.");
}
