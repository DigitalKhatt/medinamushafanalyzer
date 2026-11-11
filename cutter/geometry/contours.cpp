#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

JoinTime getJoinTime(Stretching join) {
	JoinTime joinTime;

	if (join.topBaseLine.time > join.bottomBaseLine.time) {
		joinTime.minTime = join.bottomBaseLine.time;
		joinTime.maxTime = join.topBaseLine.time;
		joinTime.startAtLeft = !join.clockWise;
	}
	else {
		joinTime.maxTime = join.bottomBaseLine.time;
		joinTime.minTime = join.topBaseLine.time;
		joinTime.startAtLeft = join.clockWise;
	}

	return joinTime;
}

JoinTime getJoinTime(Stretching join, Orientation pathOrient) {
	JoinTime joinTime;

	if (join.topBaseLine.time > join.bottomBaseLine.time) {
		joinTime.minTime = join.bottomBaseLine.time;
		joinTime.maxTime = join.topBaseLine.time;
		joinTime.startAtLeft = pathOrient != Orientation::ClockWise;
	}
	else {
		joinTime.maxTime = join.bottomBaseLine.time;
		joinTime.minTime = join.topBaseLine.time;
		joinTime.startAtLeft = pathOrient == Orientation::ClockWise;
	}

	return joinTime;
}

bool sharesLowerForkTip(const Stretching& right, const Stretching& left) {
    const auto& tip = right.bottomBaseLine;
    return right.counterEntry.pathIndex <= 0 && left.counterEntry.pathIndex <= 0 &&
        tip.pathIndex == 0 && left.bottomBaseLine.pathIndex == 0 &&
        tip.time == left.bottomBaseLine.time && tip.point == left.bottomBaseLine.point &&
        right.topBaseLine.point.x() > tip.point.x() && left.topBaseLine.point.x() < tip.point.x() &&
        right.topBaseLine.point.y() < tip.point.y() && left.topBaseLine.point.y() < tip.point.y();
}

bool cutPrecedes(const Stretching& right, const Stretching& left, Orientation orientation) {
    const auto side = getJoinTime(right, orientation);
    return side.isStrictLeft(left.topBaseLine.time) &&
        (side.isStrictLeft(left.bottomBaseLine.time) || sharesLowerForkTip(right, left));
}

JoinTime getJoinTime(SearchContext& context) {
	JoinTime joinTime;

	if (context.subword.joins.size() > 0) {
		auto& lastJoin = *context.subword.joins.rbegin();
		joinTime = getJoinTime(lastJoin);
	}

	return joinTime;
}

// Walk the matched contour step by arc length, retaining the curve parameter
// so the resulting point can be used by the vector cutter.
std::vector<QBezier> baseContourCurves(const QPainterPath &path) {
    std::vector<QBezier> curves;
    for (int i = 1; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        if (element.isMoveTo())
            break;
        const QPointF from = path.elementAt(i - 1);
        if (element.isCurveTo()) {
            curves.push_back(QBezier::fromPoints(from, element, path.elementAt(i + 1), path.elementAt(i + 2)));
            i += 2;
        } else if (element.isLineTo()) {
            const QLineF line(from, element);
            curves.push_back(QBezier::fromPoints(from, line.pointAt(1.0 / 3), line.pointAt(2.0 / 3), element));
        }
    }

    return curves;
}

// Project from a known contour landmark using the tangent on the matched
// step's side. The landmark stays fixed, including at sharp corners.
bool contourNormalCut(const SearchContext& context, const PathExtrema& anchor,
                      bool increasingTime, bool atStepEnd, Stretching& cut, bool aboveOnly,
                      double tangentSample) {
    const auto curves = baseContourCurves(context.transformedPath);
    if (curves.empty() || anchor.pathIndex != 0) return false;
    const int count = static_cast<int>(curves.size());
    double time = anchor.time;
    if (std::abs(time - std::round(time)) < 1e-8) time = std::round(time);
    int index = static_cast<int>(std::floor(time));
    double t = time - index;
    if (increasingTime == atStepEnd && t == 0) { --index; t = 1; }
    const auto& curve = curves[(index % count + count) % count];
    const double u = 1 - t;
    auto tangent = u * u * (curve.pt2() - curve.pt1()) +
        2 * u * t * (curve.pt3() - curve.pt2()) + t * t * (curve.pt4() - curve.pt3());
    if (tangentSample > 0) {
        auto limit = anchor;
        const bool forward = increasingTime != atStepEnd;
        limit.time += forward ? count : -count;
        PathExtrema sampled;
        QPointF direction;
        if (advanceOnContour(context.transformedPath, anchor, limit, forward, tangentSample, sampled, direction))
            tangent = sampled.point - anchor.point;
    }
    if (std::hypot(tangent.x(), tangent.y()) < 1e-9)
        tangent = curve.pointAt(std::min(1.0, t + 1e-4)) - curve.pointAt(std::max(0.0, t - 1e-4));
    const double length = std::hypot(tangent.x(), tangent.y());
    if (length < 1e-9) return false;
    const QPointF normal(tangent.y() / length, -tangent.x() / length);
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    double nearest = reach;
    bool found = false;
    for (double side : {-1.0, 1.0}) {
        const auto hits = FindInterections(context.transformedPath,
            QLineF(anchor.point, anchor.point + side * reach * normal), true);
        for (const auto& hit : hits) {
            const double distance = QLineF(anchor.point, hit.point).length();
            if ((aboveOnly && hit.point.y() >= anchor.point.y()) || distance <= 1e-5 || distance >= nearest ||
                !interiorChord(context.transformedPath, anchor, hit)) continue;
            cut.topBaseLine = anchor;
            cut.bottomBaseLine = hit;
            cut.referenceWidth = distance;
            nearest = distance;
            found = true;
        }
    }
    return found;
}

bool advanceOnContour(const QPainterPath& path, const PathExtrema& start,
                             const PathExtrema& end, bool increasingTime,
                             double distance, PathExtrema& point, QPointF& tangent) {
    if (start.pathIndex != 0 || end.pathIndex != 0 || distance <= 0 ||
        (increasingTime ? end.time <= start.time : end.time >= start.time)) return false;

    const auto curves = baseContourCurves(path);

    const double low = std::min(start.time, end.time);
    const double high = std::max(start.time, end.time);
    const int first = static_cast<int>(std::floor(low));
    const int last = static_cast<int>(std::ceil(high)) - 1;
    const int count = static_cast<int>(curves.size());
    if (!count || high - low > count + 1e-8) return false;

    const int direction = increasingTime ? 1 : -1;
    for (int i = increasingTime ? first : last; i >= first && i <= last; i += direction) {
        const double a = std::max(0.0, low - i);
        const double b = std::min(1.0, high - i);
        const int index = (i % count + count) % count;
        const auto& curve = curves[index];
        const double length = curve.getSubRange(a, b).length(0.001);
        if (distance > length) {
            distance -= length;
            continue;
        }
        double lo = a, hi = b;
        for (int iteration = 0; iteration < 24; ++iteration) {
            const double t = (lo + hi) / 2;
            const double traversed = (increasingTime ? curve.getSubRange(a, t) : curve.getSubRange(t, b)).length(0.001);
            if ((traversed < distance) == increasingTime) lo = t;
            else hi = t;
        }
        const double t = (lo + hi) / 2;
        point = {0, index + t, curve.pointAt(t), false, false};
        const double u = 1 - t;
        tangent = u * u * (curve.pt2() - curve.pt1())
                + 2 * u * t * (curve.pt3() - curve.pt2())
                + t * t * (curve.pt4() - curve.pt3());
        return true;
    }
    return false;
}

// Find where the descending outer edge turns toward the right, then cut
// along the inward normal at that point.
bool alefTurnCut(const SearchContext& context, const PathExtrema& limit,
                        double angle, double stroke, Stretching& cut,
                        const PathExtrema* crest, int traversal, bool oppositeFlank) {
    const auto start = crest ? crest->time : std::prev(context.xSorted.end())->time;
    const auto curves = baseContourCurves(context.transformedPath);
    if (curves.empty()) return false;
    const int curveCount = static_cast<int>(curves.size());
    const int direction = traversal ? traversal : limit.time > start ? 1 : -1;
    double finish = limit.time;
    // A stem flank may cross the arbitrary start of the closed SVG contour.
    if (direction > 0 && finish < start) finish += curveCount;
    if (direction < 0 && finish > start) finish -= curveCount;
    const double low = std::min(start, finish);
    const double high = std::max(start, finish);
    const int first = static_cast<int>(std::floor(low));
    const int last = static_cast<int>(std::ceil(high)) - 1;
    if (high - low > curveCount) return false;

    const double radians = angle * M_PI / 180;
    const QPointF target(std::cos(radians), std::sin(radians));
    for (int i = direction > 0 ? first : last; i >= first && i <= last; i += direction) {
        const int curveIndex = (i % curveCount + curveCount) % curveCount;
        const auto& curve = curves[curveIndex];
        const auto tangent = [&](double t) {
            const double u = 1 - t;
            return direction * (u * u * (curve.pt2() - curve.pt1())
                   + 2 * u * t * (curve.pt3() - curve.pt2())
                   + t * t * (curve.pt4() - curve.pt3()));
        };
        const auto offset = [&](double t) {
            const auto v = tangent(t);
            return target.x() * v.y() - target.y() * v.x();
        };
        const double a = std::max(0.0, low - i);
        const double b = std::min(1.0, high - i);
        const double begin = direction > 0 ? a : b;
        const double end = direction > 0 ? b : a;
        double previous = begin;
        for (int sample = 1; sample <= 24; ++sample) {
            const double current = begin + (end - begin) * sample / 24;
            if (offset(previous) > 0 && offset(current) <= 0) {
                double steep = previous, shallow = current;
                for (int iteration = 0; iteration < 24; ++iteration) {
                    const double middle = (steep + shallow) / 2;
                    if (offset(middle) > 0) steep = middle;
                    else shallow = middle;
                }
                const double t = (steep + shallow) / 2;
                const auto v = tangent(t);
                const double length = std::hypot(v.x(), v.y());
                if (v.x() > 0 && v.y() > 0 && length > 1e-8) {
                    const PathExtrema outer{0, curveIndex + t, curve.pointAt(t), false, false};
                    const bool invertNormal = (crest != nullptr) != oppositeFlank;
                    const QPointF normal = (invertNormal ? -1 : 1) * QPointF(v.y() / length, -v.x() / length);
                    const auto hits = FindInterections(context.transformedPath,
                        QLineF(outer.point, outer.point + normal * (3 * stroke)), true);
                    for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit) {
                        const auto& top = crest ? outer : *hit;
                        const auto& bottom = crest ? *hit : outer;
                        if (!interiorChord(context.transformedPath, top, bottom)) continue;
                        cut.topBaseLine = top;
                        cut.bottomBaseLine = bottom;
                        return true;
                    }
                }
            }
            previous = current;
        }
    }
    return false;
}

static bool walkContourSteps(SearchContext& context, timesortedtype::iterator origin,
        const QJsonArray& steps, ContourAdvance advance,
        PathExtrema& anchor, PathExtrema& before, PathExtrema& end) {
    const auto matchesLandmark = [&](const QJsonObject& features, const PathExtrema& point) {
        for (const auto& [name, sign] : {std::pair{"xMinimum", 1.}, std::pair{"xMaximum", -1.}}) {
            if (!features.contains(name)) continue;
            const auto at = context.timeSorted.find(point);
            if (at == context.timeSorted.end()) return false;
            const double x = sign * point.point.x();
            const double before = sign * context.prev(at)->point.x(), after = sign * context.next(at)->point.x();
            // A zero Bezier derivative can be reported as an extremum without
            // reversing direction. Check both flanks to identify a true turn.
            const bool minimum = x <= before + 1e-8 && x <= after + 1e-8 &&
                (x < before - 1e-8 || x < after - 1e-8);
            if (minimum != features.value(name).toBool()) return false;
        }
        return (!features.contains("sharp") || features.value("sharp").toBool() == point.isSharp) &&
            (!features.contains("xExtremum") || features.value("xExtremum").toBool() == point.isX) &&
            (!features.contains("yExtremum") || features.value("yExtremum").toBool() == point.isY);
    };

    if (steps.isEmpty()) return false;
    auto current = origin;
    int remaining = static_cast<int>(context.timeSorted.size());
    for (int index = 0; index < steps.size(); ++index) {
        auto bounds = steps[index].toObject();
        if (steps[index].isString()) bounds.insert("direction", steps[index]);
        const bool accumulate = bounds.value("accumulate").toBool();
        const QJsonObject direction{{"direction", bounds.value("direction")}};
        const auto stopAt = bounds.value("stopAt").toObject();
        auto runStart = current;
        bool found = false;
        while (remaining-- > 0) {
            const auto next = (context.*advance)(current);
            const auto delta = next->point - current->point;
            const bool continues = !accumulate || matchesOffset(direction, delta);
            if (!continues) runStart = next;
            // Extrema can subdivide one continuous flank. Measure cumulative
            // bounds over that run without stepping across a direction reversal.
            const bool matchesRun = continues && matchesLandmark(bounds.value("start").toObject(), *current) &&
                matchesOffset(bounds, accumulate ? next->point - runStart->point : delta);
            if (matchesRun && matchesLandmark(bounds.value("end").toObject(), *next)) {
                if (index == steps.size() - 1) {
                    anchor = *current;
                    end = *next;
                }
                if (index != steps.size() - 1) before = *current;
                current = next;
                found = true;
                break;
            }
            if (matchesRun && !stopAt.isEmpty() && matchesLandmark(stopAt, *next)) return false;
            // A required adjacent flank must start at the matched turn.
            if (bounds.value("immediate").toBool()) return false;
            before = *current;
            current = next;
        }
        if (!found) return false;
    }
    return true;
}

std::optional<ContourRouteEvidence> recognizeContourRoute(SearchContext& context,
        const QJsonObject& form, const PathExtrema* crest, bool reverseTop) {
    const auto start = form.value("start").toString() == "left" ?
        std::prev(context.xSorted.end()) : context.xSorted.begin();
    const auto origin = context.timeSorted.find(crest ? *crest :
        form.value("start") == "top" ? *std::prev(context.ySorted.end()) : *start);
    auto forward = context.orientation == Orientation::ClockWise ? &SearchContext::prev : &SearchContext::next;
    if (form.value("traversal").toString() == "down") {
        forward = context.prev(origin)->point.y() < origin->point.y() ? &SearchContext::next : &SearchContext::prev;
    }
    const auto backward = forward == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
    Stretching join{};
    PathExtrema topEnd{}, topBefore{};
    const auto topDirection = (form.value("topTraversal") == "forward") != reverseTop ? forward : backward;
    if (form.value("topAtAnchor").toBool()) join.topBaseLine = topEnd = *origin;
    else if (!walkContourSteps(context, origin, form.value("top").toArray(), topDirection, join.topBaseLine, topBefore, topEnd)) return {};
    if (form.value("topUseStepEnd").toBool()) join.topBaseLine = topEnd;
    const auto snap = form.value("snapCurveStart");
    auto& anchor = join.topBaseLine;
    const int curve = static_cast<int>(std::floor(anchor.time));
    if (snap.isDouble() && curve >= 0 && anchor.time - curve <= snap.toDouble()) {
        int index = 0;
        const auto& path = context.transformedPath;
        for (int element = 1; element < path.elementCount(); ++element) {
            const auto current = path.elementAt(element);
            if (current.isMoveTo()) return {};
            if (index++ == curve) {
                anchor.time = curve;
                anchor.point = path.elementAt(element - 1);
                break;
            }
            if (current.isCurveTo()) element += 2;
        }
        if (anchor.time != curve) return {};
        join.topEndpointTolerance = form.value("snapEndpointTolerance").toDouble();
    }
    return ContourRouteEvidence{start, origin, forward, topDirection,
        join.topBaseLine, topBefore, topEnd, join.topEndpointTolerance};
}

bool projectContourRoute(SearchContext& context, const QJsonObject& form,
        const ContourRouteEvidence& evidence, std::optional<ForkRefinement>* fork) {
    const auto& [start, origin, forward, topDirection, anchor, topBefore, topEnd, tolerance] = evidence;
    auto& join = context.res;
    join.topBaseLine = anchor;
    join.topEndpointTolerance = tolerance;
    PathExtrema bottomEnd{}, bottomBefore{};
    const auto probeMode = form.value("bottomProbe").toString();
    if (probeMode == "walk-stroke-up" || probeMode == "down-stroke" || probeMode == "walk-stroke-normal") {
        const auto& ink = context.transformedPath;
        QPointF tangent;
        double stroke = form.value("penWidth").toDouble(context.pairedStrokeWidth);
        if (form.value("strokeSource") == "local") {
            Stretching local{};
            if (!contourNormalCut(context, join.topBaseLine, topDirection == &SearchContext::prev, false, local)) return false;
            stroke = QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length();
        }
        if (!advanceOnContour(ink, join.topBaseLine, topEnd, topDirection == &SearchContext::prev,
                              stroke, join.bottomBaseLine, tangent)) return false;
        const auto bottom = join.bottomBaseLine.point;
        QPointF direction(0, -1);
        if (probeMode == "walk-stroke-normal") {
            const double length = std::hypot(tangent.x(), tangent.y());
            if (length < 1e-8) return false;
            direction = QPointF(-tangent.y(), tangent.x()) / length;
            const double step = stroke * 0.02;
            if (!ink.contains(bottom + step * direction)) direction = -direction;
            if (!ink.contains(bottom + step * direction)) return false;
        }
        const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
        const auto hits = FindInterections(ink, QLineF(bottom, bottom + reach * direction), true);
        auto top = hits.end();
        double nearest = reach;
        for (auto hit = hits.begin(); hit != hits.end(); ++hit) {
            const double distance = QPointF::dotProduct(hit->point - bottom, direction);
            if (distance <= 1e-5 || distance >= nearest) continue;
            nearest = distance;
            top = hit;
        }
        if (top == hits.end() || !interiorChord(ink, *top, join.bottomBaseLine)) return false;
        join.topBaseLine = *top;
    } else if (probeMode == "corner") {
        // Pair an intrinsic shoulder with the nearest opposing sharp corner
        // across ink. Neither a neighboring cut nor its stroke width is used.
        const auto anchor = join.topBaseLine;
        const auto bounds = form.value("oppositeCorner").toObject();
        double nearest = std::numeric_limits<double>::infinity();
        std::optional<PathExtrema> opposite;
        for (auto it = context.timeSorted.begin(); it != context.timeSorted.end(); ++it) {
            const auto& point = *it;
            const double distance = QLineF(anchor.point, point.point).length();
            if (point.isSharp != bounds.value("sharp").toBool(true) ||
                (bounds.contains("xExtremum") && point.isX != bounds.value("xExtremum").toBool()) ||
                (bounds.contains("yExtremum") && point.isY != bounds.value("yExtremum").toBool()) ||
                distance <= 1e-5 || distance >= nearest ||
                !matchesOffset(bounds, point.point - anchor.point) ||
                !matchesOffset(bounds.value("flanks").toObject(), (context.prev(it)->point - point.point) / distance) ||
                !matchesOffset(bounds.value("flanks").toObject(), (context.next(it)->point - point.point) / distance) ||
                !interiorChord(context.transformedPath, anchor, point)) continue;
            opposite = point;
            nearest = distance;
        }
        if (!opposite) return false;
        join.bottomBaseLine = *opposite;
        join.referenceWidth = nearest;
        join.fixedRightBoundary = true;
    } else if (probeMode == "normal") {
        if (!contourNormalCut(context, join.topBaseLine, topDirection == &SearchContext::prev,
                              form.value("topUseStepEnd").toBool(), join)) return false;
        join.fixedRightBoundary = true;
    } else if (probeMode == "up") {
        const auto bottom = join.topBaseLine;
        const auto hits = FindInterections(context.transformedPath,
            QLineF(bottom.point, QPointF(bottom.point.x(), context.bbox.top() - 1)), true);
        std::optional<PathExtrema> top;
        double distance = context.bbox.height() + 1;
        for (const auto& hit : hits) {
            const double rise = bottom.point.y() - hit.point.y();
            if (rise <= 1e-5 || rise >= distance || !interiorChord(context.transformedPath, hit, bottom)) continue;
            top = hit;
            distance = rise;
        }
        if (!top) return false;
        join.topBaseLine = *top;
        join.bottomBaseLine = bottom;
        join.fixedRightBoundary = true;
    } else if (probeMode == "below") {
        const auto anchor = join.topBaseLine;
        const auto hits = FindInterections(context.transformedPath,
            QLineF(anchor.point, QPointF(anchor.point.x(), context.bbox.bottom() + 1)), true);
        bool found = false;
        for (size_t i = 1; i < hits.size(); ++i) {
            if (hits[i - 1].point.y() < anchor.point.y() - 1e-5 ||
                !interiorChord(context.transformedPath, hits[i - 1], hits[i])) continue;
            join.topBaseLine = hits[i - 1];
            join.bottomBaseLine = hits[i];
            found = true;
            break;
        }
        if (!found) return false;
        join.fixedRightBoundary = true;
    } else if (probeMode == "left" || probeMode == "right") {
        const auto opposite = horizontalOpposite(context, join.topBaseLine, probeMode == "left");
        if (!opposite) return false;
        join.bottomBaseLine = *opposite;
        join.fixedRightBoundary = true;
    } else if (form.value("bottomProbe") == "down") {
        const auto below = [&](const PathExtrema& anchor) -> std::optional<PathExtrema> {
            const auto top = anchor.point;
            const QLineF probe(top, QPointF(top.x(), context.bbox.bottom() + 1));
            const auto hits = FindInterections(context.transformedPath, probe, true);
            const auto bottom = std::find_if(hits.begin(), hits.end(), [&](const PathExtrema& hit) {
                return hit.point.y() > top.y() + std::max(1e-5, join.topEndpointTolerance);
            });
            if (bottom == hits.end() || !interiorChord(context.transformedPath, anchor, *bottom, join.topEndpointTolerance))
                return {};
            return *bottom;
        };
        const bool trimToNormal = form.value("trimExitToNormal").toBool();
        auto bottom = below(join.topBaseLine);
        if (!bottom && trimToNormal) {
            const auto rim = context.timeSorted.find(join.topBaseLine);
            if (rim != context.timeSorted.end()) {
                const auto exit = (context.*topDirection)(rim);
                // A tiny vertical edge can split the rim landmark in two.
                // Use its end where the outgoing stroke is visible.
                const auto step = exit->point - rim->point;
                if (std::abs(step.x()) < 1e-8 && step.y() > 0) {
                    const auto opposite = below(*exit);
                    if (opposite && step.y() < .15 * QLineF(exit->point, opposite->point).length()) {
                        join.topBaseLine = *exit;
                        bottom = opposite;
                    }
                }
            }
        }
        if (!bottom) return false;
        join.bottomBaseLine = *bottom;
        if (trimToNormal) {
            auto normal = join;
            if (anchoredStrokeNormalCut(context, normal, topDirection == &SearchContext::prev, true) &&
                normal.bottomBaseLine.point.x() > join.bottomBaseLine.point.x() + 1e-5) {
                normal.referenceWidth = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).length();
                join = normal;
            }
        }
        join.fixedRightBoundary = true;
    } else if (!walkContourSteps(context, origin, form.value("bottom").toArray(), forward, join.bottomBaseLine, bottomBefore, bottomEnd)) {
        return false;
    }
    if (form.contains("visibleFrom") && !interiorChord(context.transformedPath, join.topBaseLine, join.bottomBaseLine)) {
        // A folded rim can cross the chord between the two walk landmarks.
        // Keep the uninterrupted ink segment visible from the specified flank.
        const bool fromBottom = form.value("visibleFrom") == "bottom";
        const auto& fixed = fromBottom ? join.bottomBaseLine : join.topBaseLine;
        auto& other = fromBottom ? join.topBaseLine : join.bottomBaseLine;
        const QLineF chord(fixed.point, other.point);
        const auto hits = FindInterections(context.transformedPath, chord, true);
        const auto visible = std::find_if(hits.begin(), hits.end(), [&](const PathExtrema& hit) {
            return interiorChord(context.transformedPath, fixed, hit);
        });
        if (visible == hits.end()) return false;
        other = *visible;
    }
    if (form.value("walkNormalOffset").isDouble() && form.value("walkNormalOffset").toDouble() > 0) {
        const auto original = join;
        const double width = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).length();
        auto limit = topEnd;
        if (form.value("topUseStepEnd").toBool()) {
            // The offset starts at the walk's endpoint. Continue along the
            // contour, across subdivisions (and its arbitrary SVG start),
            // rather than stopping at a possibly adjacent micro-extremum.
            limit = join.topBaseLine;
            limit.time += (topDirection == &SearchContext::prev ? 1 : -1) *
                int(baseContourCurves(context.transformedPath).size());
        }
        PathExtrema anchor{}; QPointF tangent;
        if (!advanceOnContour(context.transformedPath, join.topBaseLine, limit, topDirection == &SearchContext::prev,
                width * form.value("walkNormalOffset").toDouble(), anchor, tangent) ||
            !contourNormalCut(context, anchor, topDirection == &SearchContext::prev, false, join)) return false;
        if (form.contains("forkSide")) {
            const int side = form.value("forkSide") == "left" ? -1 : 1;
            const double originX = original.topBaseLine.point.x();
            const auto onSide = [&](const PathExtrema& point) {
                return side * (point.point.x() - originX) >= -1e-8;
            };
            // A slanted normal can legitimately swing across the probe. Only
            // correct it if it went past the upward-pointing fork of the lower outline.
            const auto from = getJoinTime(original, context.orientation);
            const auto to = getJoinTime(join, context.orientation);
            const auto lower = join.bottomBaseLine.point;
            if (onSide(anchor) && !onSide(join.bottomBaseLine)) {
                const auto corner = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [&](const PathExtrema& p) {
                    return p.isSharp && p.isY && (p.point.x()-originX)*(p.point.x()-lower.x()) < 0 &&
                        p.point.y() >= original.topBaseLine.point.y() &&
                        p.point.y() < std::min(original.bottomBaseLine.point.y(), lower.y()) &&
                        from.isStrictLeft(p.time) != to.isStrictLeft(p.time);
                });
                if (corner != context.timeSorted.end()) {
                    if (fork) *fork = ForkRefinement{join, *corner};
                    join = original;
                    if (!strokeNormalCut(context, join, side * form.value("walkNormalOffset").toDouble()) ||
                        !onSide(join.topBaseLine) || !onSide(join.bottomBaseLine)) return false;
                }
            }
        }
        join.fixedRightBoundary = true;
    }
    if (form.value("normalCut").toBool() &&
        !strokeNormalCut(context, join, form.value("strokeOffsetX").toDouble())) return false;
    if (form.value("anchorSide") == "bottom") std::swap(join.topBaseLine, join.bottomBaseLine);
    const bool span = form.value("span").toBool() && !join.fixedRightBoundary;
    join.topRight = span ? topBefore : join.topBaseLine;
    join.bottomRight = span ? bottomBefore : join.bottomBaseLine;
    join.topLeft = span ? topEnd : join.topBaseLine;
    join.bottomLeft = span ? bottomEnd : join.bottomBaseLine;
    join.length = span ? std::min(join.topRight.point.x(), join.bottomRight.point.x()) -
                         std::max(join.topLeft.point.x(), join.bottomLeft.point.x()) : 0;
    context.nextIter = start;
    return true;
}

// Adapter for forms which have not moved to body/cut declarations.
bool searchContourWalk(SearchContext& context, const PathExtrema* crest,
                              bool reverseTop, bool anchorOnly,
                              std::optional<ForkRefinement>* fork) {
    const auto evidence = recognizeContourRoute(context, context.form, crest, reverseTop);
    if (!evidence) return false;
    if (anchorOnly) {
        context.res.topBaseLine = evidence->anchor;
        context.res.topEndpointTolerance = evidence->endpointTolerance;
        return true;
    }
    return projectContourRoute(context, context.form, *evidence, fork);
}

// Project a cut using the contour evidence and policy declared by the form.
bool declaredContourRouteCut(SearchContext& context, const QJsonObject& bodyRoute,
        const QJsonObject& cutPolicy, const PathExtrema* crest,
        bool reverse, std::optional<ForkRefinement>* fork) {
    const auto evidence = recognizeContourRoute(context, bodyRoute, crest, reverse);
    if (!evidence) {
        inspectReason("Body", "The contour did not satisfy this route's directions and constraints.");
        return false;
    }
    inspectLandmark("Route origin", *evidence->origin);
    inspectLandmark("Route anchor", evidence->anchor);
    const bool projected = projectContourRoute(context, withOverrides(bodyRoute, cutPolicy), *evidence, fork);
    if (!projected) inspectReason("Cuts", "Route recognized; its probe or normal did not produce a valid cross-section.");
    return projected;
}

std::optional<SavedContourRoute> recognizeSavedRoute(SearchContext &geometry, const QJsonObject &route,
                                                            const PathExtrema *crest, bool reverse) {
    const auto found = recognizeContourRoute(geometry, route, crest, reverse);
    if (!found) return {};
    return SavedContourRoute{*found->start,
                             *found->origin,
                             found->anchor,
                             found->topBefore,
                             found->topEnd,
                             found->forward == &SearchContext::prev,
                             found->topDirection == &SearchContext::prev,
                             found->endpointTolerance};
}

bool projectSavedRoute(SearchContext &context, const std::optional<SavedContourRoute> &stored,
                              const QJsonObject &route, const QJsonObject &policy,
                              std::optional<ForkRefinement> *fork) {
    if (!stored) {
        inspectReason("Body", "The contour did not satisfy this route's directions and constraints.");
        return false;
    }
    const auto& r = *stored;
    const auto start = context.xSorted.find(r.start);
    const auto origin = context.timeSorted.find(r.origin);
    if (start == context.xSorted.end() || origin == context.timeSorted.end()) return false;
    const ContourRouteEvidence evidence{start, origin,
        r.forwardPrevious ? &SearchContext::prev : &SearchContext::next,
        r.topPrevious ? &SearchContext::prev : &SearchContext::next,
        r.anchor, r.topBefore, r.topEnd, r.endpointTolerance};
    const bool projected = projectContourRoute(context, withOverrides(route, policy), evidence, fork);
    if (!projected) inspectReason("Cuts", "Route recognized; its probe or normal did not produce a valid cross-section.");
    return projected;
}

// Split the two bridges of a loop attached to an ascender. The outer walk
// supplies the first endpoint; sharp inner left/right corners supply the
// other endpoints. The lower bridge is projected out of the counter's tip.
// Track the thin horizontal ink run through a join. The first change in
// thickness or height on either side marks the transition into a body.
bool findJoinEnds(const QPainterPath &ink, const Stretching &join, double minX, double maxX,
                         const QJsonObject &settings, Stretching &left, Stretching &right) {
    const auto center = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).center();
    const auto box = ink.boundingRect();
    const double anchorWidth = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).length();
    if (anchorWidth < settings.value("minAnchorWidth").toDouble())
        return false;
    const double samplesPerWidth = settings.value("samplesPerAnchorWidth").toDouble();
    if (anchorWidth <= 0 || samplesPerWidth <= 0)
        return false;
    const double step = anchorWidth / samplesPerWidth;
    // A steep anchor follows the cross section of the join. An oblique
    // anchor can sit against the body, so keep the wider clearance there.
    const bool steepAnchor = std::abs(join.bottomBaseLine.point.y() - join.topBaseLine.point.y()) >=
                             anchorWidth * settings.value("steepAnchorRatio").toDouble();
    const double clearanceRatio =
        settings.value(steepAnchor ? "steepClearanceRatio" : "obliqueClearanceRatio").toDouble();
    const double trackingDistance = anchorWidth * settings.value("trackingDistanceRatio").toDouble();
    const auto sample = [&](double x, double y, Stretching &run, bool &nearInk) {
        const QLineF probe(x, box.top() - step, x, box.bottom() + step);
        const auto hits = FindInterections(ink, probe);
        double distance = trackingDistance;
        bool found = false;
        for (size_t i = 1; i < hits.size(); ++i) {
            const auto &top = hits[i - 1];
            const auto &bottom = hits[i];
            const QLineF chord(top.point, bottom.point);
            const double offset = std::abs(chord.center().y() - y);
            if (offset >= distance || top.pathIndex != 0 || bottom.pathIndex != 0 || !ink.contains(chord.center()))
                continue;
            run.topBaseLine = top;
            run.bottomBaseLine = bottom;
            const double clearance = chord.length() * clearanceRatio;
            nearInk = (i >= 3 && top.point.y() - hits[i - 2].point.y() < clearance) ||
                      (i + 2 < hits.size() && hits[i + 1].point.y() - bottom.point.y() < clearance);
            distance = offset;
            found = true;
        }
        return found;
    };
    const auto width = [](const Stretching& run) {
        return run.bottomBaseLine.point.y() - run.topBaseLine.point.y();
    };
    const auto height = [](const Stretching& run) {
        return (run.topBaseLine.point.y() + run.bottomBaseLine.point.y()) / 2;
    };
    Stretching seed = join;
    bool seedNearInk = false;
    double best = std::numeric_limits<double>::infinity();
    const int seedSteps = std::clamp(settings.value("seedSteps").toInt(), 0, 1000);
    const double minSeedWidth = anchorWidth * settings.value("minSeedWidthRatio").toDouble();
    const double maxSeedWidth = anchorWidth * settings.value("maxSeedWidthRatio").toDouble();
    const double seedDistancePenalty = settings.value("seedDistancePenalty").toDouble();
    for (int offset = -seedSteps; offset <= seedSteps; ++offset) {
        const double x = center.x() + offset * step;
        Stretching run = join;
        bool nearInk = false;
        if (x <= minX || x >= maxX || !sample(x, center.y(), run, nearInk) ||
            width(run) < minSeedWidth || width(run) > maxSeedWidth) continue;
        const double cost = width(run) + std::abs(offset) * step * seedDistancePenalty;
        if (cost < best) {
            best = cost;
            seed = run;
            seedNearInk = nearInk;
        }
    }
    if (!std::isfinite(best)) return false;

    const int maxTrackSteps = std::clamp(settings.value("maxTrackSteps").toInt(), 0, 1000);
    const double minTrackWidth = width(seed) * settings.value("minTrackWidthRatio").toDouble();
    const double maxTrackWidth = width(seed) * settings.value("maxTrackWidthRatio").toDouble();
    const double maxSteepRise = step * settings.value("maxSteepSlope").toDouble();
    const double maxEdgeDrift = width(seed) * settings.value("maxEdgeDriftRatio").toDouble();
    const double minSpan = width(seed) * settings.value("minSpanToStrokeRatio").toDouble();
    const auto findEnd = [&](int direction, Stretching& end) {
        end = seed;
        const double start = seed.topBaseLine.point.x();
        for (int index = 1; index <= maxTrackSteps; ++index) {
            const double x = start + direction * index * step;
            if (x <= minX || x >= maxX) return false;
            Stretching run = join;
            bool nearInk = false;
            if (!sample(x, height(end), run, nearInk) || (!seedNearInk && nearInk) || width(run) > maxTrackWidth ||
                width(run) < minTrackWidth ||
                (steepAnchor && std::abs(height(run) - height(end)) > maxSteepRise) ||
                std::abs(run.topBaseLine.point.y() - seed.topBaseLine.point.y()) > maxEdgeDrift ||
                std::abs(run.bottomBaseLine.point.y() - seed.bottomBaseLine.point.y()) > maxEdgeDrift) return true;
            end = run;
        }
        return false;
    };
    if (join.fixedRightBoundary) right = join;
    return findEnd(-1, left) && (join.fixedRightBoundary || findEnd(1, right)) &&
           right.topBaseLine.point.x() - left.topBaseLine.point.x() >= minSpan &&
           interiorChord(ink, left.topBaseLine, left.bottomBaseLine) &&
           interiorChord(ink, right.topBaseLine, right.bottomBaseLine);
}

bool medialChordAt(const SearchContext& context, double x, double y,
                          double tolerance, Stretching& cut, bool useBottom,
                          double minY) {
    const auto& ink = context.transformedPath;
    const auto hits = FindInterections(ink, QLineF(x, context.bbox.top() - 1, x, context.bbox.bottom() + 1));
    double best = tolerance;
    bool found = false;
    for (size_t i = 1; i < hits.size(); ++i) {
        const auto& top = hits[i - 1];
        const auto& bottom = hits[i];
        const double sampleY = useBottom ? bottom.point.y() : (top.point.y() + bottom.point.y()) / 2;
        const double distance = std::abs(sampleY - y);
        if (top.point.y() < minY || distance >= best || !interiorChord(ink, top, bottom)) continue;
        cut.topBaseLine = top;
        cut.bottomBaseLine = bottom;
        best = distance;
        found = true;
    }
    return found;
}

static QPointF unitCurveTangent(const QBezier& curve, double t) {
    const double u = 1 - t;
    auto tangent = u * u * (curve.pt2() - curve.pt1()) +
        2 * u * t * (curve.pt3() - curve.pt2()) + t * t * (curve.pt4() - curve.pt3());
    if (std::hypot(tangent.x(), tangent.y()) <= 1e-9)
        tangent = curve.pointAt(std::min(1.0, t + 1e-4)) - curve.pointAt(std::max(0.0, t - 1e-4));
    const double length = std::hypot(tangent.x(), tangent.y());
    return length > 1e-9 ? tangent / length : QPointF{};
}

// Rotate a local cross-section around a fixed landmark. At a corner,
// measure both flanks in the direction of the join rather than into the body.
bool anchoredStrokeNormalCut(const SearchContext& context, Stretching& cut, bool forward, bool anchorTop) {
    const auto& ink = context.transformedPath;
    const auto curves = baseContourCurves(ink);
    const int count = static_cast<int>(curves.size());
    if (count == 0) return false;
    const auto flank = [&](const PathExtrema& point, bool forward) {
        double time = point.time;
        if (std::abs(time - std::round(time)) < 1e-8) time = std::round(time);
        int index = static_cast<int>(std::floor(time));
        double t = time - index;
        if (!forward && t == 0) { --index; t = 1; }
        const auto tangent = unitCurveTangent(curves[(index % count + count) % count], t);
        return forward ? tangent : -tangent;
    };
    const auto& anchor = anchorTop ? cut.topBaseLine : cut.bottomBaseLine;
    const auto tangent = flank(anchor, forward);
    if (tangent.isNull()) return false;
    auto& endpoint = anchorTop ? cut.bottomBaseLine : cut.topBaseLine;
    auto opposite = endpoint;
    const double reach = 2 * QLineF(anchor.point, opposite.point).length();
    for (int step = 0; step < 16; ++step) {
        const auto other = flank(opposite, !forward);
        if (other.isNull() || QPointF::dotProduct(tangent, other) <= 0) return false;
        const auto axis = tangent + other;
        const auto chord = opposite.point - anchor.point;
        const double width = std::hypot(chord.x(), chord.y());
        const double length = std::hypot(axis.x(), axis.y());
        if (std::abs(QPointF::dotProduct(axis, chord)) <= 1e-6 * width * length) {
            if (!interiorChord(ink, opposite, anchor)) return false;
            endpoint = opposite;
            cut.referenceWidth = width;
            return true;
        }
        const QPointF normal(-axis.y(), axis.x());
        const auto ray = normal * (std::copysign(reach, QPointF::dotProduct(normal, chord)) / length);
        std::optional<PathExtrema> hit;
        double nearest = reach;
        for (const auto& point : FindInterections(ink, QLineF(anchor.point, anchor.point + ray), true)) {
            const double distance = QLineF(anchor.point, point.point).length();
            if (point.pathIndex == 0 && (anchorTop ? point.point.y() > anchor.point.y() :
                    point.point.y() < anchor.point.y()) && distance > 1e-5 && distance < nearest) {
                hit = point;
                nearest = distance;
            }
        }
        if (!hit) return false;
        opposite = *hit;
    }
    return false;
}

// Bisect the directions of the two outline tangents to estimate the local
// stroke direction, then intersect its normal with the surrounding ink.
bool strokeNormalCut(const SearchContext& context, Stretching& cut, double offsetX) {
    const auto& ink = context.transformedPath;
    const auto curves = baseContourCurves(ink);
    const auto tangentAt = [&](const PathExtrema& point) {
        const double time = std::abs(point.time - curves.size()) < 1e-9 ? 0 : point.time;
        const int index = static_cast<int>(std::floor(time));
        if (point.pathIndex != 0 || index < 0 || index >= static_cast<int>(curves.size())) return QPointF{};
        return unitCurveTangent(curves[index], time - index);
    };
    const auto center = [](const Stretching& candidate) {
        return QLineF(candidate.topBaseLine.point, candidate.bottomBaseLine.point).center();
    };
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    const auto solve = [&](const QPointF& pivot) -> StrokeNormalSample {
        struct Candidate {
            std::pair<PathExtrema, PathExtrema> endpoints;
            double error;
            double width;
            std::optional<bool> inside;
        };
        const auto evaluate = [&](double angle) -> std::optional<Candidate> {
            const QPointF normal(std::sin(angle), std::cos(angle));
            const auto hits = FindInterections(ink, QLineF(pivot - normal * reach, pivot + normal * reach), true);
            std::optional<PathExtrema> top, bottom;
            double topDistance = -reach, bottomDistance = reach;
            for (const auto& hit : hits) {
                const double distance = QPointF::dotProduct(hit.point - pivot, normal);
                if (distance < -1e-6 && distance > topDistance) { top = hit; topDistance = distance; }
                if (distance > 1e-6 && distance < bottomDistance) { bottom = hit; bottomDistance = distance; }
            }
            if (!top || !bottom) return std::nullopt;
            auto a = tangentAt(*top), b = tangentAt(*bottom);
            if (a.isNull() || b.isNull()) return std::nullopt;
            if (QPointF::dotProduct(a, b) < 0) b = -b;
            auto tangent = a + b;
            const double length = std::hypot(tangent.x(), tangent.y());
            if (length <= 1e-9) return std::nullopt;
            tangent /= length;
            if (tangent.x() < 0) tangent = -tangent;
            return Candidate{{*top, *bottom}, QPointF::dotProduct(normal, tangent),
                             QLineF(top->point, bottom->point).length(), {}};
        };
        const auto inside = [&](Candidate& candidate) {
            if (!candidate.inside.has_value())
                candidate.inside = interiorChord(ink, candidate.endpoints.first, candidate.endpoints.second);
            return *candidate.inside;
        };
        std::optional<Candidate> best, previous;
        double previousAngle = 0;
        std::vector<Candidate> widthSamples;
        widthSamples.reserve(121);
        const auto accept = [&](Candidate& value) {
            if (std::abs(value.error) < 1e-5 && (!best || value.width < best->width) && inside(value)) best = value;
        };
        // Only a root or the two samples bracketing it can supply a normal.
        // Defer the five ink probes and the second intersection scan until then.
        for (int sample = 0; sample <= 120; ++sample) {
            const double angle = -M_PI / 2 + M_PI * sample / 120;
            auto current = evaluate(angle);
            if (current) {
                accept(*current);
                if (previous && previous->error * current->error < 0 && inside(*previous) && inside(*current)) {
                    double lo = previousAngle, hi = angle, loError = previous->error;
                    for (int iteration = 0; iteration < 36; ++iteration) {
                        const double middle = (lo + hi) / 2;
                        auto value = evaluate(middle);
                        if (!value || !inside(*value)) break;
                        if (std::abs(value->error) < 1e-6) { accept(*value); break; }
                        if (loError * value->error <= 0) hi = middle;
                        else { lo = middle; loError = value->error; }
                    }
                }
                if (!best && current->width < reach) widthSamples.push_back(*current);
            }
            previous = current;
            previousAngle = angle;
        }
        if (best) return {best->endpoints, best->width};
        // Without a normal, the caller uses the shortest valid sampled width.
        std::sort(widthSamples.begin(), widthSamples.end(),
                  [](const Candidate& a, const Candidate& b) { return a.width < b.width; });
        for (auto& candidate : widthSamples)
            if (inside(candidate)) return {std::nullopt, candidate.width};
        return {};
    };
    const auto orient = [&](Stretching& candidate, double* measuredWidth = nullptr) {
        const auto pivot = center(candidate);
        StrokeNormalSample sample;
        if (context.outlineCache && std::isfinite(pivot.x()) && std::isfinite(pivot.y())) {
            auto [it, inserted] = context.outlineCache->normalsByCenter.try_emplace(std::make_pair(pivot.x(), pivot.y()));
            if (inserted) it->second = solve(pivot);
            sample = it->second;
        } else {
            sample = solve(pivot);
        }
        if (measuredWidth) *measuredWidth = sample.width;
        if (!sample.endpoints) return false;
        candidate.topBaseLine = sample.endpoints->first;
        candidate.bottomBaseLine = sample.endpoints->second;
        return true;
    };
    auto result = cut;
    double stroke = 0;
    const bool initialNormal = orient(result, &stroke);
    if (offsetX != 0) {
        // At a corner the unshifted chord may have no unique normal. Its
        // shortest local cross-section still supplies a width for the offset.
        if (stroke <= 0) return false;
        const auto pivot = center(cut);
        const double dx = offsetX * stroke;
        if (!medialChordAt(context, pivot.x() + dx, pivot.y(), 2 * stroke, result) ||
            !orient(result)) return false;
    } else if (!initialNormal) return false;
    cut.referenceWidth = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
    cut.topBaseLine = result.topBaseLine;
    cut.bottomBaseLine = result.bottomBaseLine;
    cut.topEndpointTolerance = 0;
    return true;
}

} // namespace MedinaCutter
