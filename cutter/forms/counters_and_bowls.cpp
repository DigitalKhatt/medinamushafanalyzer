#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

std::function<bool(xsortedtype::iterator)> defaultCheck = [](xsortedtype::iterator xiter) {return true; };

static bool traceNeckFlanks(SearchContext &context, const JoinCheck &joinCheck) {
    auto &res = context.res;
    const auto &timeSorted = context.timeSorted;
    const auto &transformedPath = context.transformedPath;
    constexpr double minDistance = .01;
    auto item = timeSorted.find(res.bottomBaseLine);
    auto prev = item;
    auto next = item;

    int nbTentative = 3;

    while (nbTentative != 0) {
        nbTentative--;
        prev = context.prev(prev);

        if (prev == timeSorted.end()) {
            prev = --timeSorted.end();
        }

        if (std::abs(res.bottomBaseLine.point.x() - prev->point.x()) > minDistance ||
            std::abs(res.bottomBaseLine.point.y() - prev->point.y()) > minDistance) {
            break;
        }
    }
    nbTentative = 3;

    while (nbTentative != 0) {
        nbTentative--;

        next = std::next(next);

        if (next == timeSorted.end()) {
            next = timeSorted.begin();
        }

        if (std::abs(res.bottomBaseLine.point.x() - next->point.x()) > minDistance ||
            std::abs(res.bottomBaseLine.point.y() - next->point.y()) > minDistance) {
            break;
        }
    }

    bool bottomClockWiseOrder;
    if (prev->point.x() < next->point.x()) {
        res.bottomLeft = *prev;
        res.bottomRight = *next;
        bottomClockWiseOrder = true;
    } else {
        res.bottomRight = *prev;
        res.bottomLeft = *next;
        bottomClockWiseOrder = false;
    }

    if (joinCheck.bottomCheck) {
        if (res.bottomLeft.point.x() > res.bottomBaseLine.point.x() ||
            res.bottomRight.point.x() < res.bottomBaseLine.point.x() ||
            res.bottomLeft.point.y() > res.bottomBaseLine.point.y() ||
            res.bottomRight.point.y() > res.bottomBaseLine.point.y()) {

            return false;
        }
    }

    item = timeSorted.find(res.topBaseLine);
    prev = item;
    next = item;

    nbTentative = 3;

    while (nbTentative != 0) {
        nbTentative--;

        next = std::next(next);

        if (next == timeSorted.end()) {
            next = timeSorted.begin();
        }

        if (std::abs(res.topBaseLine.point.x() - next->point.x()) > minDistance ||
            std::abs(res.topBaseLine.point.y() - next->point.y()) > minDistance) {
            break;
        }
    }

    nbTentative = 3;

    while (nbTentative != 0) {
        nbTentative--;

        prev = context.prev(prev);

        if (prev == timeSorted.end()) {
            prev = --timeSorted.end();
        }

        if (std::abs(res.topBaseLine.point.x() - prev->point.x()) > minDistance ||
            std::abs(res.topBaseLine.point.y() - prev->point.y()) > minDistance) {
            break;
        }
    }

    bool topClockWiseOrder;

    if (prev->point.x() < next->point.x()) {
        res.topLeft = *prev;
        res.topRight = *next;
        topClockWiseOrder = false;
    } else {
        res.topRight = *prev;
        res.topLeft = *next;
        topClockWiseOrder = true;
    }

    if (joinCheck.topCheck) {
        if (res.topLeft.point.x() > res.topBaseLine.point.x() || res.topRight.point.x() < res.topBaseLine.point.x() ||
            res.topLeft.point.y() > res.topBaseLine.point.y() || res.topRight.point.y() > res.topBaseLine.point.y()) {

            return false;
        }
    }

    if (joinCheck.sameDirectionCheck) {
        if (topClockWiseOrder != bottomClockWiseOrder) {

            return false;
        }
    }

    res.clockWise = topClockWiseOrder;

    //check if joining segment belongs to the interior of the path area
    QLineF segment{res.topBaseLine.point, res.bottomBaseLine.point};

    if (!transformedPath.contains(segment.center())) {

        return false;
    }

    return true;
}

std::vector<NeckBody> recognizeNeckBodies(const InkOutline &outline, const QJsonObject &bounds,
                                                 const JoinCheck &joinCheck) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    context.res = {};
    std::vector<NeckBody> bodies;
    const int plookbackward = bounds.value("lookback").toInt(4);
    const int prevOff = bounds.value("previousOffset").toInt(1);
    if (plookbackward < 1 || prevOff < 1)
        return {};

    auto xiter = context.xSorted.begin();
    auto &xSorted = context.xSorted;
    auto &res = context.res;

    for (; xiter != xSorted.end(); xiter++) {
        auto &curr = *xiter;

        auto prevBasePoint = xiter;
        int offset = prevOff;
        while (offset > 0 && prevBasePoint != xSorted.begin()) {
            --prevBasePoint;
            --offset;
        }
        if (offset > 0)
            continue;

        auto lookbackward = plookbackward;

        while (lookbackward != 0 && prevBasePoint != xSorted.end()) {

            auto &prec = *prevBasePoint;

            prevBasePoint = prevBasePoint == xSorted.begin() ? xSorted.end() : std::prev(prevBasePoint);

            auto yDiff = std::abs(curr.point.y() - prec.point.y());
            auto xDiff = std::abs(curr.point.x() - prec.point.x());

            if (!within(bounds, "minX", "maxX", xDiff) || !within(bounds, "minY", "maxY", yDiff)) {
                lookbackward--;
                continue;
            }

            if (prec.point.y() > curr.point.y()) {
                res.bottomBaseLine = prec;
                res.topBaseLine = curr;

            } else {
                res.bottomBaseLine = curr;
                res.topBaseLine = prec;
            }

            if (!traceNeckFlanks(context, joinCheck))
                continue;

            auto rightKash = std::min(res.topRight.point.x(), res.bottomRight.point.x());
            auto leftKash = std::max(res.topLeft.point.x(), res.bottomLeft.point.x());

            res.length = rightKash - leftKash;

            bodies.push_back({curr, res, {}});
        }
    }

    return bodies;
}

std::optional<BowlProbeBody> recognizeBowlProbe(const InkOutline &outline, const Stretching &section,
                                                       int lookahead) {
    const auto &points = outline.time;
    auto it = points.find(section.topBaseLine);
    if (it == points.end())
        return {};
    const auto advance = [&](timesortedtype::iterator current) {
        return section.clockWise ? (current == points.begin() ? points.end() : std::prev(current)) : std::next(current);
    };
    auto previous = it, current = advance(it);
    while (current != points.end() && current->point.y() <= previous->point.y()) {
        previous = current;
        current = advance(current);
    }
    if (current == points.end())
        return {};
    BowlProbeBody body{*previous, {}};
    current = advance(current);
    while (current != points.end() && !(!current->isSharp && current->isY) && lookahead != 0) {
        current = advance(current);
        --lookahead;
    }
    if (current != points.end() && lookahead != 0)
        body.smooth = *current;
    return body;
}

bool cutBowlNecks(SearchContext &context, const std::vector<NeckBody> &bodies, const QJsonObject &policy) {
    if (context.nextIter == context.xSorted.end())
        return false;
    std::vector<const NeckBody *> selected;
    auto cursor = context.nextIter;
    for (const auto &body : bodies) {
        const auto position = context.xSorted.find(body.cursor);
        if (position == context.xSorted.end() || context.xSorted.key_comp()(*position, *cursor))
            continue;
        if (!context.subword.joins.empty()) {
            const auto &previous = *context.subword.joins.rbegin();
            const auto a = body.section.topBaseLine.time, b = body.section.bottomBaseLine.time;
            if (previous.topBaseLine.time == a || previous.bottomBaseLine.time == a || previous.topBaseLine.time == b ||
                previous.bottomBaseLine.time == b)
                continue;
        }
        selected.push_back(&body);
        cursor = std::next(position);
        if (cursor == context.xSorted.end())
            break;
    }
    if (selected.empty())
        return false;
    if (selected.size() >= 2) {
        const auto &body = *selected[selected.size() - 2];
        context.res = body.section;
        context.nextIter = std::next(context.xSorted.find(body.cursor));
        return true;
    }
    const auto &body = *selected.front();
    if (!body.probe)
        return false;
    const double x = body.probe->smooth ? body.probe->smooth->point.x()
                                        : body.probe->extremum.point.x() + policy.value("offsetX").toDouble(1.2);
    auto hits = FindInterections(context.transformedPath, QLineF(x, context.bbox.top(), x, context.bbox.bottom()));
    if (hits.size() < std::max(2., policy.value("minCrossings").toDouble(2)))
        return false;
    std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.point.y() < b.point.y(); });
    context.res = body.section;
    context.res.topBaseLine = hits[0];
    context.res.bottomBaseLine = hits[1];
    context.res.length = 0;
    context.nextIter = cursor;
    return true;
}

std::vector<NeckBody> recognizeProfileNecks(const InkOutline &outline, const QJsonObject &body) {
    const auto neck = body.value("neck").toObject(), profile = body.value("profile").toObject();
    JoinCheck checks;
    checks.bottomCheck = neck.value("bottomCheck").toBool(true);
    checks.topCheck = neck.value("topCheck").toBool(true);
    checks.sameDirectionCheck = neck.value("sameDirectionCheck").toBool(true);
    auto candidates = recognizeNeckBodies(outline, neck, checks);
    const auto strip = profile.value("strip").toObject();
    candidates.erase(
        std::remove_if(
            candidates.begin(), candidates.end(),
            [&](const auto &candidate) {
                if (!matchesNeighbors(neck.value("neighbors").toArray(), candidate.section))
                    return true;
                if (!strip.isEmpty()) {
                    const double x = candidate.section.topLeft.point.x(), radius = strip.value("radius").toDouble(.2);
                    QPainterPath band;
                    band.addRect(QRectF(QPointF(x - radius, outline.bounds.top()),
                                        QPointF(x + radius, outline.bounds.bottom())));
                    band.setFillRule(Qt::WindingFill);
                    const auto intersected = outline.ink.intersected(band);
                    int count = 0;
                    for (int i = 0; i < intersected.elementCount(); ++i)
                        count += intersected.elementAt(i).isMoveTo();
                    if (count != strip.value("count").toInt(2))
                        return true;
                }
                if (profile.contains("minLandmarkCrossings")) {
                    const auto &section = candidate.section;
                    const double x = section.topBaseLine.point.x();
                    auto hits =
                        FindInterections(outline.ink, QLineF(x, outline.bounds.top(), x, outline.bounds.bottom()));
                    if (std::none_of(hits.begin(), hits.end(), [&](const auto &hit) {
                            return QLineF(hit.point, section.topBaseLine.point).length() <= 1e-5;
                        }))
                        hits.push_back(section.topBaseLine);
                    if (hits.size() < profile.value("minLandmarkCrossings").toInt() ||
                        !interiorChord(outline.ink, section.topBaseLine, section.bottomBaseLine))
                        return true;
                }
                if (profile.contains("centerCrossings")) {
                    const double x = QLineF(candidate.section.topBaseLine.point, candidate.section.bottomBaseLine.point)
                                         .center()
                                         .x();
                    if (FindInterections(outline.ink, QLineF(x, outline.bounds.top(), x, outline.bounds.bottom()))
                            .size() != profile.value("centerCrossings").toInt())
                        return true;
                }
                const auto clear = profile.value("clearBelow").toObject();
                if (!clear.isEmpty()) {
                    const auto anchor = candidate.section.bottomBaseLine.point;
                    const double top = anchor.y() + clear.value("offsetY").toDouble(.1),
                                 height = outline.bounds.bottom() - top;
                    if (height > 0 &&
                        outline.ink.intersects(QRectF(anchor.x(), top, clear.value("width").toDouble(1), height)))
                        return true;
                }
                return false;
            }),
        candidates.end());
    if (profile.contains("edge")) {
        WordResultInfo::SubWordInfo empty;
        auto geometry = outline.geometry(empty);
        for (auto &candidate : candidates) {
            const QLineF chord(candidate.section.topBaseLine.point, candidate.section.bottomBaseLine.point);
            const double stroke = chord.length();
            if (!(stroke > 0))
                continue;
            for (auto at = geometry.timeSorted.begin(); at != geometry.timeSorted.end(); ++at)
                if (matchesOffset(profile.value("edge").toObject(), (at->point - chord.center()) / stroke) &&
                    matchesOffset(profile.value("edgeFlank").toObject(),
                                  (geometry.prev(at)->point - at->point) / stroke) &&
                    matchesOffset(profile.value("edgeFlank").toObject(),
                                  (geometry.next(at)->point - at->point) / stroke))
                    candidate.edges.push_back(*at);
        }
    }
    return candidates;
}

bool cutProfileNeck(SearchContext &context, const std::vector<NeckBody> &candidates,
                           const NeckBody **selected, const QJsonObject &probe) {
    if (context.nextIter == context.xSorted.end())
        return false;
    for (const auto &candidate : candidates) {
        const auto cursor = context.xSorted.find(candidate.cursor);
        if (cursor == context.xSorted.end() || context.xSorted.key_comp()(*cursor, *context.nextIter))
            continue;
        if (!context.subword.joins.empty()) {
            const auto &previous = *context.subword.joins.rbegin();
            const auto a = candidate.section.topBaseLine.time, b = candidate.section.bottomBaseLine.time;
            if (previous.topBaseLine.time == a || previous.bottomBaseLine.time == a || previous.topBaseLine.time == b ||
                previous.bottomBaseLine.time == b)
                continue;
        }
        context.res = candidate.section;
        if (!probe.isEmpty()) {
            const double x = context.res.topBaseLine.point.x() + probe.value("offsetX").toDouble();
            auto hits =
                FindInterections(context.transformedPath, QLineF(x, context.bbox.top(), x, context.bbox.bottom()));
            if (hits.size() < 2)
                continue;
            std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.point.y() > b.point.y(); });
            context.res.bottomBaseLine = hits[0];
            context.res.topBaseLine = hits[1];
            if (!matchesJoinFeatures(context))
                continue;
        }
        context.nextIter = std::next(cursor);
        if (selected)
            *selected = &candidate;
        return true;
    }
    return false;
}

static double inwardTurnAngle(const QPointF& before, const QPointF& after, Orientation orientation) {
    const double cross = before.x() * after.y() - before.y() * after.x();
    const double inward = orientation == Orientation::CounterClockwise ? cross : -cross;
    return std::atan2(inward, QPointF::dotProduct(before, after)) * 180 / std::acos(-1.);
}

static double inwardCornerAngle(const std::vector<QBezier>& curves, const PathExtrema& point,
                                Orientation orientation) {
    if (std::abs(point.time - std::round(point.time)) > 1e-6)
        return 0;
    const size_t index = size_t(std::lround(point.time)) % curves.size();
    const auto& incoming = curves[(index + curves.size() - 1) % curves.size()];
    const auto& outgoing = curves[index];
    QPointF a, b;
    for (const auto& control : {incoming.pt3(), incoming.pt2(), incoming.pt1()}) {
        a = point.point - control;
        if (!a.isNull()) break;
    }
    for (const auto& control : {outgoing.pt2(), outgoing.pt3(), outgoing.pt4()}) {
        b = control - point.point;
        if (!b.isNull()) break;
    }
    return inwardTurnAngle(a, b, orientation);
}

static double roundedInwardAngle(const QPainterPath& ink, const PathExtrema& point,
                                 int curveCount, double distance, Orientation orientation) {
    auto limit = point;
    limit.time -= curveCount;
    PathExtrema sample;
    QPointF before, after;
    if (!advanceOnContour(ink, point, limit, false, distance, sample, before)) return 0;
    limit.time = point.time + curveCount;
    if (!advanceOnContour(ink, point, limit, true, distance, sample, after)) return 0;
    return inwardTurnAngle(before, after, orientation);
}

std::optional<BowlTurnBody> recognizeBowlTurn(const InkOutline &outline, const QJsonObject &junction) {
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    if (geometry.xSorted.empty())
        return {};
    const auto first = std::prev(geometry.xSorted.end());
    auto current = geometry.timeSorted.find(*first);
    const auto start = current;
    const auto step =
        geometry.prev(current)->point.y() < current->point.y() ? &SearchContext::next : &SearchContext::prev;
    do {
        const auto next = (geometry.*step)(current);
        if (next->point.y() < current->point.y() && (current == start || next->point.y() < start->point.y())) {
            current = next;
            break;
        }
        current = next;
    } while (current != start);
    if (current == start) return {};

    BowlTurnBody result{*first, *current, {}};
    if (junction.isEmpty()) return result;
    const auto holes = recognizeCounterHoles(outline);
    if (holes.empty()) return result;
    const auto hole = std::ranges::min_element(holes, {}, [](const auto& h) { return h.box.center().x(); })->box;
    const auto curves = baseContourCurves(outline.ink);
    if (curves.empty()) return result;
    const double minAngle = junction.value("minCornerAngle").toDouble();
    const double reach = junction.value("maxTurnDistance").toDouble() * hole.height();
    const double sample = junction.value("roundedTurnSample").toDouble() * hole.height();
    const auto isCorner = [&](const PathExtrema& p) {
        return p.point.x() >= hole.center().x() &&
            inwardCornerAngle(curves, p, outline.orientation) >= minAngle;
    };
    const auto isRoundedTurn = [&](const PathExtrema& point) {
        if (point.point.x() < hole.center().x() || point.point.x() > hole.right() + reach) return false;
        // Compare the tangents on either side of the bend. Arc length keeps
        // short Bézier handles from determining the direction of a whole flank.
        return roundedInwardAngle(outline.ink, point, curves.size(), sample, outline.orientation) >= minAngle;
    };

    const auto matchUpper = [&](PathExtrema lower, bool lowerCorner, bool allowRounded) {
        decltype(result.junction) pair;
        lower.isSharp = lowerCorner;
        double shortest = junction.value("maxWidth").toDouble() * hole.height();
        for (auto upper : outline.time) {
            const auto span = lower.point - upper.point;
            const double width = std::hypot(span.x(), span.y());
            if (span.y() <= 0 || std::abs(span.x()) > span.y() || width >= shortest ||
                upper.point.y() > hole.bottom() + .25 * hole.height())
                continue;
            upper.isSharp = isCorner(upper);
            // At least one flank must give a sharp landmark.
            if (!upper.isSharp && !(allowRounded && lowerCorner && isRoundedTurn(upper))) continue;
            Stretching cut{};
            cut.topBaseLine = upper;
            cut.bottomBaseLine = lower;
            if (!getJoinTime(cut, outline.orientation).isStrictLeft(result.tip.time) ||
                !interiorChord(outline.ink, upper, lower)) continue;
            shortest = width;
            pair = std::pair{upper, lower};
        }
        return pair;
    };
    const auto findJunction = [&](bool allowRounded) {
        auto lower = current;
        double distance = 0;
        // Follow the bowl's lower flank a short distance beyond its turn.
        while (lower != start && distance <= reach) {
            if (lower->point.y() >= hole.center().y()) {
                const bool corner = isCorner(*lower);
                if (corner || (allowRounded && isRoundedTurn(*lower))) {
                    const auto pair = matchUpper(*lower, corner, allowRounded);
                    if (pair || !allowRounded) return pair;
                }
            }
            const auto next = (geometry.*step)(lower);
            distance += QLineF(lower->point, next->point).length();
            lower = next;
        }
        return decltype(result.junction){};
    };
    result.junction = findJunction(false);
    if (!result.junction && sample > 0) result.junction = findJunction(true);
    return result;
}

std::optional<OpenBowlBody> recognizeOpenBowl(const InkOutline& outline, const QJsonObject& body) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    const auto curves = baseContourCurves(outline.ink);
    if (curves.empty() || context.xSorted.empty()) return {};
    const double angle = body.value("minCornerAngle").toDouble();
    // The search inventory merges nearby extrema. A short segment can
    // still end at a genuine corner on the outer edge of the bowl.
    for (size_t i = 0; i < curves.size(); ++i) {
        const PathExtrema corner{0, double(i), curves[i].pt1(), false, false, true};
        if (inwardCornerAngle(curves, corner, outline.orientation) >= angle)
            context.timeSorted.insert(corner);
    }
    const auto& tip = *context.xSorted.rbegin();
    const auto origin = context.timeSorted.find(tip);
    if (origin == context.timeSorted.end()) return {};

    // Walk the outside of the bowl from its free end. That end may curl
    // upward first. The floor is the first low turn beyond the free lip.
    const bool forward = outline.orientation == Orientation::CounterClockwise;
    const auto step = [&](auto point) { return forward ? context.prev(point) : context.next(point); };
    auto floor = step(origin);
    for (; floor != origin; floor = step(floor)) {
        const auto before = forward ? context.next(floor) : context.prev(floor);
        if (floor->isY && floor->point.y() > tip.point.y() &&
            floor->point.y() >= before->point.y() && floor->point.y() > step(floor)->point.y()) break;
    }
    if (floor == origin) return {};
    const auto span = floor->point - tip.point;
    Stretching foot{};
    if (!medialChordAt(context, floor->point.x(), floor->point.y(), 1., foot, true) &&
        !medialChordAt(context, floor->point.x() + .02 * span.x(), floor->point.y(), 1., foot, true)) return {};
    const double stroke = QLineF(foot.topBaseLine.point, foot.bottomBaseLine.point).length();
    if (stroke <= 1e-5 || span.x() < stroke || span.y() < .5 * stroke) return {};

    // The same outside edge rises to the inward turn into the connector.
    // Stop before it folds back or drops into the preceding letter.
    const double sample = body.value("roundedTurnSample").toDouble(.25) * stroke;
    double rightmost = floor->point.x(), highest = floor->point.y();
    std::optional<OpenBowlBody> bowl;
    for (auto point = step(floor); point != origin; point = step(point)) {
        const auto p = point->point;
        if (p.x() < rightmost - .6 * stroke || p.y() > highest + .2 * stroke ||
            (bowl && QLineF(bowl->shoulder.point, p).length() > stroke)) break;
        rightmost = std::max(rightmost, p.x());
        highest = std::min(highest, p.y());
        if (p.x() - floor->point.x() < 1.25 * stroke || floor->point.y() - p.y() < stroke) continue;
        const bool sharp = inwardCornerAngle(curves, *point, outline.orientation) >= angle;
        if (!sharp && (bowl ||
            roundedInwardAngle(outline.ink, *point, curves.size(), sample, outline.orientation) < angle)) continue;
        auto limit = *point;
        limit.time += forward ? double(curves.size()) : -double(curves.size());
        PathExtrema after;
        QPointF tangent;
        if (!advanceOnContour(outline.ink, *point, limit, forward, sample, after, tangent)) continue;
        const auto exit = after.point - p;
        if (exit.x() <= std::abs(exit.y())) continue;
        bowl = OpenBowlBody{tip, *floor, *point, stroke, sample, forward, {}, {}};
        if (sharp) break;
    }
    if (!bowl) return {};

    // Follow the other side of the open bowl. Its inner floor rises
    // into the crest, then descends into the join's upper edge.
    const auto other = [&](auto point) { return forward ? context.next(point) : context.prev(point); };
    const double shoulderX = bowl->shoulder.point.x();
    bool passedInnerFloor = false;
    for (auto point = other(origin); point != origin; point = other(point)) {
        const auto p = point->point;
        if (p.x() > shoulderX + stroke) break;
        const double before = step(point)->point.y(), after = other(point)->point.y();
        const bool valley = p.y() >= before && p.y() > after;
        if (!passedInnerFloor) {
            passedInnerFloor = valley && p.x() > tip.point.x() + stroke && p.y() > floor->point.y() - 2 * stroke;
        } else if (!bowl->crest) {
            if (p.x() >= shoulderX - 2 * stroke && p.y() <= bowl->shoulder.point.y() - .5 * stroke &&
                p.y() <= before && p.y() < after)
                bowl->crest = *point;
        } else if (valley) {
            bowl->exit = *point;
            break;
        }
    }
    return bowl;
}

bool retainsOpenBowl(const SearchContext& context, const OpenBowlBody& body, const Stretching& cut) {
    const auto side = getJoinTime(cut, context.orientation);
    return side.isStrictLeft(body.tip.time) && side.isStrictLeft(body.floor.time) &&
        side.isLeft(body.shoulder.time) && (!body.crest || side.isStrictLeft(body.crest->time));
}

bool cutOpenBowl(SearchContext& context, const OpenBowlBody& body, const QJsonObject& cuts) {
    auto& cut = context.res;
    const auto valid = [&] {
        orientSection(context, cut, body.tip.time, true);
        return cut.referenceWidth >= .5 * body.stroke &&
            cut.referenceWidth <= cuts.value("maxWidth").toDouble() * body.stroke && retainsOpenBowl(context, body, cut);
    };
    if (!contourNormalCut(context, body.shoulder, body.forward, false, cut, true, body.tangentSample) || !valid()) {
        if (!body.exit || !interiorChord(context.transformedPath, *body.exit, body.shoulder)) return false;
        cut.topBaseLine = *body.exit;
        cut.bottomBaseLine = body.shoulder;
        cut.referenceWidth = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
        if (!valid()) return false;
    }
    cut.fixedRightBoundary = true;
    cut.length = 0;
    context.nextIter = context.xSorted.find(body.tip);
    return true;
}

bool refineContourJunction(SearchContext& context, const QJsonObject& policy) {
    const auto& ink = context.transformedPath;
    auto seed = context.res;
    if (seed.topBaseLine.pathIndex != 0 || seed.bottomBaseLine.pathIndex != 0) return false;
    if (seed.topBaseLine.point.y() > seed.bottomBaseLine.point.y())
        std::swap(seed.topBaseLine, seed.bottomBaseLine);
    const double stroke = QLineF(seed.topBaseLine.point, seed.bottomBaseLine.point).length();
    const auto curves = baseContourCurves(ink);
    if (stroke <= 1e-5 || curves.empty()) return false;
    const double reach = policy.value("maxTurnDistance").toDouble() * stroke;
    const double sample = policy.value("roundedTurnSample").toDouble() * stroke;
    const double minAngle = policy.value("minCornerAngle").toDouble();
    const double maxWidth = policy.value("maxWidth").toDouble() * stroke;
    const auto onFlank = [&](const PathExtrema& anchor, PathExtrema target) {
        if (std::abs(target.time - anchor.time) < 1e-8) return true;
        // Two outlines can be close in space at a folded bowl. Follow the
        // source contour to make sure this point belongs to the same flank.
        for (bool forward : {true, false}) {
            auto limit = target;
            if (forward != (limit.time > anchor.time))
                limit.time += forward ? double(curves.size()) : -double(curves.size());
            PathExtrema at;
            QPointF tangent;
            if (!advanceOnContour(ink, anchor, limit, forward, reach + 1e-6, at, tangent)) return true;
        }
        return false;
    };
    const auto landmarks = [&](const PathExtrema& anchor) {
        std::vector<PathExtrema> points;
        for (auto point : context.timeSorted) {
            if (QLineF(anchor.point, point.point).length() > reach) continue;
            point.isSharp = inwardCornerAngle(curves, point, context.orientation) >= minAngle;
            if (!point.isSharp && !(sample > 0 &&
                roundedInwardAngle(ink, point, curves.size(), sample, context.orientation) >= minAngle)) continue;
            if (onFlank(anchor, point)) points.push_back(point);
        }
        return points;
    };
    const auto& tip = *context.xSorted.rbegin();
    auto oriented = seed;
    orientSection(context, oriented, tip.time, true);
    const auto originalSide = getJoinTime(oriented, context.orientation);
    const PathExtrema* floor = &tip;
    for (const auto& point : context.timeSorted)
        if (originalSide.isLeft(point.time) && point.point.y() > floor->point.y()) floor = &point;

    auto bestScore = std::pair{2, std::numeric_limits<double>::infinity()};
    auto best = seed;
    const auto upper = landmarks(seed.topBaseLine), lower = landmarks(seed.bottomBaseLine);
    for (const auto& top : upper) {
        for (const auto& bottom : lower) {
            if (!top.isSharp && !bottom.isSharp) continue;
            const auto span = bottom.point - top.point;
            const double width = std::hypot(span.x(), span.y());
            if (span.y() <= 0 || std::abs(span.x()) > span.y() || width < .5 * stroke || width > maxWidth) continue;
            const auto score = std::pair{int(!top.isSharp) + int(!bottom.isSharp),
                QLineF(seed.topBaseLine.point, top.point).length() +
                QLineF(seed.bottomBaseLine.point, bottom.point).length()};
            if (score >= bestScore || !interiorChord(ink, top, bottom)) continue;
            auto cut = seed;
            cut.topBaseLine = top;
            cut.bottomBaseLine = bottom;
            orientSection(context, cut, tip.time, true);
            cut.fixedRightBoundary = true;
            cut.length = 0;
            const auto side = getJoinTime(cut, context.orientation);
            if (!side.isStrictLeft(tip.time) || !side.isLeft(floor->time)) continue;
            bestScore = score;
            best = cut;
        }
    }
    if (bestScore.first == 2 || (best.topBaseLine.point == oriented.topBaseLine.point &&
        best.bottomBaseLine.point == oriented.bottomBaseLine.point)) return false;
    context.res = best;
    return true;
}

bool cutBowlTurn(SearchContext &context, const BowlTurnBody &body, const QJsonObject &cuts) {
    auto &res = context.res;
    if (cuts.value("boundaries").toObject().value("right") == "corner-junction" && body.junction) {
        res.topBaseLine = body.junction->first;
        res.bottomBaseLine = body.junction->second;
        res.fixedRightBoundary = true;
        inspectLandmark(res.topBaseLine.isSharp ? "Upper junction" : "Rounded upper turn", res.topBaseLine);
        inspectLandmark(res.bottomBaseLine.isSharp ? "Lower junction" : "Rounded lower turn", res.bottomBaseLine);
    } else {
        const auto policy = cuts.value("probe").toObject();
        const auto p = body.turn.point;
        const double x = p.x() + policy.value("probeX").toDouble(.2), radius = policy.value("probeRadius").toDouble(2);
        const auto hits = FindInterections(context.transformedPath, QLineF(x, p.y() - radius, x, p.y() + radius));
        if (hits.size() < std::max(2., policy.value("minCrossings").toDouble(2)))
            return false;
        res.bottomBaseLine = hits.end()[-1];
        res.topBaseLine = hits.end()[-2];
    }
    res.bottomLeft = res.bottomRight = res.bottomBaseLine;
    res.topLeft = res.topRight = res.topBaseLine;
    res.length = 0;
    context.nextIter = context.xSorted.find(body.tip);
    return true;
}

LayeredHeadBody recognizeLayeredHeads(const InkOutline &outline, const QJsonObject &body) {
    LayeredHeadBody result;
    const auto profile = body.value("head").toObject();
    auto necks = recognizeNeckBodies(outline, body.value("neck").toObject());
    for (auto &neck : necks) {
        const auto interval = getJoinTime(neck.section);
        int extrema = 0;
        for (const auto &point : outline.time)
            extrema += interval.isStrictRight(point.time);
        if (extrema >= profile.value("minExtrema").toInt(8))
            result.necks.push_back(std::move(neck));
    }
    const double x = outline.bounds.right() - profile.value("ascenderProbeX").toDouble(6);
    auto hits = FindInterections(outline.ink, QLineF(x, outline.bounds.top(), x, outline.bounds.bottom()));
    if (hits.size() >= 2 && hits.size() == profile.value("ascenderCrossings").toInt(4)) {
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.point.y() < b.point.y(); });
        for (const auto &path : outline.contours)
            if (path.contains(hits[1].point))
                result.envelopes.push_back(path.boundingRect());
    }
    return result;
}

bool cutLayeredHead(SearchContext &context, const LayeredHeadBody &body, const QJsonObject &policy) {
    for (const auto &envelope : body.envelopes) {
        const double x = envelope.left() - policy.value("counterProbeX").toDouble(.6);
        auto hits = FindInterections(context.transformedPath, QLineF(x, context.bbox.top(), x, context.bbox.bottom()));
        if (hits.size() < 2 || hits.size() != policy.value("counterCrossings").toInt(2))
            continue;
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.point.y() > b.point.y(); });
        context.res.bottomBaseLine = context.res.bottomLeft = context.res.bottomRight = hits[0];
        context.res.topBaseLine = context.res.topLeft = context.res.topRight = hits[1];
        context.res.length = 0;
        return true;
    }
    return cutProfileNeck(context, body.necks);
}

std::optional<HorizontalCounterPair> recognizeHorizontalPair(const InkOutline &outline) {
    const auto &ink = outline.ink;
    std::vector<QRectF> counters;
    for (size_t i = 1; i < outline.contours.size(); ++i) {
        const auto hole = outline.contours[i].boundingRect();
        if (hole.width() > 0 && hole.height() > 0 && !ink.contains(hole.center()))
            counters.push_back(hole);
    }
    if (counters.size() < 2)
        return std::nullopt;
    std::sort(counters.begin(), counters.end(), [](const auto& a, const auto& b) {
        return a.center().x() > b.center().x();
    });
    const auto& right = counters[0];
    const auto& left = counters[1];
    const double height = (right.height() + left.height()) / 2;
    if (right.left() - left.right() > height ||
        std::min(right.bottom(), left.bottom()) <= std::max(right.top(), left.top()))
        return std::nullopt;

    return HorizontalCounterPair{right, left, height};
}

bool cutHorizontalPair(SearchContext &context, const HorizontalCounterPair &body, const QJsonObject &policy) {
    const auto &right = body.right;
    const auto &left = body.left;
    const double height = body.height;
    const double x = left.left() - height * policy.value("counterOffset").toDouble(0.35);
    const double y = std::max(right.bottom(), left.bottom()) + height * policy.value("baselineOffset").toDouble(0.6);
    Stretching cut{};
    if (!medialChordAt(context, x, y, height, cut)) return false;
    context.res = cut;
    context.res.topLeft = context.res.topRight = cut.topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = cut.bottomBaseLine;
    context.res.fixedRightBoundary = true;
    context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto& point) {
        return point.point.x() < x;
    });
    return true;
}

CounterCornerEvidence recognizeCounterCorners(const InkOutline &outline, const QRectF &counter,
                                                     const QJsonObject &settings) {
    CounterCornerEvidence result;
    result.counter = counter;
    if (settings.isEmpty())
        return result;
    result.maxDistance = settings.value("maxDistance").toDouble(1.5) * counter.height();
    result.upper = settings.value("placement") == "upper";
    result.top = settings.value("placement") == "top-right";
    result.paired = settings.value("placement") == "right-corner-pair";
    result.upperLimit = counter.top() + settings.value("maxCounterY").toDouble() * counter.height();
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    const auto curves = baseContourCurves(outline.ink);
    if (curves.empty())
        return result;
    const int count = static_cast<int>(curves.size());
    const double minStepAngle = settings.value("minStepAngle").toDouble();
    const double minRoundedAngle = settings.value("minRoundedAngle").toDouble();
    const bool sampleValley = settings.value("sampleValley").toBool();
    for (auto at = geometry.timeSorted.begin(); at != geometry.timeSorted.end(); ++at) {
        const auto p = at->point;
        const double distance =
            QLineF(p, result.upper || result.top ? counter.topRight() : counter.bottomRight()).length();
        if (distance >= result.maxDistance)
            continue;
        const bool rounded = minRoundedAngle > 0 && !at->isSharp && at->isY && p.y() >= counter.bottom();
        if (!at->isSharp && !(result.top && at->isY) && !rounded)
            continue;
        CounterCornerBody body{*at, {}, distance, {}};
        if (result.paired) {
            if (p.x() < counter.center().x() || p.y() < counter.bottom() || p.x() >= geometry.prev(at)->point.x() ||
                p.x() >= geometry.next(at)->point.x())
                continue;
            for (auto lower = geometry.timeSorted.begin(); lower != geometry.timeSorted.end(); ++lower) {
                const auto q = lower->point;
                if (!lower->isSharp || q.x() < counter.left() || q.x() >= p.x() || q.y() <= p.y() ||
                    q.y() >= geometry.prev(lower)->point.y() || q.y() >= geometry.next(lower)->point.y() ||
                    QLineF(p, q).length() >= result.maxDistance || !interiorChord(outline.ink, *at, *lower))
                    continue;
                body.oppositeCorners.push_back(*lower);
            }
            result.corners.push_back(std::move(body));
            continue;
        }
        if (result.top) {
            if (p.x() <= counter.right() || p.y() <= geometry.prev(at)->point.y() ||
                p.y() <= geometry.next(at)->point.y())
                continue;
        } else if (result.upper) {
            if (p.x() < counter.center().x() || p.y() > result.upperLimit) continue;
        } else {
            if (p.x() <= counter.right() || p.y() < counter.center().y()) continue;
            if (rounded || p.y() >= geometry.prev(at)->point.y() || p.y() >= geometry.next(at)->point.y()) {
                const bool step = !rounded && minStepAngle > 0 &&
                    inwardCornerAngle(curves, *at, outline.orientation) >= minStepAngle;
                if (!step && !sampleValley && !rounded) continue;
                // Measure both flanks near the corner. Distant extrema can hide a valley or a short step.
                const bool forward = outline.orientation == Orientation::CounterClockwise;
                const double sample = .25 * counter.height();
                auto limit = *at;
                limit.time += forward ? -count : count;
                PathExtrema rim, join;
                QPointF tangent;
                if (!advanceOnContour(outline.ink, *at, limit, !forward, sample, rim, tangent)) continue;
                limit.time = at->time + (forward ? count : -count);
                if (!advanceOnContour(outline.ink, *at, limit, forward, sample, join, tangent)) continue;
                const auto drop = rim.point - p;
                const auto run = join.point - p;
                const bool valley = !rounded && sampleValley && drop.x() < 0 && drop.y() > 1e-8 &&
                    run.x() > 0 && run.y() > 1e-8;
                const bool flatStep = drop.y() > std::abs(drop.x()) && run.x() > std::abs(run.y());
                // A rounded shoulder must separate the steep rim from the flatter join.
                const bool roundedStep = rounded && flatStep && drop.x() < 0 && run.y() >= -1e-8 &&
                    inwardTurnAngle(-drop, run, Orientation::CounterClockwise) >= minRoundedAngle;
                if (!valley && !(step && flatStep) && !roundedStep) continue;
                body.tangent = run;
                body.sampled = true;
                result.corners.push_back(std::move(body));
                continue;
            }
        }
        if (std::abs(at->time - std::round(at->time)) <= 1e-6) {
            const int index = static_cast<int>(std::lround(at->time)) % count;
            const auto& previous = curves[(index + count - 1) % count];
            const auto& next = curves[index];
            auto a = previous.pt3() - previous.pt4(), b = next.pt2() - next.pt1();
            if (std::hypot(a.x(), a.y()) <= 1e-9) a = previous.pointAt(1 - 1e-4) - p;
            if (std::hypot(b.x(), b.y()) <= 1e-9) b = next.pointAt(1e-4) - p;
            body.tangent = a.x() > b.x() ? a : b;
        } else {
            const int index = static_cast<int>(std::floor(at->time)) % count;
            const double t = at->time - std::floor(at->time), u = 1 - t;
            const auto& curve = curves[index];
            body.tangent = u * u * (curve.pt2() - curve.pt1()) + 2 * u * t * (curve.pt3() - curve.pt2()) +
                           t * t * (curve.pt4() - curve.pt3());
            if (body.tangent.x() < 0)
                body.tangent = -body.tangent;
        }
        if (body.tangent.x() > 0 && std::hypot(body.tangent.x(), body.tangent.y()) > 1e-9)
            result.corners.push_back(std::move(body));
    }
    return result;
}

static bool cutCounterCorners(SearchContext &context, const CounterCornerEvidence &evidence, const QJsonObject &policy,
                              bool defaultNormal, Stretching &cut) {
    const bool normalCut = policy.value("normalCut").toBool(defaultNormal);
    const auto &ink = context.transformedPath;
    const auto &counter = evidence.counter;
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    auto best = std::pair{true, evidence.maxDistance};
    bool found = false;
    for (const auto &body : evidence.corners) {
        const auto score = std::pair{body.sampled, body.distance};
        if (score >= best)
            continue;
        const auto &corner = body.anchor;
        const auto p = corner.point;
        if (evidence.paired) {
            std::optional<Stretching> paired;
            double closest = evidence.maxDistance;
            for (const auto &lower : body.oppositeCorners) {
                const double gap = QLineF(p, lower.point).length();
                if (gap >= closest)
                    continue;
                Stretching proposal{};
                proposal.topBaseLine = corner;
                proposal.bottomBaseLine = lower;
                const double offset = policy.value("strokeOffsetX").toDouble();
                if (offset != 0) {
                    const auto center = QLineF(p, lower.point).center();
                    if (!medialChordAt(context, center.x() + offset * gap, center.y(), 2 * gap, proposal))
                        continue;
                }
                if (normalCut && !strokeNormalCut(context, proposal))
                    continue;
                paired = proposal;
                closest = gap;
            }
            if (paired) {
                cut = *paired;
                best = score;
                found = true;
            }
            continue;
        }
        const double length = std::hypot(body.tangent.x(), body.tangent.y());
        const QPointF normal = (evidence.top ? -1 : 1) *
                               (normalCut ? QPointF(body.tangent.y(), -body.tangent.x()) / length : QPointF(0, -1));
        const auto hits = FindInterections(ink, QLineF(p, p + normal * reach), true);
        std::optional<PathExtrema> top;
        double width = reach;
        for (const auto& hit : hits) {
            const double d = QLineF(p, hit.point).length();
            if (d < width && (evidence.top ? hit.point.y() > p.y() : hit.point.y() < p.y()) &&
                interiorChord(ink, hit, corner)) {
                top = hit;
                width = d;
            }
        }
        if (!top || (!evidence.top && (evidence.upper ? std::max(top->point.y(), p.y()) > evidence.upperLimit
                                                      : std::min(top->point.x(), p.x()) <= counter.right())))
            continue;
        cut.topBaseLine = evidence.top ? corner : *top;
        cut.bottomBaseLine = evidence.top ? *top : corner;
        best = score;
        found = true;
    }
    return found;
}

bool counterEntryCorner(SearchContext &context, const QRectF &counter, const QJsonObject &settings,
                               Stretching &cut) {
    const auto corner = settings.value("entryCorner").toObject();
    return cutCounterCorners(context, recognizeCounterCorners(inkOutline(context), counter, corner), corner,
                             settings.value("normalCut").toBool(), cut);
}

std::vector<CounterHole> recognizeCounterHoles(const InkOutline &outline) {
    std::vector<CounterHole> result;
    for (size_t i = 1; i < outline.contours.size(); ++i) {
        const auto box = outline.contours[i].boundingRect();
        if (!outline.ink.contains(box.center()) && box.width() > 0 && box.height() > 0)
            result.push_back({i, box});
    }
    return result;
}

bool cutFinalCounterBody(SearchContext &context, const QRectF &counter, const CounterCornerEvidence &entry,
                                const QJsonObject &bounds) {
    const auto& ink = context.transformedPath;
    const double offset = bounds.value("offset").toDouble() * counter.height();
    const double x = counter.right() + offset;
    const double y = counter.bottom() + bounds.value("baselineOffset").toDouble() * counter.height();
    Stretching cut{};
    if (!cutCounterCorners(context, entry, bounds.value("entryCorner").toObject(), bounds.value("normalCut").toBool(),
                           cut)) {
        if (bounds.value("entryCorner").toObject().value("required").toBool()) return false;
        if (!medialChordAt(context, x, y, counter.height() * 2, cut))
            return false;
        if (bounds.value("normalCut").toBool() && !strokeNormalCut(context, cut)) return false;
    }
    const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
    if (pieces.size() != 2) return false;
    const auto body = pieces[1].path.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
    if (!body.contains(counter) || !body.contains(std::prev(context.xSorted.end())->point) ||
        body.contains(context.xSorted.begin()->point))
        return false;
    context.res = cut;
    context.res.topLeft = context.res.topRight = cut.topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = cut.bottomBaseLine;
    return true;
}

std::optional<InitialCounterBody> recognizeInitialCounterBody(const InkOutline &outline,
                                                                     const QJsonObject &settings) {
    const auto holes = recognizeCounterHoles(outline);
    if (holes.empty())
        return {};
    const auto found = std::max_element(holes.begin(), holes.end(), [](const auto &a, const auto &b) {
        return a.box.center().x() < b.box.center().x();
    });
    const auto &counter = found->box;
    if (outline.bounds.right() - counter.right() > settings.value("maxOuterDistance").toDouble(1) * counter.height())
        return {};
    InitialCounterBody result{counter, {}, {}};
    WordResultInfo::SubWordInfo empty;
    const auto geometry = outline.geometry(empty);
    const double radius = settings.value("maxNeckDistance").toDouble(1.5) * counter.height();
    for (auto corner = geometry.timeSorted.begin(); corner != geometry.timeSorted.end(); ++corner) {
        const auto p = corner->point;
        if (QLineF(p, counter.bottomLeft()).length() >= radius)
            continue;
        if (p.x() <= counter.left() && p.y() >= counter.center().y() && p.y() < geometry.prev(corner)->point.y() &&
            p.y() < geometry.next(corner)->point.y())
            result.valleys.push_back(*corner);
        if (corner->isSharp && p.x() < counter.left())
            result.normalAnchors.push_back(*corner);
    }
    return result;
}

bool cutInitialCounterBody(SearchContext &context, const InitialCounterBody &body, const QJsonObject &settings) {
    const auto &ink = context.transformedPath;
    const auto &counter = body.counter;
    const double radius = settings.value("maxNeckDistance").toDouble(1.5) * counter.height();
    const auto retainsCounter = [&](const Stretching &cut) {
        const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
        if (pieces.size() != 2) return false;
        const auto bounds = pieces[0].path.boundingRect();
        return bounds.contains(counter) && bounds.left() >= counter.left() - radius;
    };
    std::optional<Stretching> best;
    double nearest = radius;
    for (const auto &corner : body.valleys) {
        const auto p = corner.point;
        const double distance = QLineF(p, counter.bottomLeft()).length();
        if (distance >= nearest)
            continue;
        const auto hits = FindInterections(ink, QLineF(p, QPointF(p.x(), context.bbox.top() - 1)), true);
        for (const auto& top : hits) {
            if (top.point.y() >= p.y() || !interiorChord(ink, top, corner)) continue;
            Stretching cut{};
            cut.topBaseLine = top;
            cut.bottomBaseLine = corner;
            best = cut;
            nearest = distance;
            break;
        }
    }
    if (!best && settings.value("normalValley").toBool()) {
        // Find the opposite flank from the valley, then use both edges to orient the cut.
        const bool forward = context.orientation == Orientation::ClockWise;
        for (const auto &valley : body.valleys) {
            const double distance = QLineF(valley.point, counter.bottomLeft()).length();
            if (distance >= nearest) continue;
            Stretching cut{};
            if (!contourNormalCut(context, valley, forward, false, cut, true) ||
                cut.bottomBaseLine.point.x() > counter.left()) continue;
            std::swap(cut.topBaseLine, cut.bottomBaseLine);
            auto normal = cut;
            if (anchoredStrokeNormalCut(context, normal, forward) && normal.topBaseLine.point.x() <= counter.left() &&
                retainsCounter(normal)) {
                cut = normal;
            } else if (!retainsCounter(cut)) {
                continue;
            }
            best = cut;
            nearest = distance;
        }
    }
    if (!best && settings.value("normalNeck").toBool()) {
        double closest = radius;
        for (const auto &corner : body.normalAnchors) {
            const double distance = QLineF(corner.point, counter.bottomLeft()).length();
            if (distance >= closest) continue;
            for (bool previous : {false, true}) {
                Stretching cut{};
                if (!contourNormalCut(context, corner, previous, false, cut) || !retainsCounter(cut)) continue;
                best = cut; closest = distance;
            }
        }
    }
    if (!best) return false;
    context.res = *best;
    context.res.topLeft = context.res.topRight = best->topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = best->bottomBaseLine;
    context.res.fixedRightBoundary = true;
    context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto& point) {
        return point.point.x() < best->bottomBaseLine.point.x();
    });
    return true;
}

std::optional<LoopHeadBody> recognizeLoopHead(const InkOutline &outline, const QJsonObject &settings) {
    const auto holes = recognizeCounterHoles(outline);
    if (holes.empty())
        return {};
    const auto selected = std::max_element(holes.begin(), holes.end(), [](const auto &a, const auto &b) {
        return a.box.center().x() < b.box.center().x();
    });
    LoopHeadBody result{selected->box};
    const auto *counter = &result.counter;
    const double scale = counter->height();
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    const auto route = settings.value("neckWalk").toObject();
    if (!route.isEmpty()) {
        for (auto crest = geometry.timeSorted.begin(); crest != geometry.timeSorted.end(); ++crest) {
            if (crest->point.y() >= counter->top() || crest->point.y() >= geometry.prev(crest)->point.y() ||
                crest->point.y() >= geometry.next(crest)->point.y())
                continue;
            const double distance = QLineF(crest->point, QPointF(counter->center().x(), counter->top())).length();
            if (distance >= 3 * scale)
                continue;
            result.necks.push_back({*crest, distance, recognizeSavedRoute(geometry, route, &*crest)});
        }
        return result;
    }
    double nearest = settings.value("maxEdgeDistance").toDouble(1.5) * scale;
    const QPointF counterLeft(counter->left(), counter->center().y());
    for (auto point = geometry.timeSorted.begin(); point != geometry.timeSorted.end(); ++point) {
        const auto p = point->point;
        const double distance = QLineF(p, counterLeft).length();
        if (p.x() >= counter->left() || p.x() >= geometry.prev(point)->point.x() ||
            p.x() >= geometry.next(point)->point.x() || distance >= nearest)
            continue;
        result.edge = *point;
        nearest = distance;
    }
    return result;
}

bool cutLoopHead(SearchContext &context, const LoopHeadBody &evidence, const QJsonObject &bodySettings,
                        const QJsonObject &policy) {
    const auto &ink = context.transformedPath;
    const auto settings = mergeShapeSettings(bodySettings, policy);
    const auto *counter = &evidence.counter;
    const double scale = counter->height();
    const auto neckWalk = bodySettings.value("neckWalk").toObject();
    if (!neckWalk.isEmpty()) {
        // A descending loop has a return bend rather than a horizontal neck
        // below its left edge. Anchor the same route engine on this loop's own
        // crest; a neighboring ascender must not become its starting point.
        std::optional<Stretching> best;
        double nearest = 3 * scale;
        for (const auto &candidate : evidence.necks) {
            const auto *crest = &candidate.crest;
            const double distance = candidate.distance;
            if (distance >= nearest) continue;
            context.res = {};
            if (!projectSavedRoute(context, candidate.route, bodySettings.value("neckWalk").toObject(),
                                   policy.value("neckWalk").toObject()))
                continue;
            const auto cut = context.res;
            const QLineF chord(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (std::min(chord.p1().y(), chord.p2().y()) <= counter->bottom() ||
                chord.length() > 3 * scale ||
                chord.center().y() - counter->bottom() > settings.value("maxNeckDistance").toDouble(3) * scale) continue;
            const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
            if (pieces.size() != 2 || toSubpaths(pieces[0].path).size() != 2) continue;
            const auto body = pieces[0].path.boundingRect();
            if (!body.contains(*counter) || body.left() < counter->left() - 2 * scale ||
                !body.adjusted(-1e-6,-1e-6,1e-6,1e-6).contains(crest->point) || body.height() > 6 * scale) continue;
            best = cut; nearest = distance;
        }
        if (!best) return false;
        context.res = *best;
        context.res.fixedRightBoundary = true;
        return true;
    }

    const auto &edge = evidence.edge;
    if (!edge) return false;

    std::optional<Stretching> boundary;
    const double maxInset = settings.value("maxProjectionInset").toDouble(2) * scale;
    const double rimWidth = counter->left() - edge->point.x();
    // A bent neck may not reach the head's exact left edge. Search only a
    // bounded strip inside that head, never following the connecting stroke
    // toward another letter. A cusp thinner than the head's own rim is not
    // a usable neck cross-section.
    for (int sample = 0; sample <= 16 && !boundary; ++sample) {
        const double probeX = edge->point.x() + maxInset * sample / 16;
        const auto hits = FindInterections(ink, QLineF(probeX, edge->point.y(),
                                                       probeX, context.bbox.bottom() + 1));
        double distance = settings.value("maxNeckDistance").toDouble(3) * scale;
        for (size_t i = 1; i < hits.size(); ++i) {
            const auto& top = hits[i - 1];
            const auto& bottom = hits[i];
            if (top.pathIndex != 0 || bottom.pathIndex != 0 ||
                top.point.y() <= edge->point.y() || top.point.y() < counter->bottom() ||
                !interiorChord(ink, top, bottom)) continue;
            const double gap = QLineF(top.point, bottom.point).center().y() - edge->point.y();
            if (gap >= distance) continue;
            Stretching cut{};
            cut.topBaseLine = top;
            cut.bottomBaseLine = bottom;
            if (settings.value("normalCut").toBool() && !strokeNormalCut(context, cut)) continue;
            if (QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length() < rimWidth ||
                std::min(cut.topBaseLine.point.y(), cut.bottomBaseLine.point.y()) < counter->bottom()) continue;
            // A chord across a small spur can also be normal to its ink. It
            // is not this head's neck: the resulting initial region must
            // retain the loop and stay inside the head's left envelope.
            const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
            if (pieces.size() != 2) continue;
            const auto bodyBounds = pieces.front().path.boundingRect();
            const QPointF rimSample((edge->point.x() + counter->left()) / 2, counter->center().y());
            if (!bodyBounds.contains(*counter) || bodyBounds.left() < edge->point.x() - rimWidth ||
                !pieces.front().path.contains(rimSample)) continue;
            boundary = cut;
            distance = gap;
        }
    }
    if (!boundary) return false;
    context.res = *boundary;
    context.res.topLeft = context.res.topRight = boundary->topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = boundary->bottomBaseLine;
    context.res.fixedRightBoundary = true;
    const double x = QLineF(boundary->topBaseLine.point, boundary->bottomBaseLine.point).center().x();
    context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto& point) {
        return point.point.x() < x;
    });
    return true;
}

static SearchContext reflectedCounterContext(const InkOutline &outline, WordResultInfo::SubWordInfo &empty) {
    const QTransform reflection(-1, 0, 0, 1, 0, 0);
    SearchContext result{.transformedPath = reflection.map(outline.ink), .subword = empty};
    result.bbox = reflection.mapRect(outline.bounds);
    result.orientation = outline.orientation == Orientation::ClockWise ? Orientation::CounterClockwise : Orientation::ClockWise;
    for (auto p : outline.time) {
        p.point = reflection.map(p.point);
        result.timeSorted.insert(p);
    }
    return result;
}

static LoopBodyEvidence recognizeLoopBody(const InkOutline &outline, const QJsonObject &settings,
                                          const CounterHole &anchor, const std::shared_ptr<const LoopTeeth> &teeth) {
    LoopBodyEvidence result{anchor};
    result.teeth = teeth;
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    const auto &ink = context.transformedPath;
    const auto occurrence = anchor.index;
    auto hole = context.subPaths[occurrence].boundingRect();
    if (settings.value("counterPair") == "horizontal") {
        std::optional<QRectF> partner;
        double nearest = hole.height();
        for (size_t i = 1; i < context.subPaths.size(); ++i) {
            if (i == occurrence) continue;
            const auto other = context.subPaths[i].boundingRect();
            const double gap = hole.left() - other.right();
            if (gap >= 0 && gap < nearest && !ink.contains(other.center()) &&
                std::min(hole.bottom(), other.bottom()) > std::max(hole.top(), other.top())) {
                partner = other; nearest = gap;
            }
        }
        if (!partner)
            return result;
        hole = hole.united(*partner);
    }
    double bandY = -1;
    if (settings.value("counterPair") == "vertical") {
        std::optional<QRectF> partner;
        double nearest = context.bbox.height();
        for (size_t i = 1; i < context.subPaths.size(); ++i) {
            if (i == occurrence) continue;
            const auto other = context.subPaths[i].boundingRect();
            const double gap = std::max(other.top() - hole.bottom(), hole.top() - other.bottom());
            if (gap >= 0 && gap < nearest && gap <= settings.value("maxCounterGap").toDouble(2) *
                std::max(hole.height(), other.height()) && !ink.contains(other.center()) &&
                std::min(hole.right(), other.right()) > std::max(hole.left(), other.left())) {
                partner = other; nearest = gap;
            }
        }
        if (!partner)
            return result;
        bandY = partner->top() > hole.top() ? (hole.bottom() + partner->top()) / 2 :
            (partner->bottom() + hole.top()) / 2;
        hole = hole.united(*partner);
    }
    if (hole.width() < settings.value("minAspect").toDouble() * hole.height())
        return result;
    for (size_t j = 1; j < context.subPaths.size(); ++j) {
        if (j == occurrence) continue;
        const auto other = context.subPaths[j].boundingRect();
        // Stacked counters belong to a different body shape, such as medial heh.
        if (!ink.contains(other.center()) && !hole.contains(other) && other.left() < hole.right() &&
            other.right() > hole.left())
            return result;
    }

    result.hole = hole;
    result.bandY = bandY;
    result.valid = true;
    const auto entryWalk = settings.value("entryWalk").toObject();
    if (!entryWalk.isEmpty())
        for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest) {
            const auto p = crest->point;
            const double gap = hole.top() - p.y();
            if (p.x() < hole.left() || p.x() > hole.right() || gap < 0 || gap >= hole.height() * 2 ||
                p.y() >= context.prev(crest)->point.y() || p.y() >= context.next(crest)->point.y())
                continue;
            const auto route = recognizeSavedRoute(context, entryWalk, &*crest);
            if (route)
                result.entryRoutes.push_back({*crest, gap, route});
        }
    auto entryCorner = settings.value("entryCorner").toObject();
    result.entry = recognizeCounterCorners(outline, hole, entryCorner);
    if (entryCorner.value("tryLowerCorner").toBool()) {
        entryCorner.remove("placement");
        result.lowerEntry = recognizeCounterCorners(outline, hole, entryCorner);
    }
    auto reflected = reflectedCounterContext(outline, empty);
    result.exit = recognizeCounterCorners(inkOutline(reflected), QTransform(-1, 0, 0, 1, 0, 0).mapRect(hole),
                                          settings.value("exitCorner").toObject());
    return result;
}

std::vector<LoopBodyEvidence> recognizeLoopBodies(const InkOutline &outline, const QJsonObject &body) {
    std::vector<LoopBodyEvidence> result;
    const auto tooth = body.value("exitTooth").toObject();
    const auto teeth = tooth.isEmpty() ? nullptr : recognizeLoopTeeth(outline, tooth);
    for (const auto &hole : recognizeCounterHoles(outline))
        result.push_back(recognizeLoopBody(outline, body, hole, teeth));
    return result;
}

static bool cutLoopExitCorner(SearchContext &context, const CounterCornerEvidence &evidence, const QJsonObject &policy,
                              Stretching &cut) {
    WordResultInfo::SubWordInfo empty;
    auto reflected = reflectedCounterContext(inkOutline(context), empty);
    if (!cutCounterCorners(reflected, evidence, policy.value("exitCorner").toObject(),
                           policy.value("normalCut").toBool(), cut))
        return false;
    const QTransform reflection(-1, 0, 0, 1, 0, 0);
    cut.topBaseLine.point = reflection.map(cut.topBaseLine.point);
    cut.bottomBaseLine.point = reflection.map(cut.bottomBaseLine.point);
    return true;
}

std::optional<std::pair<Stretching, Stretching>>
cutLoopBody(SearchContext &context, const LoopBodyEvidence &body, const QJsonObject &settings, bool needEntry) {
    if (!body.valid)
        return {};
    const auto &ink = context.transformedPath;
    const auto &hole = body.hole;
    const auto occurrence = body.anchor.index;
    const auto bandY = body.bandY;
    const double offset = settings.value("sideOffset").toDouble(0.5) * hole.height();
    const double y = bandY >= 0 ? bandY : hole.bottom() + settings.value("baselineOffset").toDouble(.3) * hole.height();
    Stretching right{}, left{};
    const auto entryWalk = settings.value("entryWalk").toObject();
    const auto exitTooth = settings.value("exitTooth").toObject();
    bool anchoredRight = false;
    if (needEntry && !entryWalk.isEmpty()) {
        const auto originalForm = context.form;
        const auto originalResult = context.res;
        const auto originalNext = context.nextIter;
        context.form = entryWalk;
        double nearest = hole.height() * 2;
        for (const auto &entry : body.entryRoutes) {
            if (entry.gap >= nearest)
                continue;
            context.res = {};
            if (!projectSavedRoute(context, entry.route, entryWalk, {}))
                continue;
            right = context.res;
            nearest = entry.gap;
            anchoredRight = true;
        }
        context.form = originalForm;
        context.res = originalResult;
        context.nextIter = originalNext;
        if (!anchoredRight) return {};
    } else if (needEntry) {
        const auto entryCorner = settings.value("entryCorner").toObject();
        const bool normalCut = settings.value("normalCut").toBool();
        anchoredRight = cutCounterCorners(context, body.entry, entryCorner, normalCut, right) ||
            (body.lowerEntry && cutCounterCorners(context, *body.lowerEntry, entryCorner, normalCut, right));
        if (!anchoredRight && entryCorner.value("required").toBool()) return {};
    }
    const double entryMinY = settings.value("entryBelowCounter").toBool() ? hole.bottom() :
        -std::numeric_limits<double>::infinity();
    if (needEntry && !anchoredRight &&
        !medialChordAt(context, hole.right() + offset, y, hole.height() * 2, right, false, entryMinY)) return {};
    const bool anchoredLeft = exitTooth.isEmpty() && cutLoopExitCorner(context, body.exit, settings, left);
    if (!anchoredLeft && settings.value("exitCorner").toObject().value("required").toBool()) return {};
    if (!exitTooth.isEmpty()) {
        bool found = false;
        double nearest = exitTooth.value("maxGap").toDouble(1.5) * hole.height();
        for (const auto &tooth : cutLoopTeeth(context, body.teeth, exitTooth)) {
            const double gap = hole.left() - std::max(tooth.tip.point.x(), tooth.bend.point.x());
            if (gap < 0 || gap >= nearest || tooth.tip.point.y() >= hole.bottom() ||
                (needEntry && !Stretching::cmpbyx{}(right, tooth.left))) continue;
            left = loopToothExit(context, tooth);
            nearest = gap;
            found = true;
        }
        if (!found) return {};
    } else if (!anchoredLeft && !medialChordAt(context, hole.left() - offset, y, hole.height() * 2, left)) return {};
    if (settings.value("normalCut").toBool() &&
        ((needEntry && !anchoredRight && !strokeNormalCut(context, right)) ||
         (!anchoredLeft && !strokeNormalCut(context, left)))) return {};
    if (needEntry && std::min(right.topBaseLine.point.y(), right.bottomBaseLine.point.y()) < entryMinY) return {};
    if (settings.contains("maxBodyHeight")) {
        const auto cuts = needEntry ? std::vector<Stretching>{right, left} : std::vector<Stretching>{left};
        const auto pieces = cutRegions(ink, {}, cuts, context.orientation, true);
        if (pieces.size() != cuts.size() + 1 || pieces[needEntry ? 1 : 0].path.boundingRect().height() >
            settings.value("maxBodyHeight").toDouble() * hole.height()) return {};
    }
    // Rotation must leave the entire enclosed counter in the body.
    if (!needEntry) {
        const auto pieces = cutRegions(ink, {}, {left}, context.orientation, true);
        if (pieces.size() != 2 || !pieces[0].path.boundingRect().contains(hole)) return {};
    } else if (settings.contains("exitCorner")) {
        // A suspended loop's incoming cut can overlap its counter in x while
        // staying above it. Check ownership of the actual closed contour,
        // rather than rejecting an otherwise valid diagonal neck by its box.
        const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3) return {};
        const auto contours = toSubpaths(pieces[1].path);
        if (std::none_of(contours.begin(), contours.end(), [&](const auto& p) {
                return p == context.subPaths[occurrence];
            })) return {};
    } else if (entryWalk.isEmpty() && exitTooth.isEmpty() && !settings.contains("counterPair")) {
        if (std::min(right.topBaseLine.point.x(), right.bottomBaseLine.point.x()) <= hole.right() ||
            std::max(left.topBaseLine.point.x(), left.bottomBaseLine.point.x()) >= hole.left()) return {};
    } else {
        const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3 || !pieces[1].path.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(hole)) return {};
    }
    return std::pair{right, left};
}

// Slot selection is assembly: unavailable or out-of-order counters cannot
// erase their independently recognized features. Never skip the first loop
// just because its projection fails, which would borrow a later letter.
const LoopBodyEvidence *selectLoopBody(const SearchContext &context, const std::vector<LoopBodyEvidence> &bodies,
                                              double minX, double maxX, const QPainterPath *availableInk) {
    const auto availableContours = availableInk ? toSubpaths(*availableInk) : QVector<QPainterPath>{};
    const LoopBodyEvidence *selected = nullptr;
    double rightmost = minX;
    for (const auto &body : bodies) {
        if (availableInk && std::none_of(availableContours.begin(), availableContours.end(), [&](const auto &contour) {
                return contour == context.subPaths[body.anchor.index];
            }))
            continue;
        const double x = body.anchor.box.center().x();
        if (x < maxX && x > rightmost) {
            selected = &body;
            rightmost = x;
        }
    }
    return selected;
}

std::optional<std::pair<Stretching, Stretching>>
medialLoopBoundaries(SearchContext &context, const QJsonObject &settings, double minX, double maxX,
                     bool needEntry, const QPainterPath *availableInk) {
    const auto bodies = recognizeLoopBodies(inkOutline(context), settings);
    const auto body = selectLoopBody(context, bodies, minX, maxX, availableInk);
    return body ? cutLoopBody(context, *body, settings, needEntry) : std::nullopt;
}

std::vector<StackedCounterPair> recognizeStackedPairs(const InkOutline &outline, const QJsonObject &settings) {
    const auto &ink = outline.ink;
    const auto curves = baseContourCurves(ink);
    std::vector<StackedCounterPair> results;
    if (curves.empty()) return results;
    const double footInwardTolerance = std::sin(settings.value("maxFootInwardAngle").toDouble(10) * M_PI / 180);
    std::vector<QRectF> holes;
    for (size_t i = 1; i < outline.contours.size(); ++i) {
        const auto hole = outline.contours[i].boundingRect();
        if (hole.width() > 0 && hole.height() > 0 && !ink.contains(hole.center())) holes.push_back(hole);
    }
    for (const auto& upper : holes) for (const auto& lower : holes) {
        const double scale = lower.height();
        if (upper.bottom() >= lower.top() ||
            std::min(upper.right(), lower.right()) <= std::max(upper.left(), lower.left()) ||
            lower.top() - upper.bottom() > settings.value("maxCounterGap").toDouble(2) *
                std::max(upper.height(), lower.height())) continue;
        const auto corners = [&](bool rightSide) {
            std::vector<CounterFoot> found;
            const QPointF reference = rightSide ? lower.topRight() : lower.topLeft();
            const double maxDistance = settings.value("maxCornerDistance").toDouble(2) * scale;
            const double bandTop = (upper.bottom() + lower.top()) / 2;
            for (const auto &corner : outline.time) {
                const auto p = corner.point;
                const double distance = QLineF(p, reference).length();
                if (!corner.isSharp || distance >= maxDistance || p.y() < bandTop ||
                    p.y() >= lower.center().y() || (rightSide ? p.x() <= lower.center().x() : p.x() >= lower.center().x()) ||
                    std::abs(corner.time - std::round(corner.time)) > 1e-6)
                    continue;
                const int count = static_cast<int>(curves.size());
                const int index = static_cast<int>(std::lround(corner.time)) % count;
                const auto& previous = curves[(index + count - 1) % count];
                const auto& next = curves[index];
                auto a = previous.pt3() - previous.pt4();
                auto b = next.pt2() - next.pt1();
                if (std::hypot(a.x(), a.y()) < 1e-9)
                    a = previous.pointAt(1 - 1e-4) - p;
                if (std::hypot(b.x(), b.y()) < 1e-9)
                    b = next.pointAt(1e-4) - p;
                const double al = std::hypot(a.x(), a.y()), bl = std::hypot(b.x(), b.y());
                if (al < 1e-9 || bl < 1e-9)
                    continue;
                a /= al;
                b /= bl;
                // The descending side of a stepped foot can lean slightly
                // inward. Allow that small angular variation around vertical;
                // the other branch still has to lead out towards the join.
                // Prefer the outer corner so the complete foot stays in the body.
                // Without a step, the nearest inward corner is the attachment.
                const bool hasFoot = rightSide ? std::min(a.x(), b.x()) >= -footInwardTolerance - 1e-6 :
                                                std::max(a.x(), b.x()) <= footInwardTolerance + 1e-6;
                const auto bisector = a + b;
                const double length = std::hypot(bisector.x(), bisector.y());
                if (length < 1e-3)
                    continue;
                const auto probe = bisector * (0.01 * scale / length);
                // At an inward corner the smaller wedge is outside the ink.
                if (ink.contains(p + probe) || !ink.contains(p - probe))
                    continue;
                const auto tangent = (rightSide ? a.x() > b.x() : a.x() < b.x()) ? a : b;
                if (rightSide ? tangent.x() <= 0 : tangent.x() >= 0)
                    continue;
                found.push_back({corner, tangent, distance, hasFoot});
            }
            return found;
        };
        results.push_back({upper, lower, corners(true), corners(false)});
        }
    return results;
}

std::vector<std::pair<Stretching, Stretching>>
cutStackedPairs(SearchContext &context, const std::vector<StackedCounterPair> &bodies, const QJsonObject &policy) {
    const auto &ink = context.transformedPath;
    std::vector<std::pair<Stretching, Stretching>> results;
    for (const auto &body : bodies) {
        const auto &upper = body.upper;
        const auto &lower = body.lower;
        const double scale = lower.height();
        const auto cornerCut = [&](bool rightSide) -> std::optional<Stretching> {
            double closest = std::numeric_limits<double>::infinity();
            std::optional<Stretching> best;
            bool bestHasFoot = false;
            for (const auto &foot : rightSide ? body.right : body.left) {
                const auto &corner = foot.anchor;
                const auto p = corner.point, tangent = foot.tangent;
                const auto distance = foot.distance;
                const bool hasFoot = foot.hasFoot;
                if (best) {
                    if (bestHasFoot && !hasFoot)
                        continue;
                    if (bestHasFoot == hasFoot) {
                        // Both the outside step and its inner attachment can
                        // pass the angle tolerance. Keep the outside step,
                        // rather than the corner nearest the counter.
                        const double outward = (rightSide ? 1 : -1) *
                            (p.x() - best->bottomBaseLine.point.x());
                        if (hasFoot ? outward <= 0 : distance >= closest)
                            continue;
                    }
                }
                QPointF normal =
                    policy.value("normalCut").toBool() ? QPointF(tangent.y(), -tangent.x()) : QPointF(0, -1);
                if (normal.y() > 0)
                    normal = -normal;
                const double reach = upper.united(lower).height() + 2 * scale;
                const auto hits = FindInterections(ink, QLineF(p, p + normal * reach), true);
                for (const auto& top : hits) {
                    if (top.pathIndex != 0 || top.point.y() >= p.y() || !interiorChord(ink, top, corner))
                        continue;
                    Stretching cut{};
                    cut.topBaseLine = top;
                    cut.bottomBaseLine = corner;
                    cut.referenceWidth = QLineF(top.point, p).length();
                    best = cut;
                    closest = distance;
                    bestHasFoot = hasFoot;
                    break;
                }
            }
            return best;
        };
        const auto right = cornerCut(true), left = cornerCut(false);
        if (!right || !left || !Stretching::cmpbyx{}(*right, *left)) continue;
        const auto pieces = cutRegions(ink, {}, {*right, *left}, context.orientation, true);
        if (pieces.size() != 3 || !pieces[1].path.boundingRect().contains(upper) ||
            !pieces[1].path.boundingRect().contains(lower)) continue;
        results.emplace_back(*right, *left);
    }
    return results;
}

// A tapered terminal tail turns upward into two close corners at the head.
// Following both sides from its free end distinguishes these corners from
// the two much wider lips of the letter above it.
std::optional<ReturnSeamBody> recognizeRisingReturn(const InkOutline &outline, const QJsonObject &spec) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    const auto tip = ctx.timeSorted.find(*ctx.xSorted.rbegin());
    if (tip == ctx.timeSorted.end())
        return {};
    const auto corner = [&](bool forward) -> std::optional<PathExtrema> {
        auto it = tip;
        double bottom = tip->point.y();
        for (size_t i = 1; i < ctx.timeSorted.size(); ++i) {
            it = forward ? ctx.next(it) : ctx.prev(it);
            bottom = std::max(bottom, it->point.y());
            if (it->isSharp && it->point.x() - tip->point.x() >= spec.value("minRun").toDouble() &&
                bottom - it->point.y() >= spec.value("minRise").toDouble()) return *it;
        }
        return {};
    };
    const auto a = corner(true), b = corner(false);
    if (!a || !b)
        return {};
    return ReturnSeamBody{*tip, *a, *b};
}

// Width and centre of a single uninterrupted stroke at the given height.
std::optional<QLineF> singleHorizontalStroke(const QPainterPath& body, const QRectF& bounds, double y) {
    auto hits = FindInterections(body, QLineF(bounds.left() - 1, y, bounds.right() + 1, y), true);
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.point.x() < b.point.x(); });
    std::optional<QLineF> result;
    for (size_t i = 1; i < hits.size(); ++i) {
        const QLineF span(hits[i - 1].point, hits[i].point);
        if (span.length() < .005 || !body.contains(span.center()))
            continue;
        if (result)
            return {};
        result = span;
    }
    return result;
}

std::vector<RimBodyEvidence> recognizeRimBodies(const InkOutline &outline) {
    std::vector<RimBodyEvidence> result;
    for (size_t i = 1; i < outline.contours.size(); ++i) {
        const auto box = outline.contours[i].boundingRect();
        if (box.height() < .12 || box.width() < .12 || !outline.contours[0].contains(outline.contours[i]))
            continue;
        RimBodyEvidence body{{i, box}, {}};
        for (const auto &p : outline.time)
            if (p.isSharp || p.isX || p.isY)
                body.anchors.push_back(p);
        result.push_back(std::move(body));
    }
    return result;
}

std::vector<std::pair<Stretching, Stretching>>
cutRimBodies(SearchContext &ctx, const std::vector<RimBodyEvidence> &bodies, const QJsonObject &settings) {
    struct Side {
        double score;
        Stretching cut;
    };
    struct Candidate {
        double score;
        Stretching entry, exit;
    };
    std::vector<Candidate> candidates;
    const auto& ink = ctx.transformedPath;
    for (const auto &evidence : bodies) {
        const auto &hole = ctx.subPaths[evidence.counter.index];
        const auto box = evidence.counter.box;
        const double h = box.height();
        const auto right = settings.value("entry").toArray();
        const auto left = settings.value("exit").toArray();
        const double radius = settings.value("radius").toDouble(1.4) * h;
        const auto bodyFit = [&](const QPainterPath& body) {
            const auto contours = toSubpaths(body);
            if (contours.size() != 2 || contours[1] != hole)
                return false;
            const auto bounds = body.boundingRect();
            const double margin = settings.value("rimMargin").toDouble(1.6) * h;
            if (bounds.right() > box.right() + margin)
                return false;
            if (settings.contains("shaftRise")) {
                // The counter must have its own narrow ascender, continuous from the rim.
                const auto rise = box.top() - bounds.top();
                if (rise < settings.value("shaftRise").toDouble() * h || rise > 8 * h)
                    return false;
                double prev = 0;
                for (double fraction : {.3, .5, .7}) {
                    const auto y = bounds.top() + rise * fraction;
                    auto shaft = singleHorizontalStroke(body, bounds, y);
                    if (!shaft || shaft->length() > 1.4 * h)
                        return false;
                    const double x = shaft->center().x();
                    if (x < box.left() - 1.6 * h || x > box.right() + .5 * h ||
                        (fraction > .3 && std::abs(x - prev) > .65 * h))
                        return false;
                    prev = x;
                }
            } else if (bounds.top() < box.top() - margin)
                return false;
            if (bounds.bottom() > box.bottom() + margin || bounds.left() < box.left() - margin)
                return false;
            return true;
        };
        const auto side = [&](const QJsonArray& anchor, bool entry) {
            std::vector<Side> out;
            if (anchor.size() != 2)
                return out;
            const QPointF aim(box.left() + anchor[0].toDouble() * box.width(), box.top() + anchor[1].toDouble() * h);
            for (const auto &corner : evidence.anchors) {
                if (QLineF(corner.point, aim).length() > radius)
                    continue;
                for (auto cut : cornerSections(ctx, corner)) {
                    const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
                    if (line.length() < .15 || line.length() > 2.4 * h || QLineF(line.center(), aim).length() > radius)
                        continue;
                    for (int flip = 0; flip < 2; ++flip) {
                        if (flip)
                            std::swap(cut.topBaseLine, cut.bottomBaseLine);
                        cut.topRight = cut.topLeft = cut.topBaseLine;
                        cut.bottomRight = cut.bottomLeft = cut.bottomBaseLine;
                        const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
                        if (parts.size() != 2)
                            continue;
                        const auto& body = parts[entry ? 1 : 0].path;
                        const auto outlines = toSubpaths(body);
                        if (outlines.size() < 2 ||
                            std::find(outlines.begin() + 1, outlines.end(), hole) == outlines.end())
                            continue;
                        const double score = (QLineF(aim, line.center()).length() + .35 * line.length()) / h;
                        out.push_back({score, cut});
                        break;
                    }
                }
            }
            std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score < b.score; });
            if (out.size() > 12)
                out.resize(12);
            return out;
        };
        const auto entries = side(right, true);
        const auto exits = side(left, false);
        for (const auto& a : entries)
            for (const auto& b : exits) {
                const auto time = getJoinTime(a.cut, ctx.orientation);
                if (!time.isStrictLeft(b.cut.topBaseLine.time) || !time.isStrictLeft(b.cut.bottomBaseLine.time))
                    continue;
                const auto bodies = cutRegions(ink, {}, {a.cut, b.cut}, ctx.orientation, true);
                if (bodies.size() != 3 || !bodyFit(bodies[1].path))
                    continue;
                candidates.push_back({a.score + b.score, a.cut, b.cut});
            }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.score < b.score; });
    std::vector<std::pair<Stretching, Stretching>> result;
    for (const auto& a : candidates) {
        const bool duplicate = std::any_of(result.begin(), result.end(), [&](const auto& old) {
            const auto close = [](const Stretching& x, const Stretching& y) {
                return std::abs(x.topBaseLine.time - y.topBaseLine.time) < .01 &&
                       std::abs(x.bottomBaseLine.time - y.bottomBaseLine.time) < .01;
            };
            return close(a.entry, old.first) && close(a.exit, old.second);
        });
        if (!duplicate)
            result.emplace_back(a.entry, a.exit);
        if (result.size() == 8)
            break;
    }
    return result;
}

std::vector<std::pair<Stretching, Stretching>> rimSections(SearchContext &ctx, const QJsonObject &settings) {
    return cutRimBodies(ctx, recognizeRimBodies(inkOutline(ctx)), settings);
}

// Match both sides of an unbranched ascender where it enters a stacked stroke.
bool descendingStemExit(SearchContext& ctx, const QJsonObject& spec, Stretching& best) {
    if (ctx.timeSorted.empty())
        return false;
    const double pen = spec.value("penWidth").toDouble(.5);
    const double rise = spec.value("minRise").toDouble(4);
    const auto lip = spec.value("headLip").toObject();
    const bool stacked = !lip.isEmpty();
    double lowest = -std::numeric_limits<double>::infinity();
    for (const auto& p : ctx.timeSorted) {
        if ((!p.isSharp && !stacked) || p.point.y() < ctx.bbox.top() + rise)
            continue;
        std::vector<Stretching> lines = cornerSections(ctx, p);
        for (const auto& q : ctx.timeSorted) {
            if ((!q.isSharp && !stacked) || q.time <= p.time || QLineF(p.point, q.point).length() > 2.4 * pen ||
                !interiorChord(ctx.transformedPath, p, q))
                continue;
            Stretching cut{};
            cut.topBaseLine = p;
            cut.bottomBaseLine = q;
            lines.push_back(cut);
        }
        for (auto cut : lines) {
            QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (line.length() < .32 * pen || line.length() > 2.25 * pen)
                continue;
            if (stacked) {
                const auto overhang = ctx.xSorted.begin()->point - line.center();
                if (overhang.x() < lip.value("minRun").toDouble() || overhang.x() > lip.value("maxRun").toDouble() ||
                    overhang.y() < -.5 * pen || overhang.y() > lip.value("maxDrop").toDouble())
                    continue;
            }
            for (int side = 0; side < 2; ++side) {
                if (side)
                    std::swap(cut.topBaseLine, cut.bottomBaseLine);
                cut.topRight = cut.topLeft = cut.topBaseLine;
                cut.bottomLeft = cut.bottomRight = cut.bottomBaseLine;
                auto parts = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
                if (parts.size() != 2)
                    continue;
                const auto& shaft = parts.front().path;
                const auto b = shaft.boundingRect();
                if (toSubpaths(shaft).size() != 1 || b.height() < rise || b.width() > .48 * b.height() ||
                    (!stacked && b.top() > ctx.bbox.top() + 1.4 * pen) || b.bottom() - line.center().y() > pen ||
                    b.bottom() - line.center().y() < -.05)
                    continue;
                bool simple = true;
                std::optional<double> previousX;
                for (double f : {.18, .34, .5, .66, .82, .94}) {
                    // The diamond nib can have two intersections on either flank.
                    // Measure the shaft below the nib in the stacked allograph.
                    if (stacked && f < .34) continue;
                    const auto y = b.top() + f * b.height();
                    auto sample = singleHorizontalStroke(shaft, b, y);
                    if (!sample || sample->length() > (stacked ? 3.2 : 2.2) * pen || sample->length() < .17 * pen ||
                        (previousX && std::abs(sample->center().x() - *previousX) > .22 * b.height())) {
                        simple = false;
                        break;
                    }
                    previousX = sample->center().x();
                }
                if (!simple || !previousX || std::abs(*previousX - line.center().x()) > 1.45 * pen)
                    continue;
                const double score = line.center().y() - .15 * line.length();
                if (score > lowest) {
                    lowest = score;
                    best = cut;
                }
            }
        }
    }
    return std::isfinite(lowest);
}

std::vector<PathExtrema> recognizeLoopShoulders(const InkOutline &outline, const QRectF &hole,
                                                       const QJsonObject &body) {
    std::vector<PathExtrema> result;
    if (hole.height() <= 0)
        return result;
    const auto ideal = QPointF(hole.right(), hole.center().y());
    for (const auto &p : outline.time)
        if (p.isSharp && p.point.x() >= hole.right() - .25 * hole.height() &&
            QLineF(ideal, p.point).length() <= body.value("radius").toDouble(2) * hole.height())
            result.push_back(p);
    return result;
}

bool cutLoopShoulder(SearchContext &ctx, const QRectF &hole, const std::vector<PathExtrema> &anchors,
                            const QJsonObject &settings, double witness, bool final, Stretching &best) {
    const double h = hole.height();
    if (h <= 0)
        return false;
    const auto ideal = QPointF(hole.right(), hole.center().y());
    double score = std::numeric_limits<double>::infinity();
    for (const auto &anchor : anchors) {
        const auto *it = &anchor;
        const auto p = anchor.point;
        for (auto cut : cornerSections(ctx, *it)) {
            const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (line.length() > settings.value("maxWidth").toDouble(2.4) * h ||
                std::min(line.x1(), line.x2()) < hole.right() - .45 * h)
                continue;
            orientSection(ctx, cut, witness, true);
            const auto parts = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
            if (parts.size() != 2)
                continue;
            const auto paths = toSubpaths(parts[1].path);
            bool owns = false;
            for (int i = 1; i < paths.size(); ++i)
                if (paths[i].boundingRect() == hole)
                    owns = true;
            if (!owns)
                continue;
            if (final && (paths.size() != 2 || parts[1].path.boundingRect().top() <
                                                   hole.top() - settings.value("maxCapRise").toDouble(2) * h))
                continue;
            const auto fit = QLineF(ideal, p).length() + .35 * line.length();
            if (fit < score) {
                best = cut;
                score = fit;
            }
        }
    }
    return std::isfinite(score);
}

std::optional<LoopBridgeBody> recognizeLoopBridge(const InkOutline &outline) {
    if (outline.contours.size() < 2)
        return {};
    auto hole = outline.contours[1].boundingRect();
    for (size_t i = 2; i < outline.contours.size(); ++i)
        if (outline.contours[i].boundingRect().center().x() < hole.center().x())
            hole = outline.contours[i].boundingRect();
    const auto tip = *std::prev(outline.x.end());
    const double h = hole.height();
    if (h <= 0 || hole.left() - tip.point.x() < h)
        return {};
    LoopBridgeBody result{hole, tip, {}};
    const QPointF target(hole.right(), hole.bottom());
    for (const auto &p : outline.time) {
        if (!p.isSharp || p.point.x() < hole.right() - .15 * h || QLineF(p.point, target).length() > 2 * h)
            continue;
        for (const auto &q : outline.time) {
            const auto delta = q.point - p.point;
            if (q.isSharp && delta.y() >= .3 * h && std::abs(delta.x()) <= delta.y() &&
                q.point.x() >= hole.right() - .15 * h && interiorChord(outline.ink, p, q))
                result.pairs.emplace_back(p, q);
        }
    }
    return result;
}

bool cutLoopBridge(SearchContext &ctx, const LoopBridgeBody &body, const QJsonObject &settings,
                          Stretching &best) {
    const auto *tip = &body.tip;
    const auto &hole = body.hole;
    const auto &ink = ctx.transformedPath;
    const double h = hole.height();
    if (h <= 0 || hole.left() - tip->point.x() < h)
        return false;
    const QPointF target(hole.right(), hole.bottom());
    double bestFit = std::numeric_limits<double>::infinity();
    for (const auto &pair : body.pairs) {
        const auto *p = &pair.first;
        const auto *q = &pair.second;
        const QLineF edge(p->point, q->point);
        if (edge.length() > settings.value("maxWidth").toDouble(3) * h)
            continue;
        Stretching cut{};
        cut.topBaseLine = *p;
        cut.bottomBaseLine = *q;
        orientSection(ctx, cut, tip->time, true);
        const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
        if (parts.size() != 2)
            continue;
        const auto outlines = toSubpaths(parts[1].path);
        const auto box = parts[1].path.boundingRect();
        if (outlines.size() != 2 || outlines[1].boundingRect() != hole || box.right() > edge.center().x() + .7 * h ||
            box.bottom() - hole.bottom() < 1.2 * h || box.top() < hole.top() - 2.5 * h)
            continue;
        const double fit = QLineF(edge.center(), target).length() + .2 * edge.length();
        if (fit < bestFit) {
            bestFit = fit;
            best = cut;
        }
    }
    return std::isfinite(bestFit);
}

std::vector<CounterOutlineBody> recognizeCounterOutlines(const InkOutline &outline, const QJsonObject &body) {
    std::vector<CounterOutlineBody> result;
    const auto holes = recognizeCounterHoles(outline);
    const int count = body.value("counters").toInt();
    const auto aspect = body.value("outline").toObject().value("aspect").toArray();
    if (aspect.size() != 2)
        return result;
    const auto add = [&](std::vector<CounterHole> group) {
        QRectF eye;
        for (const auto &hole : group) {
            if (!outline.contours[0].contains(outline.contours[hole.index]))
                return;
            eye = eye.united(hole.box);
        }
        if (eye.height() <= .1 || eye.width() <= .1)
            return;
        const double ratio = eye.width() / eye.height();
        if (ratio >= aspect[0].toDouble() && ratio <= aspect[1].toDouble())
            result.push_back({std::move(group), eye});
    };
    for (size_t i = 0; i < holes.size(); ++i) {
        if (count == 1)
            add({holes[i]});
        else if (count == 2)
            for (size_t j = i + 1; j < holes.size(); ++j)
                add({holes[i], holes[j]});
    }
    return result;
}

std::vector<ParallelReturnBody> recognizeParallelReturns(const InkOutline &outline,
                                                                const QJsonObject &settings) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    using Band = QLineF;
    const auto &ink = ctx.transformedPath;
    auto bandsAt = [&](double x) {
        const auto hits = FindInterections(ink, QLineF(x, ctx.bbox.top() - 1, x, ctx.bbox.bottom() + 1), true);
        std::vector<Band> bands;
        for (size_t i = 1; i < hits.size(); ++i) {
            const Band b(hits[i - 1].point, hits[i].point);
            if (!hits[i - 1].pathIndex && !hits[i].pathIndex && b.length() >= .18 && b.length() <= 2.8 &&
                ink.contains(b.center()))
                bands.push_back(b);
        }
        return bands;
    };
    std::vector<ParallelReturnBody> bodies;
    for (auto elbow = ctx.timeSorted.begin(); elbow != ctx.timeSorted.end(); ++elbow) {
        if (ctx.prev(elbow)->point.x() < elbow->point.x() - .02 || ctx.next(elbow)->point.x() < elbow->point.x() - .02)
            continue;
        for (auto across : {&SearchContext::prev, &SearchContext::next}) {
            const auto up = across == &SearchContext::next ? &SearchContext::prev : &SearchContext::next;
            auto edge = elbow, tip = elbow;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                const auto next = (ctx.*across)(edge);
                if (next->point.x() < edge->point.x() - .04 || std::abs(next->point.y() - elbow->point.y()) > 2.8)
                    break;
                edge = next;
            }
            const auto run = edge->point.x() - elbow->point.x();
            if (run < settings.value("minRun").toDouble(4))
                continue;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                const auto next = (ctx.*up)(tip);
                if (next->point.x() < tip->point.x() - .16 || next->point.y() > tip->point.y() + .12)
                    break;
                tip = next;
            }
            if (tip->point.x() - elbow->point.x() < 2 || elbow->point.y() - tip->point.y() < 1.8)
                continue;
            auto parallelBands = [&](double x) -> std::optional<std::pair<Band, Band>> {
                auto bands = bandsAt(x);
                for (size_t i = 1; i < bands.size(); ++i) {
                    const Band a = bands[i - 1], b = bands[i];
                    if (std::abs(a.center().y() - elbow->point.y()) > 2.6 || b.center().y() <= a.center().y() ||
                        b.center().y() - a.center().y() > 3.4 * std::max(a.length(), b.length()) ||
                        std::max(a.length(), b.length()) > 2.3 * std::min(a.length(), b.length()))
                        continue;
                    return std::pair(a, b);
                }
                return {};
            };
            const auto a = parallelBands(elbow->point.x() + .32 * run), b = parallelBands(elbow->point.x() + .72 * run);
            if (!a || !b || std::abs(a->second.center().y() - b->second.center().y()) > 2.6)
                continue;
            const double stroke = (a->second.length() + b->second.length()) / 2;
            const double slope = (b->second.center().y() - a->second.center().y()) / (.4 * run);
            auto baseline = [&](double x) { return a->second.center().y() + slope * (x - a->second.center().x()); };
            ParallelReturnBody body{*elbow, *edge, *tip, *a, *b, stroke, slope, {}};
            for (const auto &corner : ctx.timeSorted)
                if (corner.isSharp && std::abs(corner.point.x() - edge->point.x()) <= 1.7 * stroke &&
                    std::abs(corner.point.y() - baseline(edge->point.x())) <= 2.1 * stroke)
                    body.corners.push_back(corner);
            bodies.push_back(std::move(body));
        }
    }
    return bodies;
}

std::vector<std::pair<Stretching, Stretching>>
cutParallelReturns(SearchContext &ctx, const std::vector<ParallelReturnBody> &bodies, const QJsonObject &settings) {
    const auto &ink = ctx.transformedPath;
    std::vector<std::pair<Stretching, Stretching>> pairs;
    for (const auto &body : bodies) {
        const auto *elbow = &body.elbow, *edge = &body.edge, *tip = &body.tip;
        const auto *a = &body.first, *b = &body.second;
        const double stroke = body.stroke, slope = body.slope;
        const auto baseline = [&](double x) { return a->second.center().y() + slope * (x - a->second.center().x()); };
        const double leftX = elbow->point.x() - settings.value("margin").toDouble(.5) * stroke;
        Stretching left{};
        if (!medialChordAt(ctx, leftX, baseline(leftX), 2.8 * stroke, left))
            continue;
        orientSection(ctx, left, tip->time, false);
        std::vector<Stretching> rights;
        // The tight fold can lean over its incoming stroke. Its lower
        // inward corner gives a more precise reference than the cap's projection.
        for (const auto &cornerValue : body.corners) {
            const auto *corner = &cornerValue;
            const double x = corner->point.x() + settings.value("margin").toDouble(.5) * stroke;
            Stretching cut{};
            if (medialChordAt(ctx, x, corner->point.y() + .5 * stroke, 1.8 * stroke, cut))
                rights.push_back(cut);
        }
        const double x = edge->point.x() + .5 * stroke;
        Stretching projected{};
        if (medialChordAt(ctx, x, baseline(x) + .5 * stroke, 2.8 * stroke, projected))
            rights.push_back(projected);
        for (auto right : rights) {
            orientSection(ctx, right, tip->time, true);
            const auto side = getJoinTime(right, ctx.orientation);
            if (!side.isStrictLeft(left.topBaseLine.time) || !side.isStrictLeft(left.bottomBaseLine.time))
                continue;
            const auto parts = cutRegions(ink, {}, {right, left}, ctx.orientation, true);
            if (parts.size() != 3 || toSubpaths(parts[1].path).size() != 1)
                continue;
            const auto &body = parts[1].path;
            if (!body.contains(a->first.center()) || !body.contains(a->second.center()) ||
                !body.contains(b->first.center()) || !body.contains(b->second.center()) ||
                body.boundingRect().height() > elbow->point.y() - tip->point.y() + 6 * stroke)
                continue;
            pairs.emplace_back(right, left);
        }
    }
    return pairs;
}

std::vector<BowlForkBody> recognizeBowlForks(const InkOutline &outline) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    const auto &ink = ctx.transformedPath;
    const auto tail = std::prev(ctx.xSorted.end());
    std::vector<BowlForkBody> bodies;
    for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p) {
        if (!p->isSharp || p->point.x() <= tail->point.x() + 1.5)
            continue;
        for (auto q = ctx.timeSorted.begin(); q != ctx.timeSorted.end(); ++q) {
            const auto d = q->point - p->point;
            if (!q->isSharp || d.y() < .15 || d.y() > 2 || std::abs(d.x()) > 1.6 * d.y() || !interiorChord(ink, *p, *q))
                continue;
            const double width = std::hypot(d.x(), d.y());
            for (bool forward : {true, false}) {
                auto next = (forward ? ctx.prev(q) : ctx.next(q));
                const auto drift = next->point - q->point;
                if (drift.x() <= 0 || drift.y() <= 0)
                    continue;
                BowlForkBody body{*tail, *p, *q, forward, width, {}};
                for (const auto &peak : ctx.timeSorted)
                    if (peak.point.y() < p->point.y() - 3 && std::abs(peak.point.x() - q->point.x()) < 3 * width)
                        body.caps.push_back(peak);
                bodies.push_back(std::move(body));
            }
        }
    }
    return bodies;
}

bool cutBowlForks(SearchContext &ctx, const std::vector<BowlForkBody> &bodies, const QJsonObject &form,
                         Stretching &result) {
    const auto &ink = ctx.transformedPath;
    double best = std::numeric_limits<double>::max();
    for (const auto &body : bodies) {
        const auto *tail = &body.tail, *p = &body.upper, *q = &body.lower;
        const bool forward = body.forward;
        const double width = body.width;
        auto limit = *q;
        limit.time += (forward ? 1 : -1) * int(baseContourCurves(ink).size());
        PathExtrema entry{};
        QPointF tangent;
        if (!advanceOnContour(ink, *q, limit, forward, form.value("walkWidths").toDouble(.5) * width, entry, tangent))
            continue;
        for (auto cut : cornerSections(ctx, entry)) {
            orientSection(ctx, cut, tail->time, true);
            const auto side = getJoinTime(cut, ctx.orientation);
            if (!side.isStrictLeft(p->time) || !side.isStrictLeft(q->time))
                continue;
            const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
            if (parts.size() != 2 || toSubpaths(parts[1].path).size() != 1)
                continue;
            auto box = parts[1].path.boundingRect();
            if (p->point.y() - box.top() < form.value("minRise").toDouble(3) ||
                box.right() > q->point.x() + 2.6 * width)
                continue;
            const bool cap = std::any_of(body.caps.begin(), body.caps.end(),
                                         [&](const auto &peak) { return side.isStrictLeft(peak.time); });
            const double score = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length() + .2 * width;
            if (cap && score < best) {
                best = score;
                result = cut;
            }
        }
    }
    return std::isfinite(best) && best < std::numeric_limits<double>::max();
}

CounterAttachmentBody recognizeCounterAttachments(const InkOutline &outline, const QJsonObject &form) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    CounterAttachmentBody result{form.contains("forkRise"), {}, {}};
    if (!result.fork) {
        const auto route = recognizeSavedRoute(context, form.value("outer").toObject());
        if (!route || !route->anchor.isSharp)
            return result;
        result.outer = route->anchor;
    }
    QVector<PathExtrema> points;
    std::vector<QPainterPath> paths;
    ShapeItem::getExtrema(outline.ink, paths, points);
    for (size_t index = 1; index < context.subPaths.size(); ++index) {
        const auto &hole = context.subPaths[index];
        const auto box = hole.boundingRect();
        if (box.height() <= 0 ||
            (result.fork ? !context.subPaths.front().contains(hole) : outline.ink.contains(box.center())))
            continue;
        CounterAttachmentLoop loop{
            int(index), box, {}, {}, recognizeLoopShoulders(outline, box, form.value("shoulder").toObject())};
        for (const auto &point : points)
            if (point.pathIndex == index)
                loop.inner.push_back(point);
        if (result.fork)
            for (auto vertex = context.timeSorted.begin(); vertex != context.timeSorted.end(); ++vertex) {
                double lean[2]{};
                int side = 0;
                for (auto advance : {&SearchContext::prev, &SearchContext::next}) {
                    auto current = vertex;
                    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                        auto next = (context.*advance)(current);
                        if (next->point.y() > current->point.y() + .06 * box.height())
                            break;
                        if (vertex->point.y() - next->point.y() >= form.value("forkRise").toDouble() * box.height()) {
                            lean[side] = next->point.x() - vertex->point.x();
                            break;
                        }
                        current = next;
                    }
                    ++side;
                }
                if (lean[0] * lean[1] < 0)
                    loop.forks.push_back(*vertex);
            }
        result.loops.push_back(std::move(loop));
    }
    return result;
}

bool cutCounterAttachments(SearchContext &context, const CounterAttachmentBody &body, const QJsonObject &form,
                                  const CounterAttachmentLoop **selected) {
    const bool fork = body.fork, stemSide = form.value("lowerBridge") == "stem-side";
    if (!fork && !body.outer)
        return false;
    PathExtrema outer = body.outer.value_or(PathExtrema{});
    const auto &ink = context.transformedPath;
    for (const auto &loop : body.loops) {
        const auto index = loop.index;
        const auto &hole = context.subPaths[index];
        const auto box = loop.box;
        const bool atBaseline = stemSide || form.value("lowerBridge") == "bottom-extrema";
        std::optional<PathExtrema> left, right;
        for (const auto &point : loop.inner) {
            if ((point.isSharp || (stemSide && point.isX)) &&
                std::abs(point.point.x() - box.left()) < (fork ? .04 * box.height() : 1e-5) &&
                (!fork || !left || point.point.y() < left->point.y()))
                left = point;
            if (atBaseline ? point.isY && std::abs(point.point.y() - box.bottom()) < 1e-5
                           : point.isSharp && std::abs(point.point.x() - box.right()) < 1e-5)
                right = point;
        }
        if (!left || !right)
            continue;
        QPointF bendWeights;
        const auto bridge = [&](const QPointF &from, QPointF &weights) {
            weights = {};
            if (interiorInkSegment(ink, from, left->point))
                return true;
            const double limit = form.value("maxBridgeBend").toDouble();
            if (limit <= 0)
                return false;
            const auto along = left->point - from, across = right->point - from;
            const auto cross = [](QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); };
            const double determinant = cross(along, across);
            if (std::abs(determinant) < 1e-6)
                return false;
            const QPointF normal(-along.y(), along.x());
            for (double fraction : {.2, .4, .65, 1.})
                for (double side : {-1., 1.})
                    for (double t : {.5, .33, .67}) {
                        const auto delta = t * along + side * limit * fraction * normal;
                        const auto turn = from + delta;
                        if (interiorInkSegment(ink, from, turn) && interiorInkSegment(ink, turn, left->point)) {
                            weights = {cross(delta, across) / determinant, cross(along, delta) / determinant};
                            return true;
                        }
                    }
            return false;
        };
        if (fork) {
            bool foundFork = false;
            double nearest = box.height() * form.value("maxBridgeRatio").toDouble();
            for (const auto &vertexValue : loop.forks) {
                const auto *vertex = &vertexValue;
                const double distance = QLineF(vertex->point, left->point).length();
                QPointF weights;
                if ((!vertex->isSharp && !stemSide) || distance >= nearest || vertex->point.y() >= left->point.y() ||
                    !bridge(vertex->point, weights))
                    continue;
                outer = *vertex;
                bendWeights = weights;
                nearest = distance;
                foundFork = true;
            }
            if (!foundFork)
                continue;
        }
        if (outer.point.y() >= left->point.y() ||
            QLineF(outer.point, left->point).length() > form.value("maxBridgeRatio").toDouble() * box.height() ||
            (bendWeights.isNull() && !interiorInkSegment(ink, outer.point, left->point)))
            continue;
        auto hits =
            atBaseline
                ? std::vector<PathExtrema>{}
                : FindInterections(
                      ink, QLineF(right->point, QPointF(context.bbox.right() + box.width(), right->point.y())), true);
        if (atBaseline)
            for (const auto &point : context.timeSorted) {
                if (point.isY && point.point.y() > right->point.y() && point.point.x() >= box.left() &&
                    point.point.x() <= box.right())
                    hits.push_back(point);
            }
        std::optional<PathExtrema> exit;
        double nearest = form.value("maxBridgeRatio").toDouble() * box.height();
        for (const auto &hit : hits) {
            const double distance =
                atBaseline ? QLineF(hit.point, right->point).length() : hit.point.x() - right->point.x();
            if (hit.pathIndex != 0 || distance <= 1e-5 || distance >= nearest ||
                !interiorInkSegment(ink, right->point, hit.point))
                continue;
            exit = hit;
            nearest = distance;
        }
        if (stemSide) {
            // Walk down the upright's edge, through the counter, to the
            // point where the lower return of the other letter joins it.
            exit.reset();
            double best = std::numeric_limits<double>::infinity();
            for (const auto &notch : context.timeSorted) {
                const auto d = notch.point - left->point;
                if (d.y() < .4 * box.height() || d.y() > 2 * box.height() || std::abs(d.x()) > .65 * box.height() ||
                    !notch.isSharp)
                    continue;
                const double x = std::clamp(notch.point.x(), box.left() + .18 * box.height(), box.center().x());
                for (auto inner : FindInterections(hole, QLineF(x, box.bottom() + 1, x, box.top() - 1), true)) {
                    if (inner.point.y() < box.center().y() || !interiorInkSegment(ink, notch.point, inner.point))
                        continue;
                    const double fit = QLineF(notch.point, inner.point).length() + std::abs(d.x());
                    if (fit >= best)
                        continue;
                    best = fit;
                    inner.pathIndex = int(index);
                    right = inner;
                    exit = notch;
                }
            }
        }
        if (!exit)
            continue;
        Stretching cut{};
        cut.topBaseLine = cut.topLeft = cut.topRight = outer;
        cut.bottomBaseLine = cut.bottomLeft = cut.bottomRight = *exit;
        cut.counterEntry = *left;
        cut.counterExit = *right;
        cut.counterBendWeights = bendWeights;
        // The outer endpoints span the counter, not a single stroke. Use an
        // actual ink bridge as the scale for subsequent body recognition.
        cut.referenceWidth = QLineF(outer.point, left->point).length();
        cut.fixedRightBoundary = true;
        cut.kashidaOverrides.insert("enabled", false);
        const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
        if (pieces.size() != 2 || pieces[0].path.isEmpty() || pieces[1].path.isEmpty())
            continue;
        bool ascender = false;
        for (const auto &point : context.timeSorted) {
            if (point.point.x() >= outer.point.x() ||
                outer.point.y() - point.point.y() < form.value("minAscenderRise").toDouble() * box.height())
                continue;
            ascender |= pieces[1].path.contains(point.point + QPointF(0, box.height() * .01));
        }
        if (!ascender || pieces[0].path.boundingRect().right() < context.bbox.right() - 1e-5)
            continue;
        context.res = cut;
        if (selected)
            *selected = &loop;
        return true;
    }
    return false;
}

} // namespace MedinaCutter
