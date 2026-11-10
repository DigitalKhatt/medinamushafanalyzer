// Retry caches must follow the source geometry and every edited rule setting.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) {
    if (!ok) qFatal("%s", message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    QPainterPath ink;
    ink.moveTo(0, 0);
    ink.cubicTo(3, -4, 7, -4, 10, 0);
    ink.lineTo(10, 2);
    ink.cubicTo(7, -1, 3, -1, 0, 2);
    ink.closeSubpath();
    OutlineSearchCache cache;
    WordResultInfo::SubWordInfo first, retry, transformed;
    SearchContext a{.transformedPath = ink, .subword = first};
    check(preparePassOutline(a, cache), "Initial outline preparation failed");
    const auto original = cache.outline;
    first.extrema.clear();
    a.timeSorted.clear();
    SearchContext b{.transformedPath = ink, .subword = retry, .baseText = QStringLiteral("different slots")};
    check(preparePassOutline(b, cache), "Cached outline preparation failed");
    check(cache.outline == original && !b.timeSorted.empty() && !retry.extrema.empty(),
          "Retry mutated or rebuilt the cached source landmarks");
    const auto moved = QTransform::fromTranslate(2, 3).map(ink);
    SearchContext c{.transformedPath = moved, .subword = transformed};
    check(preparePassOutline(c, cache) && cache.outline != original,
          "Changed trial geometry reused the previous landmarks");
    check(cache.outline->ink == moved && original->ink == ink,
          "Preparing a trial changed the saved original outline");
    check(transformed.extrema.size() == retry.extrema.size(), "Translation changed the landmark count");
    for (int i = 0; i < retry.extrema.size(); ++i)
        check(QLineF(transformed.extrema[i].point, retry.extrema[i].point + QPointF(2, 3)).length() < 1e-8,
              "Trial landmarks remained in the previous coordinate frame");

    auto config = PageAnalysisResult::letterForms();
    auto initials = config.value("initial").toArray();
    auto rule = initials.first().toObject();
    const auto id = rule.value("id").toString();
    rule.insert("onPartial", true);
    rule.insert("tryNext", false);
    rule.insert("retryWithVariants", false);
    initials[0] = rule;
    config.insert("initial", initials);
    const auto hasBoundary = [&](const PreparedPassRules& rules) {
        return std::any_of(rules.boundaries.begin(), rules.boundaries.end(), [&](const auto& v) {
            return v.toObject().value("id") == id;
        });
    };
    const auto& primary = preparePassRules(config, false, cache);
    const auto& recovery = preparePassRules(config, true, cache);
    check(!hasBoundary(primary) && hasBoundary(recovery), "Retry eligibility leaked between pass modes");
    check(&preparePassRules(config, false, cache) == &primary, "Unchanged rules were rebuilt");
    rule.insert("retryWithVariants", true);
    initials[0] = rule;
    auto edited = config;
    edited.insert("initial", initials);
    const auto& changed = preparePassRules(edited, true, cache);
    const auto boundary = std::find_if(changed.boundaries.begin(), changed.boundaries.end(), [&](const auto& v) {
        return v.toObject().value("id") == id;
    });
    check(boundary != changed.boundaries.end() && boundary->toObject().value("tryNext").toBool(),
          "Edited retry settings reused an obsolete prepared rule");
    check(&preparePassRules(config, true, cache) == &recovery,
          "A nested edited configuration invalidated the caller's rules");
    check(cache.outline->ink == moved, "Rule preparation changed the source geometry");

    check(!cutGeometryMemo, "Geometry memo leaked from a previous subword");
    {
        CutGeometryMemoScope memo;
        QPainterPath square;
        square.addRect(0, 0, 10, 10);
        Stretching cut{};
        cut.topBaseLine = {.pathIndex = 0, .time = .5, .point = {5, 0}};
        cut.bottomBaseLine = {.pathIndex = 0, .time = 2.5, .point = {5, 10}};
        const auto verify = [&](const QPainterPath& source, const Stretching& seam, Orientation orientation,
                                bool expanded, const QString& text) {
            const auto expected = computeCutRegions(source, text, {seam}, orientation, expanded);
            const auto actual = cutRegions(source, text, {seam}, orientation, expanded);
            check(actual.size() == expected.size(), "Cached partition changed the region count");
            for (size_t i = 0; i < actual.size(); ++i)
                check(actual[i].path == expected[i].path && actual[i].text == text,
                      "Cached partition changed curves or retained another caller's text");
        };
        verify(square, cut, Orientation::ClockWise, false, "first");
        verify(square, cut, Orientation::ClockWise, false, "different text");
        check(cutGeometryMemo->partitionHits == 1, "Identical partition was not reused");
        verify(square, cut, Orientation::CounterClockwise, false, "reverse");
        verify(square, cut, Orientation::ClockWise, true, "expanded");
        verify(square.translated(2, 3), cut, Orientation::ClockWise, false, "translated");
        auto shifted = cut;
        shifted.topBaseLine.time += .01;
        shifted.topBaseLine.point.rx() += .1;
        verify(square, shifted, Orientation::ClockWise, false, "shifted cut");
        auto holed = square;
        holed.addRect(3, 3, 4, 4);
        auto bridge = cut;
        bridge.counterEntry = {.pathIndex = 1, .time = .5, .point = {5, 3}};
        bridge.counterExit = {.pathIndex = 1, .time = 2.5, .point = {5, 7}};
        verify(holed, bridge, Orientation::ClockWise, true, "counter");
        bridge.counterBendWeights = {.25, .5};
        verify(holed, bridge, Orientation::ClockWise, true, "bent bridge");
        holed.setFillRule(Qt::WindingFill);
        verify(holed, bridge, Orientation::ClockWise, true, "fill rule");
        for (const auto& source : {square, square.translated(2, 3)})
            for (const auto& top : {QPointF(0, 5), QPointF(-.1, 5), QPointF(0, 15)})
                for (double tolerance : {0., .2}) {
                    const QPointF bottom(10, 5);
                    const bool expected = computeInteriorInkSegment(source, top, bottom, tolerance);
                    check(interiorInkSegment(source, top, bottom, tolerance) == expected &&
                          interiorInkSegment(source, top, bottom, tolerance) == expected,
                          "Cached ink check changed with geometry or endpoint tolerance");
                }
        check(cutGeometryMemo->segmentHits >= 12, "Repeated ink checks were not reused");
        const auto active = cutGeometryMemo;
        { CutGeometryMemoScope nested; check(cutGeometryMemo == active, "Recursive trial lost the parent memo"); }
        check(cutGeometryMemo == active, "Recursive trial destroyed the parent memo");
    }
    check(!cutGeometryMemo, "Completed subword retained its geometry memo");
}
