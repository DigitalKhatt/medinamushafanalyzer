#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

// Follow a head's flank through intermediate corners to its horizontal
// extremum. A Bezier vertex on the flank is not necessarily its side edge.
timesortedtype::iterator headFlank(SearchContext& context, timesortedtype::iterator crest,
                                         bool increasingTime) {
    const auto advance = increasingTime ? &SearchContext::prev : &SearchContext::next;
    auto point = (context.*advance)(crest);
    const double direction = point->point.x() - crest->point.x();
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(point);
        if (next == crest || direction * (next->point.x() - point->point.x()) <= 0) break;
        point = next;
    }
    return point;
}

std::vector<OpenHeadBody> recognizeOpenHeads(const InkOutline &outline, const QJsonObject &settings) {
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    std::vector<OpenHeadBody> result;
    for (auto crest = geometry.timeSorted.begin(); crest != geometry.timeSorted.end(); ++crest) {
        if (crest->point.y() >= geometry.prev(crest)->point.y() || crest->point.y() >= geometry.next(crest)->point.y())
            continue;
        const auto a = headFlank(geometry, crest, true), b = headFlank(geometry, crest, false);
        const auto rightEdge = a->point.x() > b->point.x() ? a : b;
        const auto leftEdge = rightEdge == a ? b : a;
        const auto r = rightEdge->point, l = leftEdge->point;
        const double width = r.x() - l.x();
        if (width <= 0 || r.x() <= crest->point.x() || l.x() >= crest->point.x() ||
            r.x() <= geometry.prev(rightEdge)->point.x() || r.x() <= geometry.next(rightEdge)->point.x() ||
            l.x() >= geometry.prev(leftEdge)->point.x() || l.x() >= geometry.next(leftEdge)->point.x())
            continue;
        const double minRise = settings.value("minFlankRise").toDouble(0.15) * width;
        const double maxRise = settings.value("maxFlankRise").toDouble(0.8) * width;
        if (r.y() - crest->point.y() < minRise || r.y() - crest->point.y() > maxRise ||
            l.y() - crest->point.y() < minRise || l.y() - crest->point.y() > maxRise) continue;
        const double height = std::max(r.y(), l.y()) - crest->point.y();
        const QRectF head(QPointF(l.x(), crest->point.y()), QPointF(r.x(), crest->point.y() + height));
        bool closed = false;
        for (size_t i = 1; i < geometry.subPaths.size(); ++i) {
            const auto hole = geometry.subPaths[i].boundingRect();
            if (head.contains(hole.center()) && !geometry.transformedPath.contains(hole.center())) {
                closed = true; break;
            }
        }
        if (closed) continue;
        result.push_back({*crest, *rightEdge, *leftEdge, height});
    }
    return result;
}

std::vector<OpenHeadBody> selectOpenHeads(const std::vector<OpenHeadBody> &bodies, double minX, double maxX) {
    std::vector<OpenHeadBody> selected;
    for (const auto &body : bodies)
        if (body.crest.point.x() > minX && body.crest.point.x() < maxX)
            selected.push_back(body);
    return selected;
}

std::vector<std::pair<Stretching, Stretching>>
cutOpenHeads(SearchContext &context, const std::vector<OpenHeadBody> &bodies, const QJsonObject &policy) {
    std::vector<std::pair<Stretching, Stretching>> candidates;
    for (const auto &body : bodies) {
        const auto r = body.rightEdge.point, l = body.leftEdge.point;
        const double depth = body.height * policy.value("depth").toDouble(1),
                     reach = body.height * policy.value("reach").toDouble(2);
        Stretching right{}, left{};
        if (medialChordAt(context, r.x(), r.y() + depth, reach, right) &&
            medialChordAt(context, l.x(), l.y() + depth, reach, left) && right.topBaseLine.point.y() >= r.y() &&
            left.topBaseLine.point.y() >= l.y())
            candidates.emplace_back(right, left);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return Stretching::cmpbyx{}(a.first, b.first);
    });
    return candidates;
}

std::vector<CrestedLoopBody> recognizeCrestedLoops(const InkOutline &outline, const QJsonObject &settings) {
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    const auto &ink = outline.ink;
    std::vector<CrestedLoopBody> result;
    for (auto crest = geometry.timeSorted.begin(); crest != geometry.timeSorted.end(); ++crest) {
        if (crest->point.y() >= geometry.prev(crest)->point.y() || crest->point.y() >= geometry.next(crest)->point.y())
            continue;
        const auto a = headFlank(geometry, crest, true), b = headFlank(geometry, crest, false);
        const auto rightEdge = a->point.x() > b->point.x() ? a : b;
        const auto leftEdge = rightEdge == a ? b : a;
        const auto r = rightEdge->point, l = leftEdge->point;
        if (r.x() <= crest->point.x() || l.x() >= crest->point.x() || r.x() <= geometry.prev(rightEdge)->point.x() ||
            r.x() <= geometry.next(rightEdge)->point.x() || l.x() >= geometry.prev(leftEdge)->point.x() ||
            l.x() >= geometry.next(leftEdge)->point.x())
            continue;
        for (size_t i = 1; i < geometry.subPaths.size(); ++i) {
            const auto counter = geometry.subPaths[i].boundingRect();
            const double scale = counter.height();
            if (scale <= 0 || counter.width() <= 0 || ink.contains(counter.center()) ||
                counter.left() <= l.x() || counter.right() >= r.x() ||
                counter.top() <= crest->point.y() || counter.top() >= std::max(r.y(), l.y()) ||
                !matchesOffset(settings.value("crestToRight").toObject(), (r - crest->point) / scale) ||
                !matchesOffset(settings.value("crestToLeft").toObject(), (l - crest->point) / scale)) continue;
            result.push_back({counter, *crest, *rightEdge, *leftEdge});
        }
    }
    return result;
}

std::vector<std::pair<Stretching, Stretching>>
cutCrestedLoops(SearchContext &context, const std::vector<CrestedLoopBody> &bodies, const QJsonObject &policy) {
    std::vector<std::pair<Stretching, Stretching>> candidates;
    const auto &ink = context.transformedPath;
    for (const auto &body : bodies) {
        const auto &counter = body.counter;
        const auto *crest = &body.crest;
        const auto r = body.rightEdge.point, l = body.leftEdge.point;
        const double scale = counter.height();
        Stretching right{}, left{};
        const double distance = policy.value("maxNeckDistance").toDouble(2) * scale;
        if (!medialChordAt(context, r.x(), counter.bottom(), distance, right, false, counter.bottom()) ||
            !medialChordAt(context, l.x(), counter.bottom(), distance, left, false, counter.bottom()))
            continue;
        const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3 || !pieces[1].path.boundingRect().contains(counter) ||
            !pieces[1].path.contains(crest->point + QPointF(0, .01 * scale)) || toSubpaths(pieces[1].path).size() != 2)
            continue;
        right.referenceWidth = QLineF(right.topBaseLine.point, right.bottomBaseLine.point).length();
        left.referenceWidth = QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length();
        candidates.emplace_back(right, left);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return Stretching::cmpbyx{}(a.first, b.first);
    });
    return candidates;
}

// A contour may record several landmarks along a single rising or falling flank.
std::optional<PathExtrema> contourFlank(SearchContext& context, timesortedtype::iterator anchor,
                                               bool previous, const QJsonObject& bounds) {
    const auto advance = previous ? &SearchContext::prev : &SearchContext::next;
    auto current = anchor;
    QPointF direction;
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(current);
        const auto delta = next->point - current->point;
        if (step == 0) direction = delta;
        else if (delta.x() * direction.x() < -1e-8 || delta.y() * direction.y() < -1e-8) return {};
        if (matchesOffset(bounds, next->point - anchor->point)) return *next;
        if (!bounds.value("accumulate").toBool()) return {};
        current = next;
    }
    return {};
}

// A stacked tooth can have a valid cut across its cap while the descending
// stroke below it is still inside the hah mask. Its true shared boundary is
// the horizontal chord at the sharp junction with the top of the hah head.
std::optional<Stretching> stackedAttachment(SearchContext& context, const QJsonObject& settings,
                                                   const QRectF& cap, const QPainterPath& head,
                                                   const Stretching& incoming, const Stretching& outgoing,
                                                   bool clippedCap) {
    const auto headFlank = settings.value("headFlank").toObject();
    const auto risingFlank = settings.value("risingFlank").toObject();
    const auto between = [&](const PathExtrema& point) {
        return getJoinTime(incoming, context.orientation).isLeft(point.time) &&
               getJoinTime(outgoing, context.orientation).isStrictRight(point.time);
    };
    const auto atIncoming = [&](const PathExtrema& point) {
        return QLineF(point.point, incoming.topBaseLine.point).length() < 1e-5 ||
               QLineF(point.point, incoming.bottomBaseLine.point).length() < 1e-5;
    };
    std::optional<Stretching> candidate;
    for (auto at = context.timeSorted.begin(); at != context.timeSorted.end(); ++at) {
        if (!at->isSharp || !at->isX || !between(*at)) continue;
        if (!(contourFlank(context, at, true, headFlank) && contourFlank(context, at, false, risingFlank)) &&
            !(contourFlank(context, at, false, headFlank) && contourFlank(context, at, true, risingFlank))) continue;
        auto opposite = horizontalOpposite(context, *at, true);
        bool foundFromOpposite = false;
        if (!opposite && clippedCap) {
            // At a cusp an intersection can be reported from only one end.
            for (const auto& edge : context.timeSorted) {
                if (!edge.isSharp || edge.point.x() >= at->point.x() ||
                    std::abs(edge.point.y() - at->point.y()) > .05 || !between(edge)) continue;
                const auto hit = horizontalOpposite(context, edge, false);
                if (!hit || QLineF(hit->point, at->point).length() > .05) continue;
                if (opposite) return {};
                opposite = edge;
                foundFromOpposite = true;
            }
        }
        if (!opposite || (!atIncoming(*at) &&
            (!between(*opposite) || !head.contains((opposite->point + at->point) / 2)))) continue;
        Stretching shared{};
        shared.topBaseLine = shared.topLeft = shared.topRight = *opposite;
        shared.bottomBaseLine = shared.bottomLeft = shared.bottomRight = *at;
        if (at->point.y() <= cap.bottom() + 1e-5) {
            // An intrinsic initial-body cut can already reach this junction
            // from a rounded lower flank. Keep the established horizontal
            // attachment instead of treating that small overshoot as a new body.
            if (atIncoming(*at) && cap.bottom() - at->point.y() <=
                QLineF(opposite->point, at->point).length()) {
                shared.fixedRightBoundary = shared.fixedLeftBoundary = true;
                shared.kashidaOverrides.insert("enabled", false);
                if (candidate) return {};
                candidate = shared;
                continue;
            }
            // The first walk can clip a speck off the hah lip below the real junction.
            // Only disregard that cap if its entire extent is under one local stroke.
            const double extent = std::hypot(cap.width(), cap.height());
            auto normal = shared;
            if (extent >= QLineF(opposite->point, at->point).length() ||
                (!foundFromOpposite && (!strokeNormalCut(context, normal) ||
                    extent >= QLineF(normal.topBaseLine.point, normal.bottomBaseLine.point).length()))) continue;
        }
        if (candidate) return {};
        shared.fixedRightBoundary = shared.fixedLeftBoundary = true;
        shared.kashidaOverrides.insert("enabled", false);
        candidate = shared;
    }
    return candidate;
}

// Probe the receiving side of an attachment corner. A curved cross-section
// can meet either branch at a sharp turn; both directions give actual ink chords.
std::vector<Stretching> cornerSections(const SearchContext& context, const PathExtrema& corner) {
    std::vector<Stretching> sections;
    for (bool forward : {true, false}) {
        Stretching s{};
        if (contourNormalCut(context, corner, forward, false, s))
            sections.push_back(s);
    }
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    for (auto delta : {QPointF(0, -reach), QPointF(0, reach), QPointF(-reach, 0), QPointF(reach, 0)}) {
        const auto hits =
            FindInterections(context.transformedPath, QLineF(corner.point, corner.point + delta), true);
        for (const auto& hit : hits)
            if (interiorChord(context.transformedPath, corner, hit)) {
                Stretching s{};
                s.topBaseLine = corner;
                s.bottomBaseLine = hit;
                s.referenceWidth = QLineF(corner.point, hit.point).length();
                sections.push_back(s);
                break;
            }
    }
    return sections;
}

void orientSection(const SearchContext& ctx, Stretching& cut, double witness, bool left) {
    if (getJoinTime(cut, ctx.orientation).isLeft(witness) != left)
        std::swap(cut.topBaseLine, cut.bottomBaseLine);
    cut.topRight = cut.topLeft = cut.topBaseLine;
    cut.bottomLeft = cut.bottomRight = cut.bottomBaseLine;
}

// The bowl's upper flank can rise into its own crest beyond the proposed cut.
// Follow that flank to its first turn, then cross the narrow stroke after it.
// The bowl depth bounds the search, so it cannot reach a preceding tooth.
std::optional<Stretching> cutAfterEntryCrest(SearchContext& ctx, Stretching cut,
                                                  const Stretching* preceding,
                                                  bool allowOblique) {
    const auto& tip = *ctx.xSorted.rbegin();
    orientSection(ctx, cut, tip.time, true);
    const auto side = getJoinTime(cut, ctx.orientation);
    const auto& upper = cut.topBaseLine;
    const auto& lower = cut.bottomBaseLine;
    if (upper.point.y() > lower.point.y() + 1e-5) return {};
    const bool upperIncreases = (upper.time < lower.time) == side.startAtLeft;
    const auto firstAfter = [&](const PathExtrema& point, bool increases) {
        if (increases) return ctx.prev(ctx.timeSorted.lower_bound(point));
        auto next = ctx.timeSorted.upper_bound(point);
        return next == ctx.timeSorted.end() ? ctx.timeSorted.begin() : next;
    };
    const auto advance = [&](auto point, bool increases) {
        return increases ? ctx.prev(point) : ctx.next(point);
    };
    auto crest = upper;
    auto point = firstAfter(upper, upperIncreases);
    for (; side.isStrictRight(point->time); point = advance(point, upperIncreases)) {
        if (point->point.y() > crest.point.y() + 1e-5) break;
        if (point->point.y() < crest.point.y()) crest = *point;
    }
    if (!side.isStrictRight(point->time) || point->point.x() <= crest.point.x()) return {};
    double floor = lower.point.y();
    for (const auto& landmark : ctx.timeSorted)
        if (side.isLeft(landmark.time)) floor = std::max(floor, landmark.point.y());
    const double depth = floor - crest.point.y();
    const double rise = upper.point.y() - crest.point.y();
    // A short cut across two inner corners can sit below a broad hook.
    // The first turn beyond the crest then lines up with the lower corner
    // even though the crest itself is farther to the left.
    const bool alignedShoulder = allowOblique && upper.isSharp && lower.isSharp &&
        QLineF(upper.point, lower.point).length() < .1 * depth &&
        std::abs(point->point.x() - lower.point.x()) < .08 * depth;
    if (depth <= 0 || rise < .27 * depth ||
        (std::abs(crest.point.x() - lower.point.x()) > .24 * depth && !alignedShoulder)) return {};

    std::optional<Stretching> best;
    double bestScore = std::numeric_limits<double>::infinity();
    const auto consider = [&](const PathExtrema& top, const PathExtrema& bottom) {
        const double width = QLineF(top.point, bottom.point).length();
        if (top.point.y() >= bottom.point.y() || width < .07 * depth || width > .34 * depth ||
            !side.isStrictRight(top.time) || !interiorChord(ctx.transformedPath, top, bottom)) return;
        if (preceding) {
            const auto limit = getJoinTime(*preceding, ctx.orientation);
            if (!limit.isStrictLeft(top.time) || !limit.isStrictLeft(bottom.time)) return;
        }
        auto candidate = cut;
        candidate.topBaseLine = top;
        candidate.bottomBaseLine = bottom;
        orientSection(ctx, candidate, tip.time, true);
        const auto expanded = getJoinTime(candidate, ctx.orientation);
        if (!expanded.isStrictLeft(crest.time) || !expanded.isLeft(upper.time) ||
            !expanded.isLeft(lower.time)) return;
        const double score = width + .4 * (bottom.point.x() - crest.point.x());
        if (score >= bestScore) return;
        candidate.referenceWidth = width;
        candidate.fixedRightBoundary = true;
        candidate.length = 0;
        best = candidate;
        bestScore = score;
    };
    const auto probe = [&](const PathExtrema& bottom) {
        const double dx = bottom.point.x() - crest.point.x();
        if (dx < .04 * depth || dx > .35 * depth) return;
        const QLineF vertical(bottom.point, QPointF(bottom.point.x(), crest.point.y()));
        for (const auto& top : FindInterections(ctx.transformedPath, vertical, true))
            consider(top, bottom);
    };
    probe(lower);
    for (auto p = firstAfter(lower, !upperIncreases); side.isStrictRight(p->time);
         p = advance(p, !upperIncreases)) {
        if (p->point.x() - crest.point.x() > .35 * depth) break;
        probe(*p);
    }
    if (!best && allowOblique && point->point.x() - crest.point.x() < .20 * depth) {
        // The join can lean past the lower corner. A vertical probe then
        // misses the neck or crosses outside the ink.
        for (auto p = firstAfter(lower, !upperIncreases); side.isStrictRight(p->time);
             p = advance(p, !upperIncreases)) {
            const double dx = p->point.x() - crest.point.x();
            if (dx > .35 * depth) break;
            if (p->isSharp && dx > 0 && std::abs(p->point.x() - point->point.x()) < .15 * depth)
                consider(*point, *p);
        }
    }
    return best;
}

std::optional<Stretching> restoreNoonBowlEntry(SearchContext& ctx, Stretching entry,
                                                     Stretching sending) {
    const auto& tip = *ctx.xSorted.rbegin();
    orientSection(ctx, entry, tip.time, true);
    orientSection(ctx, sending, tip.time, true);
    const auto old = getJoinTime(entry, ctx.orientation);
    const auto limit = getJoinTime(sending, ctx.orientation);
    auto floor = ctx.timeSorted.end();
    for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p)
        if (old.isLeft(p->time) && (floor == ctx.timeSorted.end() || p->point.y() > floor->point.y()))
            floor = p;
    if (floor == ctx.timeSorted.end()) return {};

    Stretching foot{};
    if (!medialChordAt(ctx, floor->point.x(), floor->point.y(), 1., foot, true)) return {};
    const double width = QLineF(foot.topBaseLine.point, foot.bottomBaseLine.point).length();
    if (width <= 1e-5) return {};
    for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
        auto bulge = floor;
        // The lower outline of the bowl moves right and rises from its floor.
        for (size_t n = 0; n < ctx.timeSorted.size(); ++n) {
            const auto next = (ctx.*advance)(bulge);
            if (next->point.x() < bulge->point.x() - 1e-5 ||
                next->point.y() > bulge->point.y() + .05 * width) break;
            bulge = next;
        }
        if (old.isLeft(bulge->time) ||
            bulge->point.x() - floor->point.x() < 1.5 * width ||
            floor->point.y() - bulge->point.y() < width) continue;
        auto elbow = bulge;
        // At the right flank the outer outline turns inward, then heads
        // outward into the connecting stroke.
        for (size_t n = 0; n < ctx.timeSorted.size(); ++n) {
            const auto next = (ctx.*advance)(elbow);
            if (next->point.x() > elbow->point.x() + 1e-5 ||
                next->point.y() > elbow->point.y() + .05 * width) break;
            elbow = next;
        }
        if (bulge->point.y() - elbow->point.y() < .35 * width ||
            bulge->point.x() - elbow->point.x() > .8 * width) continue;
        for (double offset : {.12, .25, .45, .7, 1.}) {
            Stretching candidate{};
            const double x = bulge->point.x() + offset * width;
            if (!medialChordAt(ctx, x, elbow->point.y(), 1.5 * width, candidate) ||
                QLineF(candidate.topBaseLine.point, candidate.bottomBaseLine.point).length() > 1.75 * width) continue;
            orientSection(ctx, candidate, tip.time, true);
            const auto side = getJoinTime(candidate, ctx.orientation);
            if (!side.isStrictLeft(bulge->time) || !side.isLeft(elbow->time) ||
                !side.isLeft(entry.topBaseLine.time) || !side.isLeft(entry.bottomBaseLine.time) ||
                !limit.isStrictLeft(candidate.topBaseLine.time) ||
                !limit.isStrictLeft(candidate.bottomBaseLine.time)) continue;
            return candidate;
        }
    }
    return {};
}

bool cutReturnSeam(SearchContext &ctx, const ReturnSeamBody &body, const QJsonObject &policy, Stretching &cut,
                          double *shoulderX) {
    if (QLineF(body.first.point, body.second.point).length() > policy.value("maxChord").toDouble() ||
        !interiorChord(ctx.transformedPath, body.first, body.second))
        return false;
    cut.topBaseLine = body.first;
    cut.bottomBaseLine = body.second;
    orientSection(ctx, cut, body.tip.time, true);
    cut.fixedLeftBoundary = cut.fixedRightBoundary = true;
    if (shoulderX)
        *shoulderX = body.shoulderX;
    return true;
}

std::optional<ReturnSeamBody> recognizeFoldedReturn(const InkOutline &outline, const QJsonObject &spec) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    const auto tip = ctx.timeSorted.find(*ctx.xSorted.rbegin());
    if (tip == ctx.timeSorted.end())
        return {};
    const auto turn = [&](bool forward) -> std::optional<std::pair<PathExtrema, double>> {
        auto peak = tip, bend = tip, it = tip;
        bool returning = false;
        for (size_t i = 1; i < ctx.timeSorted.size(); ++i) {
            it = forward ? ctx.next(it) : ctx.prev(it);
            const auto p = it->point;
            if (!returning) {
                if (p.x() >= peak->point.x()) peak = it;
                else if (peak->point.x() - tip->point.x() >= spec.value("minBowlRun").toDouble() &&
                         peak->point.x() - p.x() >= spec.value("minFoldRun").toDouble()) {
                    returning = true; bend = it;
                }
            } else if (p.x() < bend->point.x()) bend = it;
            else if (p.x() - bend->point.x() >= spec.value("minReturn").toDouble()) {
                if (bend->point.y() >= peak->point.y()) return {};
                return std::pair{*bend, peak->point.x()};
            }
        }
        return {};
    };
    const auto a = turn(true), b = turn(false);
    const double shoulderDistance = spec.value("maxShoulderDistance").toDouble();
    if (a && b && (shoulderDistance <= 0 || interiorChord(ctx.transformedPath, a->first, b->first)))
        return ReturnSeamBody{*tip, a->first, b->first, std::max(a->second, b->second)};
    if (shoulderDistance <= 0) return {};

    // A rising flank can meet the upper stroke at a shoulder without
    // reversing in X. Pair that corner with the return on the opposite flank.
    const auto shoulder = [&](const auto& bend, bool forward) -> std::optional<ReturnSeamBody> {
        if (!bend) return {};
        const auto advance = forward ? &SearchContext::next : &SearchContext::prev;
        auto before = tip;
        for (auto point = (ctx.*advance)(tip); point != tip; point = (ctx.*advance)(point)) {
            const auto after = (ctx.*advance)(point);
            if (point->time == bend->first.time) break;
            const bool rising = point->point.y() < before->point.y() && after->point.y() < point->point.y();
            if (point->isSharp && rising && point->point.x() - tip->point.x() >= spec.value("minBowlRun").toDouble() &&
                QLineF(point->point, bend->first.point).length() <= shoulderDistance &&
                interiorChord(ctx.transformedPath, *point, bend->first))
                return ReturnSeamBody{*tip, bend->first, *point, std::max(bend->second, point->point.x())};
            before = point;
        }
        return {};
    };
    if (const auto found = shoulder(a, false)) return found;
    return shoulder(b, true);
}

bool readTerminalOutline(SearchContext& context) {
    context.transformedPath.setFillRule(Qt::WindingFill);
    context.bbox = context.transformedPath.boundingRect();
    QVector<PathExtrema> landmarks;
    ShapeItem::getExtrema(context.transformedPath, context.subPaths, landmarks);
    for (const auto& point : landmarks) if (point.pathIndex == 0) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    if (context.timeSorted.size() < 3) return false;
    context.setOrientation();
    return true;
}

static std::vector<MouthLips> recognizeMouthLips(const InkOutline &outline, const QJsonObject &form) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    std::vector<MouthLips> mouths;
    for (auto notch = ctx.timeSorted.begin(); notch != ctx.timeSorted.end(); ++notch) {
        if (!notch->isSharp)
            continue;
        struct Lip {
            PathExtrema turn;
            double rise;
        };
        auto lip = [&](auto advance) -> std::optional<Lip> {
            auto current = notch;
            double rise = 0;
            bool moved = false;
            for (size_t step = 0; step < ctx.timeSorted.size(); ++step) {
                const auto next = (ctx.*advance)(current);
                const auto q = next->point - notch->point;
                if (next->point.x() > current->point.x() + .04)
                    break;
                if (std::abs(q.y()) > std::abs(rise))
                    rise = q.y();
                current = next;
                moved = true;
                // A lower lip reaches the first valley before the rising stem
                // of the next body, even if its contour keeps moving left.
                if (rise > .2 && (ctx.*advance)(current)->point.y() < current->point.y() - .04)
                    break;
            }
            const double run = notch->point.x() - current->point.x();
            if (!moved || run < form.value("minLipRun").toDouble(1.65) || run > form.value("maxLipRun").toDouble(9))
                return {};
            return Lip{*current, rise};
        };
        auto a = lip(&SearchContext::prev), b = lip(&SearchContext::next);
        if (!a || !b)
            continue;
        if (a->rise > b->rise)
            std::swap(a, b);
        if (a->rise > -.22 || b->rise < .28 || b->turn.point.y() <= a->turn.point.y())
            continue;
        mouths.push_back({*notch, a->turn, b->turn});
    }
    return mouths;
}

std::vector<OpenMouthBody> recognizeOpenMouths(const InkOutline &outline, const QJsonObject &form) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    std::vector<OpenMouthBody> bodies;
    for (const auto &lips : recognizeMouthLips(outline, form)) {
        const auto &[notch, upper, lower] = lips;
        const double span = notch.point.x() - upper.point.x();
        // The outer nose lies to the right of the notch. Protect it and both
        // lips from being assigned to an adjacent stem or to a connector.
        auto nose = ctx.timeSorted.end();
        double nearest = 1e9;
        for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p) {
            const auto d = p->point - notch.point;
            if (d.x() < .28 * span || d.x() > 1.75 * span || std::abs(d.y()) > 1.4 ||
                (!p->isX &&
                 !(p->isSharp && ctx.next(p)->point.x() <= p->point.x() && ctx.prev(p)->point.x() <= p->point.x())))
                continue;
            const double v = std::abs(d.y()) + .02 * d.x();
            if (v < nearest) {
                nose = p;
                nearest = v;
            }
        }
        if (nose == ctx.timeSorted.end())
            continue;
        OpenMouthBody body{lips, *nose, {}};
        for (auto corner = ctx.timeSorted.begin(); corner != ctx.timeSorted.end(); ++corner) {
            const auto d = corner->point - notch.point;
            if (!corner->isSharp || d.x() < -span * .65 || d.x() > span * 1.9 || d.y() > 2.25 || d.y() < -3.5)
                continue;
            body.entryCorners.push_back(*corner);
        }
        bodies.push_back(std::move(body));
    }
    return bodies;
}

// The page assembler can reuse the same intrinsic lip inventory independently
// of which mouth currently lies between its neighboring cuts.
std::vector<MouthLips> mouthLips(SearchContext &ctx, const QJsonObject &form) {
    return recognizeMouthLips(inkOutline(ctx), form);
}

std::vector<std::pair<Stretching, Stretching>>
cutOpenMouths(SearchContext &ctx, const std::vector<OpenMouthBody> &bodies, const QJsonObject &form) {
    std::vector<std::pair<Stretching, Stretching>> pairs;
    for (const auto &body : bodies) {
        const auto &[notch, upper, lower] = body.lips;
        const auto &nose = body.nose;
        const double span = notch.point.x() - upper.point.x();
        std::vector<Stretching> lefts = cornerSections(ctx, lower);
        if (form.value("projectExit").toBool() && notch.point.x() - lower.point.x() > 1.3 * span) {
            Stretching projected{};
            if (medialChordAt(ctx, upper.point.x() - .25, notch.point.y() + 1.5, 3, projected))
                lefts.insert(lefts.begin(), projected);
        }
        std::vector<std::pair<double, Stretching>> rights;
        for (const auto &corner : body.entryCorners) {
            const auto d = corner.point - notch.point;
            for (auto cut : cornerSections(ctx, corner)) {
                orientSection(ctx, cut, notch.time, true);
                const auto s = getJoinTime(cut, ctx.orientation);
                const auto l = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
                if (!s.isStrictLeft(upper.time) || !s.isStrictLeft(lower.time) || !s.isStrictLeft(nose.time) ||
                    l.length() > 2.8 || l.length() < .18)
                    continue;
                rights.emplace_back(l.length() + .18 * std::abs(d.x()) + .1 * std::abs(d.y()), cut);
            }
        }
        std::sort(rights.begin(), rights.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto left : lefts) {
            orientSection(ctx, left, notch.time, false);
            const auto s = getJoinTime(left, ctx.orientation);
            if (!s.isStrictRight(upper.time) || !s.isStrictRight(nose.time) ||
                QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length() > 3)
                continue;
            for (const auto& [score, right] : rights) {
                const auto side = getJoinTime(right, ctx.orientation);
                if (!side.isStrictLeft(left.topBaseLine.time) || !side.isStrictLeft(left.bottomBaseLine.time))
                    continue;
                const auto parts = cutRegions(ctx.transformedPath, {}, {right, left}, ctx.orientation, true);
                if (parts.size() != 3 || toSubpaths(parts[1].path).size() != 1)
                    continue;
                const auto box = parts[1].path.boundingRect();
                if (box.height() > form.value("maxHeight").toDouble(5.5) || box.width() > 3.6 * span)
                    continue;
                pairs.emplace_back(right, left);
            }
        }
    }
    return pairs;
}

std::vector<BladeBody> recognizeBlades(const InkOutline &outline, const QJsonObject &form) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    std::vector<BladeBody> bodies;
    for (auto elbow = ctx.timeSorted.begin(); elbow != ctx.timeSorted.end(); ++elbow) {
        if (ctx.prev(elbow)->point.x() < elbow->point.x() - .035 ||
            ctx.next(elbow)->point.x() < elbow->point.x() - .035)
            continue;
        for (auto ascend : {&SearchContext::prev, &SearchContext::next}) {
            const auto descend = ascend == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
            auto tip = elbow;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                auto next = (ctx.*ascend)(tip);
                if (next->point.y() > tip->point.y() + .08 || next->point.x() < tip->point.x() - .12)
                    break;
                tip = next;
            }
            const auto blade = tip->point - elbow->point;
            if (blade.x() < form.value("minReach").toDouble(3.5) || -blade.y() < form.value("minRise").toDouble(3))
                continue;
            // Ignore the short shoulder across the pen nib before following the
            // longer descending branch to its turn.
            auto turn = elbow;
            bool started = false;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                auto next = (ctx.*descend)(turn);
                if (next->point.x() < turn->point.x() - .1 || (started && next->point.y() < turn->point.y() - .15))
                    break;
                if (next->point.y() - elbow->point.y() > 1.8)
                    started = true;
                turn = next;
            }
            const auto arm = turn->point - elbow->point;
            if (arm.x() < 1.3 || arm.y() < 1.8 || arm.y() > 1.6 * (-blade.y()))
                continue;
            for (auto fork = ctx.timeSorted.begin(); fork != ctx.timeSorted.end(); ++fork) {
                if (!fork->isSharp)
                    continue;
                const auto d = fork->point - turn->point;
                if (d.y() >= -.12 || d.y() < -3.4 || std::abs(d.x()) > 2.4)
                    continue;
                const auto a = ctx.prev(fork)->point - fork->point, b = ctx.next(fork)->point - fork->point;
                if (a.y() < -.07 || b.y() < -.07 || std::max(a.x(), b.x()) < .55)
                    continue;
                const bool forward = a.x() > b.x(); // prev walks towards increasing Bezier time
                Stretching local{};
                if (!contourNormalCut(ctx, *fork, forward, false, local))
                    continue;
                const double width = local.referenceWidth;
                if (width <= .08 || width > 3.5)
                    continue;
                bodies.push_back({*elbow, *tip, *turn, *fork, forward, width});
            }
        }
    }
    return bodies;
}

std::vector<std::pair<Stretching, Stretching>>
cutBlades(SearchContext &ctx, const std::vector<BladeBody> &bodies, const QJsonObject &form) {
    std::vector<std::pair<Stretching, Stretching>> pairs;
    for (const auto &body : bodies) {
        const auto *elbow = &body.elbow, *tip = &body.tip, *turn = &body.turn, *fork = &body.fork;
        const bool forward = body.forward;
        const double width = body.width;
        auto rightPoint = *fork;
        auto limit = rightPoint;
        limit.time += forward ? int(baseContourCurves(ctx.transformedPath).size())
                              : -int(baseContourCurves(ctx.transformedPath).size());
        QPointF tangent;
        const double offset = form.value("margin").toDouble(.5);
        if (offset > 0 &&
            !advanceOnContour(ctx.transformedPath, *fork, limit, forward, width * offset, rightPoint, tangent))
            continue;
        for (auto right : cornerSections(ctx, rightPoint)) {
            orientSection(ctx, right, tip->time, true);
            const auto entry = getJoinTime(right, ctx.orientation);
            if (!entry.isStrictLeft(elbow->time) || !entry.isStrictLeft(turn->time) ||
                QLineF(right.topBaseLine.point, right.bottomBaseLine.point).length() > 2.3 * width)
                continue;
            for (auto left : cornerSections(ctx, *turn)) {
                orientSection(ctx, left, tip->time, false);
                const auto exit = getJoinTime(left, ctx.orientation);
                if (!exit.isStrictRight(elbow->time) || !entry.isStrictLeft(left.topBaseLine.time) ||
                    !entry.isStrictLeft(left.bottomBaseLine.time) ||
                    QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length() > 2.8 * width)
                    continue;
                const auto parts = cutRegions(ctx.transformedPath, {}, {right, left}, ctx.orientation, true);
                if (parts.size() == 3 && toSubpaths(parts[1].path).size() == 1)
                    pairs.emplace_back(right, left);
            }
        }
    }
    return pairs;
}

std::vector<EnclosedLipsBody> recognizeEnclosedLips(const InkOutline &outline, const QJsonObject &settings) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    const bool initial = settings.value("freeEntry").toBool();
    std::vector<EnclosedLipsBody> bodies;
    for (size_t i = 1; i < ctx.subPaths.size(); ++i) {
        const auto& hole = ctx.subPaths[i];
        const auto box = hole.boundingRect();
        const double h = box.height();
        if (h <= 0 || box.width() < settings.value("minAspect").toDouble(initial ? 1.25 : 1.6) * h ||
            !ctx.subPaths[0].contains(hole))
            continue;
        for (auto nose = ctx.timeSorted.begin(); nose != ctx.timeSorted.end(); ++nose) {
            if (initial) {
                if (nose->time != ctx.xSorted.begin()->time || nose->point.x() < box.right() + .3 * h ||
                    nose->point.x() > box.right() + 4 * h || nose->point.y() < box.top() - h ||
                    nose->point.y() > box.bottom() + h)
                    continue;
            } else {
                const auto d = nose->point - QPointF(box.right(), box.center().y());
                if (d.x() < .35 * h || d.x() > 3.7 * h || d.y() < -1.2 * h || d.y() > .75 * h ||
                    ctx.prev(nose)->point.x() > nose->point.x() + 1e-7 ||
                    ctx.next(nose)->point.x() > nose->point.x() + 1e-7)
                    continue;
            }
            EnclosedLipsBody body{box, *nose, {}};
            for (const auto &corner : ctx.timeSorted) {
                if (!corner.isSharp)
                    continue;
                const auto p = corner.point;
                if (initial) {
                    if (p.x() <= box.left() + .35 * h &&
                        QLineF(p, box.bottomLeft()).length() <= settings.value("radius").toDouble(2) * h)
                        body.corners[1].push_back(corner);
                    continue;
                }
                for (int side : {0, 1}) {
                    const bool entry = side == 0;
                    const double scale = settings.value("radius").toDouble(1.7), x = p.x(), y = p.y();
                    if (entry ? (x < box.right() - .1 * h || x > nose->point.x() + .6 * h || y < box.top() - .5 * h ||
                                 y > box.bottom() + .6 * h)
                              : (x > box.left() + .3 * h || x < box.left() - scale * h || y < box.top() - .5 * h ||
                                 y > box.bottom() + .8 * h))
                        continue;
                    body.corners[side].push_back(corner);
                }
            }
            bodies.push_back(std::move(body));
        }
    }
    return bodies;
}

std::vector<std::pair<Stretching, Stretching>> cutEnclosedLips(SearchContext &ctx,
                                                                      const std::vector<EnclosedLipsBody> &bodies,
                                                                      const QJsonObject &policy, bool initial) {
    std::vector<std::pair<Stretching, Stretching>> result;
    double best = std::numeric_limits<double>::infinity();
    for (const auto &body : bodies) {
        const auto &box = body.counter;
        const auto &nose = body.nose;
        const double h = box.height();
        if (initial) {
            for (const auto &corner : body.corners[1])
                for (auto cut : cornerSections(ctx, corner)) {
                    orientSection(ctx, cut, nose.time, false);
                    const auto parts = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
                    if (parts.size() != 2)
                        continue;
                    const auto paths = toSubpaths(parts[0].path);
                    if (paths.size() != 2 || paths[1].boundingRect() != box ||
                        paths[0].boundingRect().height() > 3.4 * h)
                        continue;
                    const double score = QLineF(corner.point, box.bottomLeft()).length() +
                                         .35 * QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
                    if (score < best) {
                        best = score;
                        result = {{Stretching{}, cut}};
                    }
                }
            continue;
        }
        std::vector<std::pair<double, Stretching>> sides[2];
        for (int side : {0, 1})
            for (const auto &corner : body.corners[side]) {
                const bool entry = side == 0;
                for (auto cut : cornerSections(ctx, corner)) {
                    const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
                    if (line.length() > policy.value("maxSectionWidth").toDouble(1.5) * h)
                        continue;
                    orientSection(ctx, cut, nose.time, entry);
                    const auto pieces = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
                    if (pieces.size() != 2 ||
                        !pieces[entry ? 1 : 0].path.boundingRect().adjusted(-1e-7, -1e-7, 1e-7, 1e-7).contains(box))
                        continue;
                    const auto aim = entry ? QPointF(nose.point.x() - .45 * h, box.center().y()) : box.bottomLeft();
                    sides[side].emplace_back(QLineF(corner.point, aim).length() + .35 * line.length(), cut);
                }
            }
        for (auto &side : sides)
            std::sort(side.begin(), side.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &[sr, right] : sides[0])
            for (const auto &[sl, left] : sides[1]) {
                const auto boundary = getJoinTime(right, ctx.orientation);
                if (!boundary.isStrictLeft(left.topBaseLine.time) || !boundary.isStrictLeft(left.bottomBaseLine.time))
                    continue;
                const auto parts = cutRegions(ctx.transformedPath, {}, {right, left}, ctx.orientation, true);
                if (parts.size() != 3)
                    continue;
                const auto paths = toSubpaths(parts[1].path);
                if (paths.size() != 2 || paths[1].boundingRect() != box)
                    continue;
                const auto bounds = paths[0].boundingRect();
                if (bounds.height() > 3.6 * h || bounds.width() > box.width() + 4 * h)
                    continue;
                result.emplace_back(right, left);
            }
    }
    return result;
}

std::optional<ComponentArmBody> recognizeArmEnvelope(const QPainterPath &ink, double minRise, bool leaning) {
    const auto box = ink.boundingRect();
    if (box.height() < minRise || box.width() < .25 * box.height() || box.width() > 1.7 * box.height())
        return {};
    QVector<PathExtrema> extrema;
    std::vector<QPainterPath> outlines;
    ShapeItem::getExtrema(ink, outlines, extrema);
    if (extrema.isEmpty())
        return {};
    const auto byY = [](const auto &a, const auto &b) { return a.point.y() < b.point.y(); };
    const auto [top, bottom] = std::minmax_element(extrema.begin(), extrema.end(), byY);
    if (leaning && bottom->point.x() - top->point.x() < .45 * box.width())
        return {};
    return ComponentArmBody{0, box, *top, *bottom};
}

std::vector<ComponentArmBody> recognizeComponentArms(const InkOutline &outline, const QJsonObject &body) {
    std::vector<ComponentArmBody> result;
    for (size_t i = 1; i < outline.contours.size(); ++i) {
        auto arm = recognizeArmEnvelope(outline.contours[i], body.value("minRise").toDouble(3), true);
        if (!arm)
            continue;
        arm->index = int(i);
        arm->top.pathIndex = arm->bottom.pathIndex = int(i);
        result.push_back(*arm);
    }
    return result;
}

std::vector<InkComponentBody> recognizeSourceComponents(const QPainterPath &ink) {
    const auto paths = toSubpaths(ink);
    std::vector<InkComponentBody> pieces;
    if (paths.size() < 2)
        return pieces;
    for (int i = 0; i < paths.size(); ++i) {
        bool inside = false;
        for (int j = 0; j < paths.size() && !inside; ++j)
            inside = i != j && paths[j].contains(paths[i]);
        if (!inside)
            pieces.push_back({paths[i], {i}});
    }
    if (pieces.size() != 2 || pieces[0].ink.intersects(pieces[1].ink))
        return {};
    for (auto &piece : pieces) {
        const auto outer = piece.ink;
        for (int i = 0; i < paths.size(); ++i)
            if (i != piece.indices.front() && outer.contains(paths[i])) {
                piece.ink.addPath(paths[i]);
                piece.indices.push_back(i);
            }
        piece.ink.setFillRule(ink.fillRule());
    }
    return pieces;
}

} // namespace MedinaCutter
