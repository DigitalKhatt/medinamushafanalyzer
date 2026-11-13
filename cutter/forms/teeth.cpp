#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

static std::vector<PathExtrema> lowerBendsAtCurveJoins(const QPainterPath& path) {
    const auto curves = baseContourCurves(path);
    std::vector<PathExtrema> bends;
    // The first control point with a different height determines the
    // direction away from the join, even when an endpoint tangent is flat.
    const auto descendsFrom = [](double height, std::array<double, 3> controls) {
        for (double y : controls)
            if (y != height) return y > height;
        return false;
    };
    for (size_t i = 0; i < curves.size(); ++i) {
        const auto& before = curves[i == 0 ? curves.size() - 1 : i - 1];
        const auto& after = curves[i];
        const auto point = after.pt1();
        if (before.pt4() == point &&
            descendsFrom(point.y(), {before.y3, before.y2, before.y1}) &&
            descendsFrom(point.y(), {after.y2, after.y3, after.y4}))
            bends.push_back({0, double(i), point, false, true});
    }
    return bends;
}

// The incoming tooth shoulder turns back to the right; its opposite inner
// bend is a local minimum on the lower contour. Recognize sharp corners and
// their rounded extrema equivalents using only the first tooth's geometry.
std::optional<Stretching> seenEntryCornerPair(SearchContext& context, const PathExtrema& tip,
                                             bool increasingTime, double maxDistance, bool includeCurveJoins) {
    const auto curveBends = includeCurveJoins ? lowerBendsAtCurveJoins(context.transformedPath)
                                             : std::vector<PathExtrema>{};
    const auto advance = increasingTime ? &SearchContext::prev : &SearchContext::next;
    auto current = context.timeSorted.find(tip);
    if (current == context.timeSorted.end()) return {};
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(current);
        if (next->point.y() <= current->point.y()) break;
        current = next;
        if ((!current->isSharp && !current->isX) ||
            current->point.x() >= context.prev(current)->point.x() ||
            current->point.x() >= context.next(current)->point.x() ||
            QLineF(current->point, tip.point).length() > maxDistance) continue;
        std::optional<Stretching> pair;
        double nearest = maxDistance;
        const auto consider = [&](const PathExtrema& bottom) {
            const double distance = QLineF(current->point, bottom.point).length();
            if (bottom.point.y() <= current->point.y() || distance >= nearest ||
                !interiorChord(context.transformedPath, *current, bottom)) return;
            Stretching cut{};
            cut.topBaseLine = *current;
            cut.bottomBaseLine = bottom;
            pair = cut;
            nearest = distance;
        };
        for (auto bottom = context.timeSorted.begin(); bottom != context.timeSorted.end(); ++bottom)
            if ((bottom->isSharp || bottom->isY) &&
                bottom->point.y() < context.prev(bottom)->point.y() &&
                bottom->point.y() < context.next(bottom)->point.y()) consider(*bottom);
        for (const auto& bottom : curveBends) consider(bottom);
        if (pair) return pair;
    }
    return {};
}

// A curved final tooth may not admit a vertical exit chord. Its lower inner
// bend supplies a normal to the branch leaving the tooth. Accept the opposite
// endpoint only on this tooth's descending flank, before its first valley.
static bool seenExitBendCut(SearchContext& context, const PathExtrema& tip,
                            bool increasingTime, double maxDistance, Stretching& cut,
                            double alongContour = 0,
                            double maxSectionWidth = std::numeric_limits<double>::infinity(),
                            double crestTolerance = 0) {
    const auto advance = increasingTime ? &SearchContext::prev : &SearchContext::next;
    auto end = context.timeSorted.find(tip);
    if (end == context.timeSorted.end()) return false;
    bool subdividedCrest = false;
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(end);
        if (next->point.y() <= end->point.y()) {
            if (crestTolerance <= 0 || QLineF(tip.point, next->point).length() > crestTolerance) break;
            subdividedCrest = true;
        }
        end = next;
    }
    if (crestTolerance > 0 && !subdividedCrest) return false;
    const double count = baseContourCurves(context.transformedPath).size();
    const auto distanceOnFlank = [&](double time) {
        const double delta = increasingTime ? time - tip.time : tip.time - time;
        return delta < 0 ? delta + count : delta;
    };
    const double reach = distanceOnFlank(end->time);
    double nearest = maxDistance;
    bool found = false;
    for (auto inner = context.timeSorted.begin(); inner != context.timeSorted.end(); ++inner) {
        const double distance = QLineF(tip.point, inner->point).length();
        if (inner->point.y() <= tip.point.y() || inner->point.x() >= tip.point.x() ||
            distance >= nearest ||
            inner->point.y() >= context.prev(inner)->point.y() ||
            inner->point.y() >= context.next(inner)->point.y()) continue;
        for (bool forward : {false, true}) {
            const auto adjacent = forward ? context.prev(inner) : context.next(inner);
            if (adjacent->point.x() >= inner->point.x()) continue;
            Stretching proposed{};
            if (!contourNormalCut(context, *inner, forward, false, proposed, true)) continue;
            if (proposed.topBaseLine.point.y() > proposed.bottomBaseLine.point.y())
                std::swap(proposed.topBaseLine, proposed.bottomBaseLine);
            if (distanceOnFlank(proposed.topBaseLine.time) > reach ||
                proposed.referenceWidth > maxSectionWidth) continue;
            if (alongContour > 0) {
                // Stay on the branch leaving the bend. A free normal through a
                // shifted midpoint can re-enter a different stroke on a steep join.
                auto limit = *adjacent;
                if (forward && limit.time <= inner->time) limit.time += count;
                if (!forward && limit.time >= inner->time) limit.time -= count;
                const double travel = alongContour * proposed.referenceWidth;
                PathExtrema shifted{};
                QPointF tangent;
                if (!advanceOnContour(context.transformedPath, *inner, limit, forward, travel, shifted, tangent) ||
                    !contourNormalCut(context, shifted, forward, false, proposed, true)) continue;
                std::swap(proposed.topBaseLine, proposed.bottomBaseLine);
                if (distanceOnFlank(proposed.topBaseLine.time) > reach) continue;
            }
            cut = proposed;
            cut.referenceWidth = QLineF(proposed.topBaseLine.point, proposed.bottomBaseLine.point).length();
            nearest = distance;
            found = true;
        }
    }
    return found;
}

// The outer flank of a folded last tooth descends directly to a leftward
// turn. The facing turn across the ink is the other end of the final's cut.
bool seenTipMeetsFold(SearchContext& context, const PathExtrema& tip, bool increasingTime,
                             const Stretching& cut) {
    auto outer = context.timeSorted.find(cut.topBaseLine);
    auto inner = context.timeSorted.find(cut.bottomBaseLine);
    if (outer == context.timeSorted.end() || inner == context.timeSorted.end()) return false;
    if (outer->point.x() > inner->point.x()) std::swap(outer, inner);
    const auto leftTurn = [&](auto point) {
        return point->point.x() < context.prev(point)->point.x() &&
               point->point.x() < context.next(point)->point.x();
    };
    if (tip.point.y() >= outer->point.y() || !leftTurn(outer) || !leftTurn(inner) ||
        inner->point.y() >= outer->point.y() ||
        !interiorChord(context.transformedPath, *outer, *inner)) return false;
    auto current = context.timeSorted.find(tip);
    while (current != outer) {
        const auto next = increasingTime ? context.prev(current) : context.next(current);
        if (next->point.x() >= current->point.x() || next->point.y() <= current->point.y()) return false;
        current = next;
    }
    return true;
}

std::vector<ToothClusterBody> recognizeToothClusters(const InkOutline &outline, const QJsonObject &form) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    const double rise = form.value("minToothRise").toDouble(.55);
    const double minPitch = form.value("minPitch").toDouble(1), maxPitch = form.value("maxPitch").toDouble(5);
    const double minLastPitch = form.value("minLastPitch").toDouble(minPitch);
    const double maxLastPitch = form.value("maxLastPitch").toDouble(maxPitch);
    std::vector<ToothClusterBody> bodies;
    for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
        for (auto first = context.timeSorted.begin(); first != context.timeSorted.end(); ++first) {
            if (first->point.x() <= context.bbox.left() || first->point.y() >= context.prev(first)->point.y() ||
                first->point.y() >= context.next(first)->point.y())
                continue;
            std::array<PathExtrema, 5> turns{};
            turns[0] = *first;
            double minimumX = first->point.x();
            auto current = first;
            auto extreme = first;
            bool down = true;
            size_t count = 1;
            for (int step = 0; step < 24 && count < turns.size(); ++step) {
                current = (context.*advance)(current);
                if (current == first || current->point.x() > first->point.x() + maxPitch)
                    break;
                minimumX = std::min(minimumX, current->point.x());
                const double delta = current->point.y() - extreme->point.y();
                if (down ? delta > 0 : delta < 0) {
                    extreme = current;
                } else if (down ? delta < -rise : delta > rise) {
                    turns[count++] = *extreme;
                    extreme = current;
                    down = !down;
                }
            }
            if (count != turns.size()) continue;
            bool convex = true;
            {
                for (int index : {0, 2, 4}) {
                    const auto at = context.timeSorted.find(turns[index]);
                    const auto incoming = at->point - context.prev(at)->point;
                    const auto outgoing = context.next(at)->point - at->point;
                    const double turn = incoming.x() * outgoing.y() - incoming.y() * outgoing.x();
                    convex &= context.orientation == Orientation::ClockWise ? turn < 0 : turn > 0;
                }
                if (form.value("convexTips").toBool() && !convex)
                    continue;
            }
            const double pitch1 = turns[0].point.x() - turns[2].point.x();
            const double pitch2 = turns[2].point.x() - turns[4].point.x();
            const bool ordinary = pitch2 <= maxLastPitch;
            const bool wide = convex && pitch2 > maxLastPitch && pitch2 <= form.value("retryMaxLastPitch").toDouble();
            if (pitch1 < minPitch || pitch1 > maxPitch || pitch2 < minLastPitch || (!ordinary && !wide) ||
                turns[1].point.y() - turns[0].point.y() < rise || turns[3].point.y() - turns[2].point.y() < rise)
                continue;

            // An adjacent ascender can form a valid up/down sequence with only
            // two teeth. A seen head has three tips of comparable height.
            // Measure their spread against their own mean pitch, so the test
            // does not use a neighboring letter, cut, or stroke-width estimate.
            const double highestTip = std::min({turns[0].point.y(), turns[2].point.y(), turns[4].point.y()});
            const double lowestTip = std::max({turns[0].point.y(), turns[2].point.y(), turns[4].point.y()});
            if (lowestTip - highestTip > form.value("maxTipHeightSpreadRatio").toDouble(1) *
                (pitch1 + pitch2) / 2) continue;

            bodies.push_back({turns, advance == &SearchContext::prev, ordinary, wide, minimumX});
        }
    }
    return bodies;
}

static double meanToothPitch(const ToothClusterBody& body) {
    return (body.turns.front().point.x() - body.turns.back().point.x()) / 2;
}

static bool followsUpperFlank(const SearchContext& context, const ToothClusterBody& body) {
    // A run on the underside can look like three teeth in reverse.
    return body.advancePrevious == (context.orientation == Orientation::CounterClockwise);
}

static std::optional<std::array<PathExtrema, 2>> seenBowlLandmarks(
        const SearchContext& context, const ToothClusterBody& body, const QRectF& bounds) {
    const auto& tip = body.turns.back();
    const double pitch = meanToothPitch(body);
    const auto tail = std::prev(context.xSorted.end());
    if (bounds.bottom() <= tip.point.y() + pitch || tip.point.x() <= tail->point.x() + pitch)
        return {};

    // The third tooth must descend into the bowl. A crest on the far
    // lip of the bowl may have the right spacing but is not a tooth.
    auto floor = context.timeSorted.find(tip);
    for (size_t i = 0; i < context.timeSorted.size(); ++i) {
        const auto next = body.advancePrevious ? context.prev(floor) : context.next(floor);
        if (next->point.y() < floor->point.y() - .03) break;
        floor = next;
    }
    if (floor->point.y() < bounds.bottom() - .8 * pitch) return {};
    return std::array{*floor, *tail};
}

static bool distinctToothTips(const SearchContext& context, const PathExtrema& a,
                             const PathExtrema& b, double penWidth, double rise) {
    if (a.time == b.time) return false;
    if (QLineF(a.point, b.point).length() > penWidth) return true;
    // Two nearby crests separated only by a nib corner are the same tooth.
    // Look for a full valley between them on the shorter contour interval.
    const auto count = baseContourCurves(context.transformedPath).size();
    const bool wraps = std::abs(a.time - b.time) > count / 2.;
    const auto advance = (b.time > a.time) != wraps ? &SearchContext::prev : &SearchContext::next;
    const auto end = context.timeSorted.find(b);
    const auto valley = std::max(a.point.y(), b.point.y()) + rise;
    for (auto point = (context.*advance)(context.timeSorted.find(a)); point != end; point = (context.*advance)(point))
        if (point->point.y() >= valley) return true;
    return false;
}

static QLineF toothCutLine(const Stretching& cut) {
    return {cut.topBaseLine.point, cut.bottomBaseLine.point};
}

static bool keepsClusterAtExit(const ToothClusterBody& cluster, const Stretching& cut, Orientation orientation) {
    const auto exit = getJoinTime(cut, orientation);
    return std::all_of(cluster.turns.begin(), cluster.turns.end(),
                       [&](const auto& point) { return exit.isStrictRight(point.time); });
}

static std::optional<Stretching> seenEntryNormal(SearchContext& context, const ToothClusterBody& cluster,
                                                 double penWidth) {
    const auto& tip = cluster.turns.front();
    const auto towardEntry = cluster.advancePrevious ? &SearchContext::next : &SearchContext::prev;
    const auto lowerTowardEntry = cluster.advancePrevious ? &SearchContext::prev : &SearchContext::next;
    auto foot = context.timeSorted.find(tip);
    if (foot == context.timeSorted.end()) return {};
    // Follow only the flank of the first tooth. A normal landing farther
    // along the incoming connector or on the next tooth is unsuitable.
    for (size_t i = 0; i < context.timeSorted.size(); ++i) {
        const auto next = (context.*towardEntry)(foot);
        if (next->point.y() <= foot->point.y()) break;
        foot = next;
    }
    const double count = baseContourCurves(context.transformedPath).size();
    const auto alongFlank = [&](double t) {
        const double distance = cluster.advancePrevious ? tip.time - t : t - tip.time;
        return distance < 0 ? distance + count : distance;
    };
    const double flank = alongFlank(foot->time);
    std::optional<Stretching> best;
    double nearest = meanToothPitch(cluster);
    for (auto lower = context.timeSorted.begin(); lower != context.timeSorted.end(); ++lower) {
        const double distance = QLineF(tip.point, lower->point).length();
        if (distance >= nearest || lower->point.y() <= tip.point.y() ||
            lower->point.y() >= context.prev(lower)->point.y() ||
            lower->point.y() >= context.next(lower)->point.y() ||
            (context.*lowerTowardEntry)(lower)->point.x() <= lower->point.x()) continue;
        Stretching cut{};
        if (!contourNormalCut(context, *lower, cluster.advancePrevious, false, cut, true) ||
            cut.bottomBaseLine.pathIndex != 0 || cut.referenceWidth > 2 * penWidth ||
            alongFlank(cut.bottomBaseLine.time) > flank) continue;
        std::swap(cut.topBaseLine, cut.bottomBaseLine);
        best = cut;
        nearest = distance;
    }
    return best;
}

static std::optional<Stretching> clusterEntryAtJunction(SearchContext& context, const ToothClusterBody& cluster,
                                                        const Stretching& current, double penWidth) {
    const auto& tip = cluster.turns.front();
    // Compare all opposing bends. An extrema flag can mark a farther bend
    // on the preceding letter even when a smooth bend faces this tooth.
    auto junction = seenEntryCornerPair(context, tip, !cluster.advancePrevious, meanToothPitch(cluster), true);
    if (!junction) junction = seenEntryNormal(context, cluster, penWidth);
    if (!junction) return {};
    orientSection(context, *junction, tip.time, true);
    const auto side = getJoinTime(*junction, context.orientation);
    if (!std::all_of(cluster.turns.begin(), cluster.turns.end(),
                     [&](const auto& point) { return side.isStrictLeft(point.time); })) return {};
    if (QLineF(junction->topBaseLine.point, current.topBaseLine.point).length() < 1e-5 &&
        QLineF(junction->bottomBaseLine.point, current.bottomBaseLine.point).length() < 1e-5) return {};
    junction->referenceWidth = toothCutLine(*junction).length();
    return junction;
}

static std::optional<Stretching> clusterExitAtBend(SearchContext& context, const ToothClusterBody& cluster,
                                                const QJsonObject& settings) {
    const SeenExitRetry retry{0, SeenExitMode::AlongContour};
    Stretching boundary{};
    if (retry.supports(settings) &&
        cutToothClusters(context, {cluster}, settings, context.bbox.left(), context.bbox.right() + 1e-6,
                         nullptr, &boundary, nullptr, nullptr, &retry) &&
        keepsClusterAtExit(cluster, boundary, context.orientation)) return boundary;
    return {};
}

static std::optional<Stretching> clusterExitAtHollow(SearchContext& context, const ToothClusterBody& cluster,
        const Stretching& current, const Stretching& receiving, double penWidth, const QJsonObject& cutSettings) {
    const auto& tip = cluster.turns.back();
    const double reach = cutSettings.value("maxExitBendDistance").toDouble(2) * meanToothPitch(cluster);
    const double maxWidth = 2 * std::max(penWidth, toothCutLine(receiving).length());
    Stretching cut{};
    if (!seenExitBendCut(context, tip, cluster.advancePrevious, reach, cut, 0, maxWidth) ||
        cut.topBaseLine.pathIndex != 0 ||
        !keepsClusterAtExit(cluster, cut, context.orientation) || !cutPrecedes(cut, receiving, context.orientation))
        return {};

    // The inward turn follows the lower bulge of this tooth. A hollow on a
    // long, bowed connector can also face the descending upper contour.
    const auto hollow = context.timeSorted.find(cut.bottomBaseLine);
    if (hollow == context.timeSorted.end()) return {};
    const auto towardTooth = cluster.advancePrevious ? &SearchContext::prev : &SearchContext::next;
    const double stroke = std::max(penWidth, cut.referenceWidth);
    auto bulge = hollow;
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*towardTooth)(bulge);
        if (next->point.y() < bulge->point.y()) break;
        if (QLineF(next->point, hollow->point).length() > 2 * stroke) return {};
        bulge = next;
    }
    if (bulge == hollow || bulge->point.x() <= hollow->point.x() ||
        std::abs(bulge->point.x() - tip.point.x()) > stroke) return {};

    if (QLineF(cut.topBaseLine.point, current.topBaseLine.point).length() < 1e-4 &&
        QLineF(cut.bottomBaseLine.point, current.bottomBaseLine.point).length() < 1e-4) return current;
    return cut;
}

static std::vector<ToothClusterExit> nearerConnectorExits(SearchContext& context, const ToothClusterBody& cluster,
        const Stretching& current, double penWidth, const QJsonObject& clusterSettings, const QJsonObject& cutsSettings) {
    const auto& tip = cluster.turns.back();
    const auto cap = toothCutLine(current);
    const double width = std::max(penWidth, cap.length());
    if (tip.point.x() - cap.center().x() <= std::max(2 * width, 1.65 * meanToothPitch(cluster))) return {};

    std::vector<ToothClusterExit> cuts;
    bool foundTurn = false;
    const auto add = [&](std::optional<Stretching> turn) {
        if (!turn) return;
        orientSection(context, *turn, tip.time, false);
        const auto excess = toothCutLine(*turn).center() - cap.center();
        if (excess.x() <= 2 * width || std::abs(excess.y()) >= .5 * excess.x() ||
            !getJoinTime(*turn, context.orientation).isStrictRight(tip.time) ||
            !cutPrecedes(*turn, current, context.orientation)) return;
        foundTurn = true;
        if (!keepsClusterAtExit(cluster, *turn, context.orientation)) return;
        if (std::none_of(cuts.begin(), cuts.end(), [&](const auto& cut) {
                return toothCutLine(cut.cut) == toothCutLine(*turn);
            })) cuts.push_back({*turn});
    };
    add(strokeFlankTurn(context, tip, penWidth, -1, true, 43, .65));
    // The tip can lean back. The turn may be clearer walking from the cap.
    add(strokeFlankTurn(context, current.topBaseLine, width, 1, false, 43, 2));
    add(strokeFlankTurn(context, current.topBaseLine, width, 1, false, 25, 2));
    add(strokeFlankTurn(context, current.bottomBaseLine, width, 1, false, 43, 2));
    if (!cuts.empty() || !foundTurn || tip.point.x() <= cluster.turns[3].point.x()) return cuts;

    // A normal at the first turn can cross a backturned tooth's valley.
    // Try the existing offset along its own exit bend.
    const auto bend = clusterExitAtBend(context, cluster, mergeShapeSettings(clusterSettings, cutsSettings));
    if (bend && toothCutLine(*bend).center().x() - cap.center().x() >= .5 * width &&
        cutPrecedes(*bend, current, context.orientation)) cuts.push_back({*bend});
    return cuts;
}

static std::optional<Stretching> exitAfterFoldedTooth(SearchContext& context, const ToothClusterBody& cluster,
        const QJsonObject& settings, const Stretching& current, double penWidth) {
    const auto& valley = cluster.turns[3];
    const auto& tip = cluster.turns.back();
    const auto cap = toothCutLine(current);
    // A shared seam can start in the valley before a tooth that bends back
    // to the right. Its peak is then across the seam despite its page x.
    if (QLineF(valley.point, cap.p1()).length() > .08 * penWidth ||
        tip.point.x() <= valley.point.x() ||
        getJoinTime(current, context.orientation).isRight(tip.time)) return {};

    const double reach = std::max(2 * penWidth, meanToothPitch(cluster));
    const auto boundary = clusterExitAtBend(context, cluster, settings);
    if (boundary && QLineF(cap.center(), toothCutLine(*boundary).center()).length() <= reach &&
        cutPrecedes(current, *boundary, context.orientation)) return boundary;
    return {};
}

ToothClusterFit fitToothCluster(SearchContext& context, const QJsonObject& body, const QJsonObject& cuts,
                               const QPainterPath& mask, bool terminal, double penWidth,
                               const ToothClusterAttachment& attachment, bool proposeBoundaries) {
    const auto evidence = recognizedSharedShape(context, body);
    const ToothClusterBody* accepted = nullptr;
    const auto clusterSettings = body.value("cluster").toObject();
    const auto cutSettings = cuts.value("cluster").toObject();
    const auto rise = clusterSettings.value("minToothRise").toDouble(.55);
    QString reason = QStringLiteral("mask does not own all three teeth and both valleys");
    const auto ownedBounds = mask.boundingRect().adjusted(-1e-4, -1e-4, 1e-4, 1e-4);
    const auto entry = attachment.entry ? getJoinTime(*attachment.entry, context.orientation) : JoinTime{};
    const auto exit = attachment.exit ? getJoinTime(*attachment.exit, context.orientation) : JoinTime{};
    std::vector<std::vector<QBezier>> maskCurves;
    const auto owns = [&](PathExtrema point) {
        if (!ownedBounds.contains(point.point)) return false;
        if (attachment.sameContour)
            return (!attachment.entry || entry.isStrictLeft(point.time)) &&
                   (!attachment.exit || exit.isStrictRight(point.time));
        // A recovered component can have different contour numbering.
        if (maskCurves.empty())
            for (const auto& contour : toSubpaths(mask)) maskCurves.push_back(baseContourCurves(contour));
        return sourceLandmark(point, maskCurves);
    };
    const auto upper = [&](const ToothClusterBody& cluster) {
        return (cluster.ordinary || cluster.wide) && followsUpperFlank(context, cluster);
    };
    for (const auto& cluster : evidence->clusters) {
        if (!upper(cluster) || !std::all_of(cluster.turns.begin(), cluster.turns.end(), owns)) continue;
        if (terminal) {
            const auto bowl = seenBowlLandmarks(context, cluster, mask.boundingRect());
            if (!bowl || !std::all_of(bowl->begin(), bowl->end(), owns)) {
                reason = QStringLiteral("third tooth does not descend into the owned terminal bowl");
                continue;
            }
        } else {
            const bool meetsFold = attachment.followingFold &&
                seenTipMeetsFold(context, cluster.turns.back(), cluster.advancePrevious, *attachment.followingFold);
            if ((attachment.requiresFold && !meetsFold) || (meetsFold && attachment.separateConnector)) {
                reason = QStringLiteral("third tooth and the folded yeh require one shared cut");
                continue;
            }
        }
        if (accepted) {
            for (int i : {0, 2, 4})
                if (distinctToothTips(context, cluster.turns[i], accepted->turns[i], penWidth, rise))
                    return {QStringLiteral("more than one distinct set of teeth supports the mask"), {}};
        } else accepted = &cluster;
    }
    if (accepted) {
        ToothClusterFit fit{};
        if (!proposeBoundaries) return fit;
        if (attachment.sameContour && attachment.entry && cutSettings.value("entryAtJunction").toBool())
            fit.entry = clusterEntryAtJunction(context, *accepted, *attachment.entry, penWidth);
        if (!attachment.exit) return fit;
        const auto hollow = attachment.canFitHollow && cutSettings.value("exitAtHollow").toBool()
            ? clusterExitAtHollow(context, *accepted, *attachment.exit, *attachment.receiving,
                                  penWidth, cutSettings) : std::nullopt;
        if (hollow && toothCutLine(*hollow) == toothCutLine(*attachment.exit)) return fit;
        if (attachment.separateConnector)
            fit.exits = nearerConnectorExits(context, *accepted, *attachment.exit, penWidth, clusterSettings, cutSettings);
        if (hollow) fit.exits.insert(fit.exits.begin(), {*hollow, true});
        return fit;
    }

    if (!proposeBoundaries || terminal || !attachment.sameContour || !attachment.exit || attachment.separateConnector ||
        attachment.requiresFold || attachment.followingFold) return {reason, {}};

    const auto settings = mergeShapeSettings(clusterSettings, cutSettings);
    std::optional<Stretching> shared;
    for (const auto& cluster : evidence->clusters) {
        if (!upper(cluster) || !std::all_of(cluster.turns.begin(), cluster.turns.begin() + 3, owns)) continue;
        const auto boundary = exitAfterFoldedTooth(context, cluster, settings, *attachment.exit, penWidth);
        if (!boundary) continue;
        if (shared && toothCutLine(*shared) != toothCutLine(*boundary)) return {reason, {}};
        shared = boundary;
    }
    return shared ? ToothClusterFit{reason, {{*shared}}} : ToothClusterFit{reason, {}};
}

bool cutToothClusters(SearchContext &context, const std::vector<ToothClusterBody> &bodies,
                             const QJsonObject &form, double minX, double maxX, Stretching *right, Stretching *left,
                             const Stretching *finalFold, std::array<PathExtrema, 3> *matchedTips,
                             const SeenExitRetry *retry) {
    const auto mode = retry ? retry->mode : SeenExitMode::Configured;
    const bool wideLastPitch = retry && retry->wideLastPitch;
    const bool retryBend = mode != SeenExitMode::Configured;
    const double rise = form.value("minToothRise").toDouble(0.55);
    const double maxPitch = form.value("maxPitch").toDouble(5);
    const auto &ink = context.transformedPath;
    const auto box = ink.boundingRect();
    const double contourLength = baseContourCurves(ink).size();
    const bool final = right && !left;
    const bool needsBowl = final && (!form.value("_penLiftExit").toBool() || form.value("terminalBowl").toBool());
    std::optional<std::pair<Stretching, Stretching>> match;
    double firstX = minX;
    for (const auto &body : bodies) {
        const auto &turns = body.turns;
        if (final && !form.value("terminalBowl").toBool() && !followsUpperFlank(context, body)) continue;
        if (turns[0].point.x() >= maxX || turns[0].point.x() <= minX || body.minimumX < minX ||
            (wideLastPitch ? !body.wide : !body.ordinary))
            continue;
        const auto advance = body.advancePrevious ? &SearchContext::prev : &SearchContext::next;
        const auto reverse = body.advancePrevious ? &SearchContext::next : &SearchContext::prev;
        const auto first = context.timeSorted.find(turns[0]);
        const double pitch1 = turns[0].point.x() - turns[2].point.x(), pitch2 = turns[2].point.x() - turns[4].point.x();
        if (finalFold && !seenTipMeetsFold(context, turns[4], body.advancePrevious, *finalFold))
            continue;
        const auto verticalCut = [&](const PathExtrema &top, Stretching &cut) {
            const auto hits = FindInterections(ink, QLineF(top.point, QPointF(top.point.x(), box.bottom() + 1)), true);
            const auto bottom = std::find_if(hits.begin(), hits.end(),
                                             [&](const auto &point) { return point.point.y() > top.point.y() + 1e-5; });
            if (bottom == hits.end() || !interiorChord(ink, top, *bottom))
                return false;
            cut.topBaseLine = top;
            cut.bottomBaseLine = *bottom;
            return true;
        };
        const auto project =
            [&](const PathExtrema &tip, double y, bool onRight, Stretching &cut) {
                const bool forward = (advance == &SearchContext::prev) != onRight;
                const auto contourDistance = [&](double time) {
                    const double delta = forward ? time - tip.time : tip.time - time;
                    return delta < 0 ? delta + contourLength : delta;
                };
                auto end = context.timeSorted.find(tip);
                if (!onRight)
                    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                        const auto next = (context.*advance)(end);
                        if (next->point.y() <= end->point.y())
                            break;
                        end = next;
                    }
                const double reach = onRight ? 4 : contourDistance(end->time);
                auto hits = FindInterections(ink, QLineF(box.left() - 1, y, box.right() + 1, y));
                double distance = maxPitch;
                bool found = false;
                for (const auto& hit : hits) {
                    const double offset = (hit.point.x() - tip.point.x()) * (onRight ? 1 : -1);
                    if (hit.pathIndex != 0 || contourDistance(hit.time) > reach ||
                        offset <= 0 || offset >= distance ||
                        hit.point.x() <= minX || hit.point.x() >= maxX)
                        continue;
                    if (!verticalCut(hit, cut))
                        continue;
                    distance = offset;
                    found = true;
                }
                return found;
            };
        Stretching r{}, l{};
        bool cornerEntry = false;
        if (right && form.value("terminalBowl").toBool() && !form.value("roundedEntry").toBool()) {
            const auto tail = std::prev(context.xSorted.end());
            const double pitch = (pitch1 + pitch2) / 2;
            // Follow the incoming flank from the first tip to its root.
            // The opposed landmark is on the underside of the same stroke.
            auto upper = first;
            double best = std::numeric_limits<double>::infinity();
            for (int step = 0; step < 4; ++step) {
                const auto next = (context.*reverse)(upper);
                if (next->point.y() < upper->point.y() - .06 ||
                    QLineF(next->point, first->point).length() > 1.15 * pitch)
                    break;
                upper = next;
                if (upper->point.y() - first->point.y() < .25)
                    continue;
                for (const auto &lower : context.timeSorted) {
                    const auto d = lower.point - upper->point;
                    if (d.y() < .3 || d.y() > 2.5 || std::abs(d.x()) > 1.7 * d.y() ||
                        !interiorChord(ink, *upper, lower))
                        continue;
                    Stretching candidate{};
                    candidate.topBaseLine = *upper;
                    candidate.bottomBaseLine = lower;
                    if (getJoinTime(candidate, context.orientation).isStrictRight(tail->time))
                        std::swap(candidate.topBaseLine, candidate.bottomBaseLine);
                    const auto side = getJoinTime(candidate, context.orientation);
                    if (!side.isStrictLeft(turns[0].time) || !side.isStrictLeft(turns[2].time) ||
                        !side.isStrictLeft(turns[4].time))
                        continue;
                    const double fit = std::hypot(d.x(), d.y()) + .35 * std::abs(upper->point.y() - turns[1].point.y());
                    if (fit < best) {
                        best = fit;
                        r = candidate;
                        cornerEntry = true;
                    }
                }
            }
        }
        if (right && form.value("roundedEntry").toBool()) {
            const auto tail = std::prev(context.xSorted.end());
            // Stay on the incoming stroke beside the first tooth, including
            // when the contour normal from its sharp tip crosses the tooth.
            for (double offset : {.25, .5, .75}) {
                Stretching vertical{};
                if (!medialChordAt(context, turns[0].point.x() + offset, turns[1].point.y(), 2.5, vertical))
                    continue;
                auto normal = vertical;
                const bool hasNormal = strokeNormalCut(context, normal);
                for (auto option : {hasNormal ? normal : vertical, vertical}) {
                    if (getJoinTime(option, context.orientation).isStrictRight(tail->time))
                        std::swap(option.topBaseLine, option.bottomBaseLine);
                    const auto side = getJoinTime(option, context.orientation);
                    const QLineF chord(option.topBaseLine.point, option.bottomBaseLine.point);
                    if (chord.length() < .3 || chord.length() > 2.8 || !side.isStrictLeft(turns[0].time) ||
                        !side.isStrictLeft(turns[2].time) || !side.isStrictLeft(turns[4].time))
                        continue;
                    r = option;
                    cornerEntry = true;
                    break;
                }
                if (cornerEntry)
                    break;
            }
        }
        if (right && !cornerEntry && form.contains("rightOffset")) {
            const auto entry =
                seenEntryCornerPair(context, turns[0], reverse == &SearchContext::prev, (pitch1 + pitch2) / 2);
            if (entry) {
                const QLineF edge(entry->topBaseLine.point, entry->bottomBaseLine.point);
                const double stroke = edge.length();
                const double offset = form.value("rightOffset").toDouble();
                if (offset == 0) {
                    r = *entry;
                    orientSection(context, r, turns[0].time, true);
                    cornerEntry = true;
                } else {
                    const auto pivot = edge.center();
                    // Shift into the incoming join by a fraction of the width
                    // between these landmarks, then use the local stroke normal.
                    cornerEntry = medialChordAt(context, pivot.x() + offset * stroke, pivot.y(), 2 * stroke, r) &&
                        strokeNormalCut(context, r);
                }
                if (cornerEntry)
                    r.referenceWidth = stroke;
            }
            if (!cornerEntry && !form.value("entryCornerOptional").toBool())
                continue;
        }
        if (right && !cornerEntry) {
            auto foot = first;
            for (int step = 0; step < 4; ++step) {
                const auto next = (context.*reverse)(foot);
                if (next->point.y() <= foot->point.y() || next->point.x() >= maxX)
                    break;
                foot = next;
            }
            const bool atFoot = (left || form.value("terminalBowl").toBool()) &&
                                foot->point.y() - first->point.y() >= rise &&
                                std::abs(foot->point.x() - first->point.x()) <= maxPitch && verticalCut(*foot, r);
            if (!atFoot && !project(turns[0], turns[1].point.y(), true, r))
                continue;
        }
        bool intrinsicExit = finalFold != nullptr;
        if (finalFold)
            l = *finalFold;
        else if (left && (retryBend || form.value("exitBendFirst").toBool())) {
            const double radius = form.value("maxExitBendDistance").toDouble(2) * (pitch1 + pitch2) / 2;
            const auto offset = retryBend ? SeenExitRetry::offset(form, mode) : form.value("exitOffset");
            const bool followContour = mode == SeenExitMode::AlongContour;
            intrinsicExit = seenExitBendCut(context, turns[4], advance == &SearchContext::prev, radius, l,
                                            followContour ? offset.toDouble() : 0);
            if (intrinsicExit && !followContour && offset.isDouble())
                intrinsicExit = strokeNormalCut(context, l, -offset.toDouble());
            if (retryBend && !intrinsicExit)
                continue;
        }
        if (left && !intrinsicExit &&
            !project(turns[4], turns[3].point.y() + form.value("exitDrop").toDouble(0.05), false, l)) {
            auto foot = context.timeSorted.find(turns[4]);
            for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                const auto next = (context.*advance)(foot);
                if (next->point.y() <= foot->point.y())
                    break;
                foot = next;
            }
            const double y = foot->point.y() - form.value("exitDrop").toDouble(.05);
            if (y <= turns[4].point.y() || !project(turns[4], y, false, l)) {
                const double radius = form.value("maxExitBendDistance").toDouble() * (pitch1 + pitch2) / 2;
                if (!seenExitBendCut(context, turns[4], advance == &SearchContext::prev, radius, l)) {
                    // The end of a rounded crest can have two nearly
                    // coincident extrema before its descending flank.
                    if (!seenExitBendCut(context, turns[4], advance == &SearchContext::prev,
                            pitch1 + pitch2, l, 0, (pitch1 + pitch2) / 2, rise * .05)) continue;
                }
            }
        }
        // A closed loop can have a crest resembling a third tooth. Check
        // the proposed body itself so a following loop is not swallowed.
        std::vector<Stretching> cuts;
        if (right)
            cuts.push_back(r);
        if (left)
            cuts.push_back(l);
        const auto pieces = cutRegions(ink, {}, cuts, context.orientation, true);
        if (pieces.size() != cuts.size() + 1 || toSubpaths(pieces[right ? 1 : 0].path).size() != 1)
            continue;
        if (needsBowl) {
            const auto bowl = seenBowlLandmarks(context, body, pieces[1].path.boundingRect());
            const auto entry = getJoinTime(r, context.orientation);
            if (!bowl || !entry.isStrictLeft(turns.front().time) ||
                !std::all_of(bowl->begin(), bowl->end(), [&](const auto& point) { return entry.isStrictLeft(point.time); }))
                continue;
        }
        if (match && first->point.x() <= firstX)
            continue;
        firstX = first->point.x();
        if (matchedTips)
            *matchedTips = {turns[0], turns[2], turns[4]};
        match = std::pair{r, l};
    }
    if (!match) {
        // A shifted normal can cross the tip of a tight first tooth. The
        // original facing corners define a second, unshifted entry.
        if (!right || !left || !form.value("allowUnshiftedEntry").toBool() ||
            form.value("rightOffset").toDouble() <= 0) return false;
        auto unshifted = form;
        unshifted.insert("rightOffset", 0);
        return cutToothClusters(context, bodies, unshifted, minX, maxX, right, left, finalFold, matchedTips, retry);
    }
    if (right) *right = match->first;
    if (left) *left = match->second;
    return true;
}

// Geometry-only callers and tests use the same recognizer and projection.
bool seenTeethBoundaries(SearchContext &context, const QJsonObject &form, double minX, double maxX,
                                Stretching *right, Stretching *left, const Stretching *finalFold,
                                std::array<PathExtrema, 3> *matchedTips,
                                const SeenExitRetry *retry) {
    return cutToothClusters(context, recognizeToothClusters(inkOutline(context), form), form, minX, maxX, right, left,
                            finalFold, matchedTips, retry);
}

// A loop's tooth can have a descending spur on the underside. A normal
// through the tooth root may land on the wrong side of that spur. Use the
// narrow section at the return corner when it keeps both tips on the body.
Stretching loopToothExit(const SearchContext& context, const ToothBody& tooth) {
    const auto& cut = tooth.left;
    const auto& upper = cut.topBaseLine.point;
    const auto& lower = cut.bottomBaseLine.point;
    if (lower.x() <= tooth.tip.point.x()) return cut;
    const auto previous = getJoinTime(cut, context.orientation);
    const double cutLength = QLineF(upper, lower).length();
    double nearest = cutLength;
    Stretching result = cut;
    for (const auto& corner : context.timeSorted) {
        const auto p = corner.point;
        const double gap = std::abs(p.x() - upper.x());
        if (!corner.isSharp || !corner.isY || gap >= nearest || p.x() >= tooth.tip.point.x() ||
            p.y() <= tooth.bend.point.y() || !previous.isStrictLeft(corner.time)) continue;
        const auto hits = FindInterections(context.transformedPath,
            QLineF(p, QPointF(p.x(), context.bbox.top() - cutLength)), true);
        for (const auto& top : hits) {
            const double width = p.y() - top.point.y();
            if (width <= 1e-6 || width * 2 >= cutLength || gap > width ||
                !interiorChord(context.transformedPath, top, corner)) continue;
            auto candidate = cut;
            candidate.topBaseLine = top;
            candidate.bottomBaseLine = corner;
            candidate.referenceWidth = width;
            const auto side = getJoinTime(candidate, context.orientation);
            if (!side.isStrictRight(cut.bottomBaseLine.time) ||
                !side.isStrictRight(tooth.tip.time) || !side.isStrictRight(tooth.bend.time)) continue;
            const bool retainsSpur = std::any_of(context.timeSorted.begin(), context.timeSorted.end(), [&](const auto& point) {
                return point.isY && point.point.x() > p.x() && point.point.x() < lower.x() &&
                    point.point.y() > std::max(p.y(), lower.y()) + width &&
                    previous.isStrictLeft(point.time) && side.isStrictRight(point.time);
            });
            if (!retainsSpur) continue;
            nearest = gap;
            result = candidate;
            break;
        }
    }
    return result;
}

static std::vector<ToothEntryLandmark> recognizeToothEntries(SearchContext& context,
        const PathExtrema& tip, double stroke, const QJsonObject& entry) {
    std::vector<ToothEntryLandmark> result;
    if (entry.isEmpty() || stroke <= 0) return result;
    const auto peak = context.timeSorted.find(tip);
    if (peak == context.timeSorted.end()) return result;
    for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
        auto foot = peak;
        for (size_t step = 1; step < context.timeSorted.size(); ++step) {
            const auto next = (context.*advance)(foot);
            if (next->point.y() <= foot->point.y() + 1e-7) break;
            foot = next;
        }
        if (foot->point.x() <= tip.point.x() ||
            foot->point.y() - tip.point.y() < entry.value("minRise").toDouble() * stroke)
            continue;
        result.push_back({*foot, advance == &SearchContext::prev});
    }
    return result;
}

static std::optional<Stretching> cutToothEntry(SearchContext& context, const PathExtrema& tip,
        const PathExtrema& bend, double stroke, const std::vector<ToothEntryLandmark>& entries,
        const QJsonObject& policy) {
    if (policy.isEmpty()) return std::nullopt;
    for (const auto& entry : entries) {
        const auto& foot = entry.foot;
        const bool forward = entry.forward;
        Stretching cut{};
        if (!alefTurnCut(context, foot, policy.value("turnAngle").toDouble(45), stroke, cut, &tip, forward ? 1 : -1) &&
            !contourNormalCut(context, foot, forward, true, cut))
            continue;
        cut.referenceWidth = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
        const auto side = getJoinTime(cut, context.orientation);
        if (cut.referenceWidth < 2.5 * stroke && side.isStrictLeft(tip.time) && side.isStrictLeft(bend.time)) return cut;
    }
    return std::nullopt;
}

// The ordinary tip detector can enclose the crown while leaving the entire
// inclined shaft in the preceding connector. Replace that short entry only
// when the same tooth's baseline turn produces a solid, ordered body.
static void extendToothToBaseline(SearchContext& context, ToothBody& body,
        const QJsonObject& entry, const QJsonObject& policy) {
    if (!body.right) return;
    const auto chord = [](const Stretching& cut) {
        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
    };
    const double stroke = std::min(chord(*body.right).length(), chord(body.left).length());
    const auto entries = recognizeToothEntries(context, body.tip, stroke, entry);
    const auto turn = cutToothEntry(context, body.tip, body.bend, stroke, entries, policy);
    if (!turn || chord(*turn).center().y() - chord(*body.right).center().y() <= .5 * stroke) return;
    const auto side = getJoinTime(*turn, context.orientation);
    if (!side.isStrictLeft(body.left.topBaseLine.time) || !side.isStrictLeft(body.left.bottomBaseLine.time)) return;
    const auto pieces = cutRegions(context.transformedPath, {}, {*turn, body.left}, context.orientation, true);
    if (pieces.size() == 3 && toSubpaths(pieces[1].path).size() == 1 && body.retainedBy(pieces[1].path))
        body.right = turn;
}

static std::vector<BentToothBodyEvidence> recognizeToothBends(
        SearchContext& context, const QJsonObject& body) {
    const auto& ink = context.transformedPath;
    const int count = static_cast<int>(baseContourCurves(ink).size());
    std::vector<BentToothBodyEvidence> results;
    if (!count) return results;
    const auto turn = [&](auto at) {
        const auto a = at->point - context.prev(at)->point;
        const auto b = context.next(at)->point - at->point;
        const double length = std::hypot(a.x(), a.y()) * std::hypot(b.x(), b.y());
        const double cross = a.x() * b.y() - a.y() * b.x();
        return length <= 1e-10 ? 0. : (context.orientation == Orientation::ClockWise ? -cross : cross) / length;
    };
    const auto minTurn = body.value("minBendTurn").toDouble();
    const auto maxRatio = body.value("maxPairDistance").toDouble(4.);
    const bool hooked = body.value("shape") == "tooth-hook";
    const bool rounded = body.value("shape") != "tooth-bend";
    for (auto tip = context.timeSorted.begin(); tip != context.timeSorted.end(); ++tip) {
        if ((!rounded && turn(tip) <= 0) || tip->point.y() > context.prev(tip)->point.y() + 1e-6 ||
            tip->point.y() > context.next(tip)->point.y() + 1e-6)
            continue;
        for (auto bend = context.timeSorted.begin(); bend != context.timeSorted.end(); ++bend) {
            const double depth = bend->point.y() - tip->point.y();
            if (depth <= 0 || (!rounded && turn(bend) >= -minTurn)) continue;
            const double distance = QLineF(tip->point, bend->point).length();
            if (std::abs(tip->point.x() - bend->point.x()) > depth) continue;
            QVector<QPointF> curved;
            if (!interiorChord(ink, *tip, *bend)) {
                if (!hooked) continue;
                // The inner face of a rolled tip can cross its straight
                // tip-to-bend chord. Trace a connected strip inside the ink.
                const QLineF axis(tip->point, bend->point);
                for (int sample = 1; sample < 20; ++sample) {
                    auto point = axis.pointAt(sample / 20.);
                    if (!ink.contains(point)) {
                        const double reach = .2 * distance;
                        auto hits =
                            FindInterections(ink, QLineF(point - QPointF(reach, 0), point + QPointF(reach, 0)));
                        std::sort(hits.begin(), hits.end(),
                                  [](const auto& a, const auto& b) { return a.point.x() < b.point.x(); });
                        double best = reach;
                        for (size_t j = 1; j < hits.size(); ++j) {
                            const auto middle = (hits[j - 1].point + hits[j].point) / 2.;
                            const double dx = std::abs(middle.x() - axis.pointAt(sample / 20.).x());
                            if (dx < best && ink.contains(middle)) {
                                best = dx;
                                point = middle;
                            }
                        }
                        if (best == reach) break;
                    }
                    if (!curved.empty() && !ink.contains((curved.back() + point) / 2.)) break;
                    curved.append(point);
                }
                if (curved.size() != 19) continue;
            }
            ToothFlankGeometry geometry(context, *tip, *bend, count);
            std::array<Stretching, 2> sections{};
            double stroke = 0;
            if (rounded) {
                const auto above = [&](const PathExtrema& p, double length) {
                    const auto a = geometry.contourPoint(p, length, true).point;
                    const auto b = geometry.contourPoint(p, length, false).point;
                    return std::min(a.y(), b.y()) - p.point.y();
                };
                if (above(*tip, .65 * distance) < -.03 * distance ||
                    (above(*bend, .65 * distance) < -.07 * distance && turn(bend) > -minTurn))
                    continue;
                if (!geometry.projectSections(.55 * distance, sections) && (!hooked || !geometry.normalSections(.4 * distance, sections))) continue;
                stroke = std::min(sections[0].referenceWidth, sections[1].referenceWidth);
                if (distance > maxRatio * stroke || distance < body.value("minPairDistance").toDouble(.5) * stroke) continue;
            } else {
                if (!geometry.normalSections(.4 * distance, sections)) continue;
                stroke = std::min(sections[0].referenceWidth, sections[1].referenceWidth);
                if (distance > maxRatio * stroke || distance < body.value("minPairDistance").toDouble(.6) * stroke)
                    continue;
            }
            results.push_back({*tip, *bend, stroke, std::move(curved),
                recognizeToothEntries(context, *tip, stroke, body.value("baselineEntry").toObject())});
        }
    }
    return results;
}

static std::vector<ToothBody> cutToothBends(SearchContext& context,
        const std::vector<BentToothBodyEvidence>& bodies, const QJsonObject& policy) {
    const auto& ink = context.transformedPath;
    const int count = static_cast<int>(baseContourCurves(ink).size());
    std::vector<ToothBody> results;
    const double offset = policy.value("flankOffset").toDouble(.5);
    const bool hooked = policy.value("flankMode") == "contour-search";
    const bool projected = policy.value("flankMode") == "axis-projection" || hooked;
    for (const auto& evidence : bodies) {
        const auto& tip = evidence.tip;
        const auto& bend = evidence.bend;
        const double stroke = evidence.stroke;
        ToothFlankGeometry geometry(context, tip, bend, count);
        std::array<Stretching, 2> cuts{};
        if (projected) {
            if (hooked) {
                // On a curled flank the underside normal can point back
                // through the tip. Find the shortest solid section to
                // each outer flank and classify its side by contour order.
                std::array<double, 2> best{3 * stroke, 3 * stroke};
                for (bool forward : {true, false}) {
                    const auto lower = geometry.contourPoint(bend, offset * stroke, forward);
                    for (int angle = -70; angle <= 70; angle += 10) {
                        const double radians = angle * M_PI / 180;
                        const QPointF direction(std::sin(radians), -std::cos(radians));
                        const auto hits = FindInterections(
                            ink, QLineF(lower.point, lower.point + 3 * stroke * direction), true);
                        for (const auto& upper : hits) {
                            const double length = QLineF(upper.point, lower.point).length();
                            if (length < .25 * stroke || !interiorChord(ink, upper, lower)) continue;
                            Stretching sample{};
                            sample.topBaseLine = upper;
                            sample.bottomBaseLine = lower;
                            const auto side = getJoinTime(sample, context.orientation);
                            // A short chord can clip just a corner of the
                            // underside. An attachment separates the two
                            // ends of the connected writing stroke.
                            if (!side.isStrictRight(context.xSorted.begin()->time) ||
                                !side.isStrictLeft(std::prev(context.xSorted.end())->time))
                                continue;
                            int arm = side.isStrictLeft(tip.time) ? 0 : 1;
                            if ((arm == 0 && !side.isStrictLeft(bend.time)) ||
                                (arm == 1 && !side.isStrictRight(bend.time)) || length >= best[arm])
                                continue;
                            best[arm] = length;
                            sample.referenceWidth = length;
                            cuts[arm] = sample;
                        }
                    }
                }
                if (best[0] == 3 * stroke || best[1] == 3 * stroke) continue;
            } else if (!geometry.projectSections(offset * stroke, cuts))
                continue;
            for (auto& cut : cuts) {
                auto normal = cut;
                if (strokeNormalCut(context, normal)) {
                    const auto side = getJoinTime(normal, context.orientation);
                    const auto nominal = getJoinTime(cut, context.orientation);
                    if ((!hooked || (side.isStrictRight(context.xSorted.begin()->time) &&
                                     side.isStrictLeft(std::prev(context.xSorted.end())->time))) &&
                        side.isStrictLeft(tip.time) == nominal.isStrictLeft(tip.time) &&
                        side.isStrictLeft(bend.time) == nominal.isStrictLeft(bend.time))
                        cut = normal;
                }
            }
        } else if (!geometry.normalSections(offset * stroke, cuts)) continue;
        const auto entryTurn = cutToothEntry(context, tip, bend, stroke, evidence.entries,
                                             policy.value("baselineEntry").toObject());
        for (int side = 0; side < 2; ++side) {
            const auto& right = entryTurn ? *entryTurn : cuts[side];
            const auto& left = cuts[1 - side];
            const auto entry = getJoinTime(right, context.orientation),
                       exit = getJoinTime(left, context.orientation);
            if (!entry.isStrictLeft(tip.time) || !entry.isStrictLeft(bend.time) ||
                !exit.isStrictRight(tip.time) || !exit.isStrictRight(bend.time) ||
                !entry.isStrictLeft(left.topBaseLine.time) || !entry.isStrictLeft(left.bottomBaseLine.time))
                continue;
            if (std::max(right.referenceWidth, left.referenceWidth) > policy.value("maxSectionWidth").toDouble(2.5) * stroke) {
                if (inspectingForm) inspectReason("Cuts", QString("Section width exceeds maxSectionWidth (%1 × local stroke).")
                    .arg(policy.value("maxSectionWidth").toDouble(2.5)));
                continue;
            }
            const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
            if (pieces.size() != 3 || toSubpaths(pieces[1].path).size() != 1) continue;
            const auto& body = pieces[1].path;
            const QLineF axis(tip.point, bend.point);
            ToothBody tooth{tip, bend, left, right, evidence.axis};
            if (!tooth.retainedBy(body)) {
                inspectReason("Cuts", "The projected mask would discard part of the tooth core.");
                continue;
            }
            if (evidence.axis.isEmpty() && (!body.contains(axis.pointAt(.25)) || !body.contains(axis.pointAt(.5)) ||
                                     !body.contains(axis.pointAt(.75))))
                continue;
            results.push_back(std::move(tooth));
            break;
        }
    }
    return results;
}

BentToothRecognition recognizeBentToothBodies(const InkOutline& outline, const QJsonObject& body) {
    WordResultInfo::SubWordInfo empty{};
    auto local = outline.geometry(empty);
    if (body.value("shape") == "tooth-bend") return {false, {}, recognizeToothBends(local, body)};
    const auto curves = baseContourCurves(local.transformedPath);
    for (int i = 0; i < static_cast<int>(curves.size()); ++i) {
        const auto p = curves[i].pt1();
        const auto a = curves[(i+curves.size()-1)%curves.size()].pointAt(.97);
        const auto b = curves[i].pointAt(.03);
        if ((a.y()-p.y())*(b.y()-p.y()) <= 0) continue;
        const bool already = std::any_of(local.timeSorted.begin(), local.timeSorted.end(), [&](const auto& e) {
            return std::abs(e.time-i)<.05 && QLineF(e.point,p).length()<.01;
        });
        if (!already) local.timeSorted.insert({0,double(i),p,false,true});
    }
    auto bodies = recognizeToothBends(local, body);
    return {true, std::move(local.timeSorted), std::move(bodies)};
}

std::vector<ToothBody> cutBentToothBodies(SearchContext& context,
        const BentToothRecognition& evidence, const QJsonObject& policy) {
    if (!evidence.refined) return cutToothBends(context, evidence.bodies, policy);
    SearchContext local{.transformedPath=context.transformedPath, .xSorted=context.xSorted,
        .timeSorted=evidence.landmarks, .bbox=context.bbox, .subword=context.subword, .orientation=context.orientation,
        .outlineCache=context.outlineCache};
    return cutToothBends(local, evidence.bodies, policy);
}

std::vector<ToothBodyEvidence> recognizeToothBodies(
        const InkOutline& outline, const QJsonObject& settings, StrokeNormalCache* normals) {
    WordResultInfo::SubWordInfo empty{};
    auto context = outline.geometry(empty);
    const auto& ink = context.transformedPath;
    std::vector<ToothBodyEvidence> results;
    // Derivative roots and curve endpoints can name the same extremum
    // within floating-point precision. It is one tip, with two real flanks.
    const auto adjacent = [&](auto point, auto advance) {
        auto next = (context.*advance)(point);
        while (next != point && std::abs(next->time - point->time) < 1e-7)
            next = (context.*advance)(next);
        return next;
    };
    const auto duplicate = [&](auto point) {
        return point != context.timeSorted.begin() && std::abs(std::prev(point)->time - point->time) < 1e-7;
    };
    const auto minimum = [&](auto point) {
        return point->point.y() < adjacent(point, &SearchContext::prev)->point.y() &&
               point->point.y() < adjacent(point, &SearchContext::next)->point.y();
    };
    const auto innerMinimum = [&](auto point) {
        if (minimum(point)) return true;
        if (!settings.value("allowFlatBend").toBool()) return false;
        // A short horizontal contour segment can split one inner minimum into
        // two equal-height endpoints. Examine the flanks of that plateau.
        for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
            auto flank = adjacent(point, advance);
            for (size_t step = 0; step < context.timeSorted.size() &&
                 std::abs(flank->point.y() - point->point.y()) < 1e-7; ++step)
                flank = adjacent(flank, advance);
            if (flank->point.y() <= point->point.y() + 1e-7) return false;
        }
        return true;
    };
    // Classify the outer tips and inner bends once. Their flank topology is
    // intrinsic to the outline, so each tip need not reclassify every point.
    std::vector<timesortedtype::const_iterator> tips, innerBends;
    for (auto at = context.timeSorted.begin(); at != context.timeSorted.end(); ++at) {
        if (duplicate(at)) continue;
        if (minimum(at)) tips.push_back(at);
        if (innerMinimum(at)) innerBends.push_back(at);
    }
    for (const auto peak : tips) {
        const auto tip = peak->point;
        const auto flank = [&](auto advance) {
            std::vector<PathExtrema> points{*peak};
            auto current = peak;
            for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                const auto next = adjacent(current, advance);
                if (next == peak || next->point.y() <= current->point.y()) break;
                points.push_back(*next);
                current = next;
            }
            return points;
        };
        auto reverseFlank = flank(&SearchContext::next), forwardFlank = flank(&SearchContext::prev);
        if (reverseFlank.size() < 2 || forwardFlank.size() < 2) continue;
        const bool reverseIsRight = reverseFlank.back().point.x() > forwardFlank.back().point.x();
        const auto& rightFlank = reverseIsRight ? reverseFlank : forwardFlank;
        const auto& leftFlank = reverseIsRight ? forwardFlank : reverseFlank;
        const auto baselineEntry = settings.value("baselineEntry").toObject();
        std::optional<Stretching> footEntry;
        double entryStroke = 0;
        if (!baselineEntry.isEmpty()) {
            // The tooth's own descending flank ends at the baseline turn.
            // Its cross-section provides the scale, without a neighbor's join.
            const auto& anchor = rightFlank.back();
            const auto below = FindInterections(ink, QLineF(anchor.point,
                QPointF(anchor.point.x(), context.bbox.bottom() + context.bbox.height())), true);
            for (const auto& bottom : below) {
                if (bottom.point.y() <= anchor.point.y() || !interiorChord(ink, anchor, bottom)) continue;
                Stretching cut{};
                cut.topBaseLine = anchor;
                cut.bottomBaseLine = bottom;
                if (strokeNormalCut(context, cut, 0, normals)) footEntry = cut;
                break;
            }
            if (footEntry) {
                const double width = QLineF(footEntry->topBaseLine.point, footEntry->bottomBaseLine.point).length();
                if (anchor.point.x() <= tip.x() || anchor.point.y() - tip.y() <
                    baselineEntry.value("minRise").toDouble() * width) footEntry.reset();
                else entryStroke = width;
            }
        }
        const auto onFlank = [&](const PathExtrema& point, const auto& points) {
            if (point.pathIndex != 0) return false;
            const bool forward = &points == &forwardFlank;
            for (size_t i = 1; i < points.size(); ++i) {
                const double from = forward ? points[i - 1].time : points[i].time;
                const double to = forward ? points[i].time : points[i - 1].time;
                const bool after = point.time >= from - 1e-6, before = point.time <= to + 1e-6;
                if (from <= to ? after && before : after || before) return true;
            }
            return false;
        };
        std::vector<PathExtrema> bends;
        for (const auto inner : innerBends)
            if (inner->point.y() > tip.y() && interiorChord(ink, *peak, *inner))
                bends.push_back(*inner);
        std::sort(bends.begin(), bends.end(), [&](const auto& x, const auto& y) {
            return QLineF(tip, x.point).length() < QLineF(tip, y.point).length();
        });
        ToothBodyEvidence body{*peak, forwardFlank, reverseFlank, reverseIsRight, footEntry, entryStroke,
            baselineEntry.value("required").toBool(), baselineEntry.value("minRise").toDouble(), {}};
        for (const auto& inner : bends) {
            const double distance = QLineF(tip, inner.point).length();
            // Sample the joining stroke just past the inner bend, on the
            // tooth's descending flank. Normalization belongs to this tooth.
            Stretching local{};
            if (!medialChordAt(context, inner.point.x() - 0.5 * distance, inner.point.y(), distance, local, true) ||
                !onFlank(local.topBaseLine, leftFlank)) {
                local.topBaseLine = *peak;
                local.bottomBaseLine = inner;
            }
            const double localWidth = QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length();
            const auto measure = [&](double stroke) -> std::optional<ToothScaleEvidence> {
                if (stroke <= 0 || distance < settings.value("minPairDistance").toDouble() * stroke ||
                    distance > settings.value("maxPairDistance").toDouble(2.5) * stroke) {
                    if (inspectingForm) inspectReason("Body", QString("Tip–bend distance outside [%1, %2] × local stroke.")
                        .arg(settings.value("minPairDistance").toDouble()).arg(settings.value("maxPairDistance").toDouble(2.5)));
                    return std::nullopt;
                }
                const double rise = std::min(rightFlank.back().point.y(), leftFlank.back().point.y()) - tip.y();
                if (rise < settings.value("minFlankRise").toDouble() * stroke) {
                    inspectReason("Body", "Flank rise is below minFlankRise.");
                    return std::nullopt;
                }
                const double drop = std::min(settings.value("flankDrop").toDouble(0.6) * stroke, 0.95 * rise);
                const auto hits = FindInterections(ink, QLineF(context.bbox.left() - 1, tip.y() + drop,
                                                              context.bbox.right() + 1, tip.y() + drop), true);
                const auto foot = [&](const auto& points) -> std::optional<QPointF> {
                    for (const auto& hit : hits) if (onFlank(hit, points)) return hit.point;
                    return std::nullopt;
                };
                const auto r = foot(rightFlank), l = foot(leftFlank);
                if (!r || !l) return std::nullopt;
                const double width = r->x() - l->x();
                if (r->x() + settings.value("maxTipOverhang").toDouble(0.12) * stroke < tip.x() ||
                    tip.x() < l->x() || width < settings.value("minWidth").toDouble(0.2) * stroke ||
                    width > settings.value("maxWidth").toDouble(3) * stroke) {
                    inspectReason("Body", "Tooth width or tip overhang is outside the configured limits.");
                    return std::nullopt;
                }
                return ToothScaleEvidence{stroke, hits};
            };
            auto localScale = measure(localWidth);
            auto entryScale = footEntry ? (entryStroke == localWidth ? localScale : measure(entryStroke)) : std::nullopt;
            if (localScale || entryScale)
                body.pairs.push_back({inner, local, std::move(localScale), std::move(entryScale)});
        }
        if (!body.pairs.empty()) results.push_back(std::move(body));
    }
    return results;
}

static bool retainsTooth(const SearchContext& context, const Stretching& cut, QPointF tip, QPointF bend) {
    const auto pieces = cutRegions(context.transformedPath, {}, {cut}, context.orientation, true);
    return pieces.size() == 2 && pieces[0].path.contains(tip + .05 * (bend - tip)) &&
        pieces[0].path.contains(tip + .95 * (bend - tip));
}

// Only this stage places the normal sections and chooses an accepted pair of
// boundaries. Candidate order and all offsets retain the previous geometry.
std::vector<ToothBody> cutToothBodies(SearchContext& context,
        const std::vector<ToothBodyEvidence>& bodies, const QJsonObject& settings, bool bothSides) {
    const auto& ink = context.transformedPath;
    const double rightOffset = settings.value("rightOffset").toDouble(.5);
    std::vector<ToothBody> results;
    const auto adjacent = [&](auto point, auto advance) {
        auto next = (context.*advance)(point);
        while (next != point && std::abs(next->time - point->time) < 1e-7)
            next = (context.*advance)(next);
        return next;
    };
    for (const auto& evidence : bodies) {
        const auto tip = evidence.tip.point;
        const auto& forwardFlank = evidence.forwardFlank;
        const auto& reverseFlank = evidence.reverseFlank;
        const auto& rightFlank = evidence.reverseIsRight ? reverseFlank : forwardFlank;
        const auto& leftFlank = evidence.reverseIsRight ? forwardFlank : reverseFlank;
        const auto baselineEntry = settings.value("baselineEntry").toObject();
        auto footEntry = evidence.baselineSection;
        if (footEntry) {
            const auto& anchor = rightFlank.back();
            const auto width = evidence.entryStroke;
            if (baselineEntry.contains("turnAngle") && anchor.point.x() - tip.x() >=
                baselineEntry.value("minTurnRun").toDouble() * width) {
                Stretching turn{};
                const int direction = &rightFlank == &forwardFlank ? 1 : -1;
                if (alefTurnCut(context, anchor, baselineEntry.value("turnAngle").toDouble(),
                                width, turn, &evidence.tip, direction)) {
                    turn.referenceWidth = width;
                    footEntry = turn;
                } else if (!baselineEntry.contains("minTurnRun")) footEntry.reset();
            }
        }
        const auto onFlank = [&](const PathExtrema& point, const auto& points) {
            if (point.pathIndex != 0) return false;
            const bool forward = &points == &forwardFlank;
            for (size_t i = 1; i < points.size(); ++i) {
                const double from = forward ? points[i - 1].time : points[i].time;
                const double to = forward ? points[i].time : points[i - 1].time;
                const bool after = point.time >= from - 1e-6, before = point.time <= to + 1e-6;
                if (from <= to ? after && before : after || before) return true;
            }
            return false;
        };
        for (const auto& pair : evidence.pairs) {
            const auto& inner = pair.bend;
            const auto& local = pair.localSection;
            const double localWidth = QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length();
            // The lower flank can supply a reliable width measurement without
            // also being the cut. Only a baseline-entry policy uses that section.
            auto rightTurn = settings.contains("baselineEntry") ? footEntry : std::nullopt;
            if (!rightTurn && evidence.requiredEntry) {
                // An inclined entry can end at a corner whose vertical misses
                // the underside. Measure the tooth at its own opposite flank
                // and search the descending edge directly for its turning normal.
                const auto& root = rightFlank.back();
                const int direction = &rightFlank == &forwardFlank ? 1 : -1;
                Stretching turn{};
                if (root.point.x() <= tip.x() || root.point.y() - tip.y() <
                    evidence.minEntryRise * localWidth ||
                    !baselineEntry.contains("turnAngle") ||
                    !alefTurnCut(context, root, baselineEntry.value("turnAngle").toDouble(),
                                 localWidth, turn, &evidence.tip, direction)) continue;
                turn.referenceWidth = localWidth;
                rightTurn = turn;
            }
            const auto& profile = footEntry ? pair.entryScale : pair.localScale;
            if (!profile) continue;
            const double stroke = profile->stroke;
            const auto& hits = profile->flankSamples;
            Stretching right{}, left{};
            // Keep the known bend as the lower endpoint. Re-discovering an
            // exact curve endpoint by intersection can lose it to rounding.
            const auto leftHits = FindInterections(ink, QLineF(inner.point,
                QPointF(inner.point.x(), context.bbox.top() - stroke)), true);
            double shortest = 2 * stroke;
            bool foundLeft = false;
            for (const auto& top : leftHits) {
                const double length = QLineF(top.point, inner.point).length();
                if (top.point.y() >= inner.point.y() || length >= shortest ||
                    !onFlank(top, leftFlank) || !interiorChord(ink, top, inner)) continue;
                left.topBaseLine = top;
                left.bottomBaseLine = inner;
                shortest = length;
                foundLeft = true;
            }
            if (!foundLeft) {
                for (bool previous : {false, true}) {
                    Stretching normal{};
                    if (!contourNormalCut(context, inner, previous, false, normal)) continue;
                    if (!onFlank(normal.topBaseLine, leftFlank) && onFlank(normal.bottomBaseLine, leftFlank))
                        std::swap(normal.topBaseLine, normal.bottomBaseLine);
                    if (!onFlank(normal.topBaseLine, leftFlank)) continue;
                    left = normal; foundLeft = true; break;
                }
            }
            bool shiftedLeft = false;
            if (!foundLeft && onFlank(local.topBaseLine, leftFlank)) {
                left = local; foundLeft = true; shiftedLeft = true;
            }
            if (!foundLeft) continue;
            bool haveRightSection = true;
            if (rightTurn) right = *rightTurn;
            if (bothSides && !rightTurn && (!medialChordAt(context, tip.x() + rightOffset * stroke,
                               inner.point.y(), 2 * stroke, right, true) || !onFlank(right.topBaseLine, rightFlank))) {
                bool found = false;
                for (const auto& top : hits) {
                    if (!onFlank(top, rightFlank)) continue;
                    const auto below = FindInterections(ink, QLineF(top.point,
                        QPointF(top.point.x(), context.bbox.bottom() + stroke)), true);
                    for (const auto& bottom : below) {
                        if (bottom.point.y() <= top.point.y() || !interiorChord(ink, top, bottom)) continue;
                        right.topBaseLine = top; right.bottomBaseLine = bottom; found = true; break;
                    }
                    if (found) break;
                }
                haveRightSection = found;
            }
            const auto originalLeft = left;
            const double leftOffset = shiftedLeft ? 0 : -settings.value("leftOffset").toDouble(.45);
            bool normalLeft = strokeNormalCut(context, left, leftOffset);
            const double tolerance = settings.value("normalOffsetTolerance").toDouble(0.25);
            for (int step = 1; !normalLeft && step <= 5; ++step) {
                for (double direction : {-1., 1.}) {
                    left = originalLeft;
                    const double offset = leftOffset + direction * tolerance * step / 5;
                    if (offset < 0 && strokeNormalCut(context, left, offset)) { normalLeft = true; break; }
                }
            }
            normalLeft = normalLeft && (bothSides || onFlank(left.topBaseLine, leftFlank)) &&
                retainsTooth(context, left, tip, inner.point);
            const bool remote = bothSides && normalLeft &&
                QLineF(inner.point, left.bottomBaseLine.point).length() > 1.5 * stroke;
            if (!normalLeft || remote) {
                // A projection can reach the far side of a sloping join.
                // Walk the underside from this tooth's bend for a local normal.
                bool walked = false;
                const auto start = context.timeSorted.find(inner);
                for (auto advance : {&SearchContext::next, &SearchContext::prev}) {
                    auto end = adjacent(start, advance);
                    if (end->point.x() >= inner.point.x()) continue;
                    for (int step = 0; step < 4; ++step) {
                        PathExtrema anchor{}; QPointF tangent;
                        if (advanceOnContour(ink, inner, *end, advance == &SearchContext::prev,
                                settings.value("leftOffset").toDouble(.45) * stroke, anchor, tangent)) {
                            Stretching proposal{};
                            if (contourNormalCut(context, anchor, advance == &SearchContext::prev, false, proposal)) {
                                std::swap(proposal.topBaseLine, proposal.bottomBaseLine);
                                if ((bothSides || onFlank(proposal.topBaseLine, leftFlank)) &&
                                    retainsTooth(context, proposal, tip, inner.point)) {
                                    left = proposal;
                                    walked = true;
                                }
                            }
                            break;
                        }
                        const auto next = adjacent(end, advance);
                        if (next->point.x() >= end->point.x()) break;
                        end = next;
                    }
                    if (walked) break;
                }
                normalLeft |= walked;
            }
            if (!normalLeft) {
                inspectReason("Cuts", "No left normal at this offset keeps the tooth on its own flank.");
                continue;
            }
            std::vector<Stretching> rightOptions;
            if (!bothSides) rightOptions.push_back(right);
            else if (rightTurn) rightOptions.push_back(*rightTurn);
            else {
                // An upper probe can project far into the connector. Only
                // keep it if the lower endpoint stays close to this bend.
                const auto addRightProbe = [&](const Stretching& cut) {
                    if (QLineF(inner.point, cut.bottomBaseLine.point).length() <= (rightOffset + 1) * stroke)
                        rightOptions.push_back(cut);
                };
                auto candidate = right;
                if (haveRightSection && strokeNormalCut(context, candidate)) addRightProbe(candidate);
                for (int step = 1; step <= 4; ++step) {
                    if (!medialChordAt(context, tip.x() + (rightOffset + .25 * step) * stroke,
                            inner.point.y(), 3 * stroke, candidate, true) ||
                        !onFlank(candidate.topBaseLine, rightFlank) || !strokeNormalCut(context, candidate) ||
                        !onFlank(candidate.topBaseLine, rightFlank)) continue;
                    addRightProbe(candidate);
                }
                // Walk from the inner bend for a local right normal when the
                // upper probe misses the shoulder or reaches too far.
                const auto start = context.timeSorted.find(inner);
                for (auto advance : {&SearchContext::prev, &SearchContext::next}) {
                    auto end = start;
                    for (size_t step = 0; step + 1 < context.timeSorted.size(); ++step) {
                        const auto next = adjacent(end, advance);
                        if (next->point.x() <= end->point.x()) break;
                        end = next;
                        PathExtrema anchor{}; QPointF tangent;
                        if (!advanceOnContour(ink, inner, *end, advance == &SearchContext::prev,
                                rightOffset * stroke, anchor, tangent)) continue;
                        Stretching normal{};
                        if (contourNormalCut(context, anchor, advance == &SearchContext::prev, false, normal)) {
                            const auto& upper = normal.bottomBaseLine;
                            // A very short upright shoulder can end before
                            // this half-width offset. Accept its adjacent
                            // baseline only within one local stroke of the foot.
                            const auto& foot = rightFlank.back();
                            if (onFlank(upper, rightFlank) ||
                                (upper.point.x() >= foot.point.x() && upper.point.y() < anchor.point.y() &&
                                 QLineF(foot.point, upper.point).length() <= stroke &&
                                 QLineF(anchor.point, upper.point).length() <= 1.75 * stroke)) {
                                std::swap(normal.topBaseLine, normal.bottomBaseLine);
                                rightOptions.push_back(normal);
                            }
                        }
                        break;
                    }
                }
            }
            bool accepted = false;
            for (const auto& proposal : rightOptions) {
                const auto rightTime = getJoinTime(proposal, context.orientation);
                if (bothSides && (!rightTime.isStrictLeft(left.topBaseLine.time) ||
                    !rightTime.isStrictLeft(left.bottomBaseLine.time))) continue;
                const std::vector<Stretching> cuts = bothSides ? std::vector{proposal, left} : std::vector{left};
                const auto pieces = cutRegions(ink, {}, cuts, context.orientation, true);
                const int bodyIndex = bothSides ? 1 : 0;
                if (pieces.size() != cuts.size() + 1 || pieces[bodyIndex].path.boundingRect().isEmpty()) continue;
                const auto& bodyPath = pieces[bodyIndex].path;
                // An open tooth has no counter. A loop crest can satisfy the
                // same tip/bend distances, but belongs to a loop-shaped body.
                if (bothSides && toSubpaths(bodyPath).size() != 1) continue;
                const auto body = bodyPath.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
                if (!body.contains(tip) || !body.contains(inner.point) ||
                    !bodyPath.contains(tip + .05 * (inner.point - tip)) ||
                    !bodyPath.contains(tip + .95 * (inner.point - tip))) continue;
                results.push_back({evidence.tip, inner, left, bothSides ? std::optional{proposal} : std::nullopt});
                accepted = true;
                break;
            }
            if (!accepted) {
                inspectReason("Cuts", "No right/left pair passed contour ordering, single-body topology and core containment.");
                continue;
            }
            break;
        }
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.right && b.right ? Stretching::cmpbyx{}(*a.right, *b.right) : a.tip.point.x() > b.tip.point.x();
    });
    return results;
}

// A compound loop shares the same intrinsic tooth recognizer as a senna.
std::shared_ptr<const LoopTeeth> recognizeLoopTeeth(const InkOutline &outline, const QJsonObject &body) {
    return std::make_shared<const LoopTeeth>(recognizeToothBodies(outline, body));
}

std::vector<ToothBody> cutLoopReturningTeeth(const SearchContext& context, const LoopTeeth& bodies) {
    std::vector<ToothBody> result;
    for (const auto& body : bodies) {
        // A descending return can leave the tooth's outer tip on the lower
        // edge of the connector. The inward bend then points into the return.
        const auto& rightFlank = body.reverseIsRight ? body.reverseFlank : body.forwardFlank;
        const auto foot = std::ranges::max_element(rightFlank, {}, [](const auto& p) { return p.point.y(); });
        for (const auto& pair : body.pairs) {
            if (!pair.localScale || foot->point.y() - pair.bend.point.y() <= pair.localScale->stroke) continue;
            Stretching exit{};
            if (!contourNormalCut(context, body.tip, body.reverseIsRight, false, exit, true)) break;
            std::swap(exit.topBaseLine, exit.bottomBaseLine);
            const auto interval = getJoinTime(exit, context.orientation);
            if (!interval.isStrictRight(foot->time) || !interval.isStrictRight(pair.bend.time) ||
                !retainsTooth(context, exit, body.tip.point, pair.bend.point)) continue;
            result.push_back({body.tip, pair.bend, exit, {}});
            break;
        }
    }
    return result;
}

// A tooth entering a stem attachment can expose a tip on the earlier seam.
// Walk both flanks of that same tip/bend pair to find its incoming shoulder.
std::optional<Stretching> toothShoulderEntry(SearchContext& context, const Stretching& incoming,
                                          const QJsonObject& settings) {
    const auto tip = context.timeSorted.find(incoming.topBaseLine);
    if (tip == context.timeSorted.end() || tip->point.y() >= context.prev(tip)->point.y() ||
        tip->point.y() >= context.next(tip)->point.y()) return {};
    const auto& ink = context.transformedPath;
    const double width = QLineF(tip->point, incoming.bottomBaseLine.point).length();
    const double maxBendDistance = settings.value("maxBendDistance").toDouble();
    const int flankSteps = settings.value("flankSteps").toInt();
    const auto shoulder = [&](auto point, auto advance) {
        for (int step = 0; step < flankSteps; ++step) {
            const auto next = (context.*advance)(point);
            if (next->point.y() <= point->point.y()) break;
            point = next;
        }
        return point;
    };
    for (auto bend = context.timeSorted.begin(); bend != context.timeSorted.end(); ++bend) {
        if (bend->point.y() <= tip->point.y() ||
            bend->point.y() > context.prev(bend)->point.y() ||
            bend->point.y() > context.next(bend)->point.y() ||
            QLineF(tip->point, bend->point).length() > maxBendDistance * width ||
            !interiorChord(ink, *tip, *bend)) continue;
        for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
            const auto reverse = advance == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
            const auto top = shoulder(tip, advance);
            const auto bottom = shoulder(bend, reverse);
            if (top->point.x() <= tip->point.x() || bottom->point.x() <= tip->point.x() ||
                bottom->point.y() <= top->point.y() || !interiorChord(ink, *top, *bottom)) continue;
            auto section = incoming;
            section.topBaseLine = *top;
            section.bottomBaseLine = *bottom;
            return section;
        }
    }
    return {};
}

static void projectBaselineEntries(SearchContext& context, std::vector<ToothBody>& candidates,
                                   const QJsonObject& body, const QJsonObject& cuts) {
    const auto entry = body.value("baselineEntry").toObject();
    const auto policy = cuts.value("baselineEntry").toObject();
    if (!toothPairDescription(body) || entry.isEmpty() || policy.isEmpty()) return;
    for (auto& candidate : candidates) extendToothToBaseline(context, candidate, entry, policy);
}

std::vector<ToothBody> computeToothBoundaries(SearchContext& context, const QJsonObject& form, bool recovering) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto body = form.value("body").toObject();
    const auto cuts = form.value("cuts").toObject();
    const auto evidence = recognizedSharedShape(context, body);
    inspectSharedShape(*evidence);
    std::vector<ToothBody> result;
    const auto count = evidence->toothPairs.size() + evidence->bentTeeth.bodies.size();
    if (cutGeometryMemo && cutGeometryMemo->activeForm) cutGeometryMemo->activeForm->bodies += count;
    if (!count) {
        inspectReason("Body", "No tip–bend pair passed the body constraints.");
    } else if (toothPairDescription(body)) {
        result = cutToothBodies(context, evidence->toothPairs, cuts);
        if (recovering) projectBaselineEntries(context, result, body, cuts);
    } else
        result = cutBentToothBodies(context, evidence->bentTeeth, cuts);
    if (count && result.empty())
        inspectReason("Cuts", "Body evidence found, but no boundary pair passed projection and retained-body checks.");
    if (inspectingForm) for (const auto& item : result) inspectCandidate(QString("Tip %1 / bend %2").arg(item.tip.time, 0, 'f', 2)
        .arg(item.bend.time, 0, 'f', 2), item.left, item.right);
    return result;
}

// Both cuts are projected from one form's cached body. A recovery cut uses
// the same landmarks and policy and leaves the ordinary proposal available.
std::vector<ToothBody> declaredToothBoundaries(SearchContext& context, const QJsonObject& form, bool recovering) {
    CandidateSearchScope measured(form);
    const auto project = [&](bool recovery) {
        if (measured.stats) ++measured.stats->builds;
        auto candidates = computeToothBoundaries(context, form, recovery);
        if (measured.stats) measured.stats->cutPairs += candidates.size();
        return candidates;
    };
    if (!context.outlineCache || formInspection) return project(recovering);
    auto& memo = context.outlineCache->toothCandidates;
    if (!memo || memo->source != context.transformedPath ||
        memo->source.fillRule() != context.transformedPath.fillRule() || memo->orientation != context.orientation)
        memo = std::make_shared<ToothCandidateMemo>(context.transformedPath, context.orientation);
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto id = form.value("id").toString();
    auto saved = memo->forms.find(id);
    if (saved == memo->forms.end() || saved->body != body || saved->cuts != cuts)
        saved = memo->forms.insert(id, {body, cuts, project(false), {}});
    if (!recovering || !toothPairDescription(body) || cuts.value("baselineEntry").toObject().isEmpty())
        return saved->candidates;
    if (!saved->recovered) {
        saved->recovered = saved->candidates;
        projectBaselineEntries(context, *saved->recovered, body, cuts);
    }
    return *saved->recovered;
}

} // namespace MedinaCutter
