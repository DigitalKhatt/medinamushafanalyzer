#include "../detail.h"

namespace MedinaCutter {

InkOutline inkOutline(const SearchContext &context) {
    return {context.transformedPath, context.xSorted, context.ySorted, context.timeSorted,
            context.subPaths, context.bbox, context.orientation};
}

// Cache source landmarks with corner-preserving deduplication.
bool preparePassOutline(SearchContext& context, OutlineSearchCache& cache, bool completeClosedOutline) {
    if (!cache.outline || cache.outline->ink != context.transformedPath) {
        cache.routeCuts.clear();
        cache.normalsByCenter.clear();
        auto prepared = std::make_shared<PreparedPassOutline>();
        prepared->ink = context.transformedPath;
        QVector<PathExtrema> raw;
        readContourLandmarks(prepared->ink, prepared->contours, raw, completeClosedOutline);
        for (const auto& point : raw) {
            auto existing = std::find_if(prepared->extrema.begin(), prepared->extrema.end(), [&](const auto& other) {
                const double dx = std::abs(other.point.x() - point.point.x());
                const double dy = std::abs(other.point.y() - point.point.y());
                return other.pathIndex == point.pathIndex && dx < .01 && dy < .01 &&
                    (other.isSharp == point.isSharp || (dx < 1e-5 && dy < 1e-5));
            });
            if (existing == prepared->extrema.end()) prepared->extrema.append(point);
            else if (point.isSharp && !existing->isSharp) *existing = point;
        }
        for (const auto& point : prepared->extrema) if (point.pathIndex == 0) {
            prepared->time.insert(point);
            prepared->x.insert(point);
            prepared->y.insert(point);
        }
        if (prepared->time.size() < 3) return false;
        context.subPaths = prepared->contours;
        context.timeSorted = prepared->time;
        context.xSorted = prepared->x;
        context.ySorted = prepared->y;
        context.setOrientation();
        prepared->orientation = context.orientation;
        cache.outline = std::move(prepared);
    } else {
        context.subPaths = cache.outline->contours;
        context.timeSorted = cache.outline->time;
        context.xSorted = cache.outline->x;
        context.ySorted = cache.outline->y;
        context.orientation = cache.outline->orientation;
    }
    context.subword.extrema = cache.outline->extrema;
    return true;
}

bool leavesEntryClearance(const Stretching& sending, const Stretching& receiving,
                                 Orientation orientation, double minimumGap) {
    if (!cutPrecedes(sending, receiving, orientation)) return false;
    const auto start = sectionLine(sending), end = sectionLine(receiving);
    if (start.isNull() || end.isNull() || start.intersects(end, nullptr) == QLineF::BoundedIntersection)
        return false;
    // The sending cap can run along a sloping stroke. Measure the gap between
    // the finite cuts so the direction of that cap cannot hide a valid join.
    const auto distance = [](const QPointF& point, const QLineF& line) {
        const auto axis = line.p2() - line.p1();
        const double position = QPointF::dotProduct(point - line.p1(), axis) / QPointF::dotProduct(axis, axis);
        return QLineF(point, line.pointAt(std::clamp(position, 0., 1.))).length();
    };
    return std::min({distance(start.p1(), end), distance(start.p2(), end),
                     distance(end.p1(), start), distance(end.p2(), start)}) > minimumGap;
}

} // namespace MedinaCutter
