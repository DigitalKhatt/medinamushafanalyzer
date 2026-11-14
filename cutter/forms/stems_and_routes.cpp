#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

std::optional<UprightStemBody> recognizeUprightStem(const InkOutline &outline) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    const auto start = std::prev(context.xSorted.end());
    const auto& ink = context.transformedPath;
    const auto box = ink.boundingRect();
    const double y = start->point.y() + box.height() * 0.25;
    const auto crossSection = [&](double probeY) {
        auto hits = FindInterections(ink, QLineF(box.left() - 1, probeY, box.right() + 1, probeY));
        std::sort(hits.begin(), hits.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.point.x() < rhs.point.x();
        });
        return hits;
    };
    const auto stem = crossSection(y);
    if (stem.size() < 2 || stem[0].pathIndex != 0 || stem[1].pathIndex != 0 || !interiorChord(ink, stem[0], stem[1]))
        return {};
    const double stroke = stem[1].point.x() - stem[0].point.x();
    if (stroke <= 0 || box.height() < stroke * 5)
        return {};

    UprightStemBody body{*start, y, stroke, {}};
    for (const auto &point : outline.time)
        if (point.point.y() >= y && point.point.x() > start->point.x())
            body.turns.push_back(point);
    return body;
}

bool cutUprightStem(const SearchContext &context, const UprightStemBody &body, const QJsonObject &form,
                           Stretching &result) {
    const auto &ink = context.transformedPath;
    const auto box = context.bbox;
    const double y = body.y, stroke = body.stroke;
    const auto crossSection = [&](double probeY) {
        auto hits = FindInterections(ink, QLineF(box.left() - 1, probeY, box.right() + 1, probeY));
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.point.x() < b.point.x(); });
        return hits;
    };
    std::optional<Stretching> best;
    for (const auto &limit : body.turns) {
        Stretching cut{};
        if (!alefTurnCut(context, limit, form.value("turnAngle").toDouble(45), stroke, cut)) continue;
        const QLineF chord(cut.topBaseLine.point, cut.bottomBaseLine.point);
        if (chord.center().x() - box.left() > 4 * stroke || chord.center().y() < y ||
            chord.length() < 0.5 * stroke || chord.length() > 2.5 * stroke) continue;
        if (!best || cut.bottomBaseLine.point.x() < best->bottomBaseLine.point.x()) best = cut;
    }
    if (!best && form.value("stemWidthLimit").isDouble()) {
        // Straight stems can meet a foot without an eligible diagonal normal.
        // Use the last narrow cross-section before the left stem widens.
        const double step = stroke / 5;
        const double maxWidth = stroke * form.value("stemWidthLimit").toDouble();
        std::optional<Stretching> narrow;
        for (double probeY = y; probeY < box.bottom(); probeY += step) {
            const auto hits = crossSection(probeY);
            if (hits.size() < 2) break;
            const auto& left = hits[0];
            const auto& right = hits[1];
            const double width = right.point.x() - left.point.x();
            if (width > maxWidth) {
                if (narrow && probeY - y >= stroke * 3) best = narrow;
                break;
            }
            if (left.point.x() - box.left() > 4 * stroke || width < 0.5 * stroke ||
                !interiorChord(ink, right, left)) break;
            narrow = Stretching{};
            narrow->topBaseLine = right;
            narrow->bottomBaseLine = left;
        }
    }
    if (!best) return false;
    result = *best;
    result.topLeft = result.topRight = result.topBaseLine;
    result.bottomLeft = result.bottomRight = result.bottomBaseLine;
    return true;
}

std::optional<Stretching> uprightStemFoot(const SearchContext& context, const Stretching& entry, double penWidth) {
    const QLineF cap(entry.topBaseLine.point, entry.bottomBaseLine.point);
    const double width = cap.length();
    if (width < .25 * penWidth || width > .83 * penWidth || std::abs(cap.dy()) > .05 * width) return {};
    auto foot = strokeFlankTurn(context, entry.bottomBaseLine, width, 1, true, 43, 2.6);
    if (!foot) return {};
    std::swap(foot->topBaseLine, foot->bottomBaseLine);
    if (!cutPrecedes(*foot, entry, context.orientation)) return {};
    return foot;
}

std::optional<Stretching> descenderEntry(const SearchContext& context, const Stretching& entry, double penWidth) {
    const QLineF cap(entry.topBaseLine.point, entry.bottomBaseLine.point);
    const double width = cap.length();
    if (width < .25 * penWidth || width > 2.7 * penWidth ||
        std::abs(cap.dy()) < .4 * width || std::abs(cap.dy()) > .91 * width) return {};
    auto turn = strokeFlankTurn(context, entry.topBaseLine, width, 1, true, 13, 1.1);
    if (!turn || !cutPrecedes(*turn, entry, context.orientation)) return {};
    const QLineF section(turn->topBaseLine.point, turn->bottomBaseLine.point);
    return section.center().x() - cap.center().x() >= 1.6 * width ? turn : std::nullopt;
}

std::optional<PathExtrema> horizontalOpposite(const SearchContext& context,
                                                     const PathExtrema& anchor, bool left) {
    const double edge = left ? context.bbox.left() - 1 : context.bbox.right() + 1;
    const auto hits = FindInterections(context.transformedPath,
        QLineF(anchor.point, QPointF(edge, anchor.point.y())), true);
    std::optional<PathExtrema> opposite;
    double nearest = context.bbox.width() + 1;
    for (const auto& hit : hits) {
        const double distance = (left ? -1 : 1) * (hit.point.x() - anchor.point.x());
        if (distance <= 1e-5 || distance >= nearest || !interiorChord(context.transformedPath, anchor, hit)) continue;
        opposite = hit;
        nearest = distance;
    }
    return opposite;
}

std::vector<PairedRouteBody> recognizePairedRoutes(const InkOutline &outline, const QJsonObject &settings) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    std::vector<PairedRouteBody> bodies;
    const auto feature = settings.value("anchor").toObject();
    const auto flanks = feature.value("flanks").toArray();
    for (auto anchor = context.timeSorted.begin(); anchor != context.timeSorted.end(); ++anchor) {
        if ((feature.contains("sharp") && feature.value("sharp").toBool() != anchor->isSharp) ||
            (feature.contains("xExtremum") && feature.value("xExtremum").toBool() != anchor->isX) ||
            (feature.contains("yExtremum") && feature.value("yExtremum").toBool() != anchor->isY)) continue;
        if (feature.contains("convex")) {
            const auto incoming = anchor->point - context.prev(anchor)->point;
            const auto outgoing = context.next(anchor)->point - anchor->point;
            const double turn = incoming.x() * outgoing.y() - incoming.y() * outgoing.x();
            const bool convex = context.orientation == Orientation::ClockWise ? turn < 0 : turn > 0;
            if (std::abs(turn) < 1e-10 || convex != feature.value("convex").toBool()) continue;
        }
        std::vector<double> retainedBranchTimes;
        if (flanks.size() == 2) {
            int first = 0;
            auto a = contourFlank(context, anchor, true, flanks[0].toObject());
            auto b = contourFlank(context, anchor, false, flanks[1].toObject());
            if (!a || !b) {
                first = 1;
                a = contourFlank(context, anchor, true, flanks[1].toObject());
                b = contourFlank(context, anchor, false, flanks[0].toObject());
            }
            if (!a || !b) continue;
            if (flanks[first].toObject().value("insideBody").toBool()) retainedBranchTimes.push_back(a->time);
            if (flanks[1-first].toObject().value("insideBody").toBool()) retainedBranchTimes.push_back(b->time);
        }
        // Either boundary may start from a second intrinsic feature. Resolve
        // it relative to this body's anchor, never a neighboring letter's cut.
        const auto secondaryAnchor = [&](const QJsonObject& secondary) {
            if (secondary.isEmpty()) return anchor;
            auto selected = context.timeSorted.end();
            double nearest = std::hypot(context.bbox.width(), context.bbox.height());
            bool found = false;
            for (auto point = context.timeSorted.begin(); point != context.timeSorted.end(); ++point) {
                if ((secondary.contains("sharp") && secondary.value("sharp").toBool() != point->isSharp) ||
                    (secondary.contains("xExtremum") && secondary.value("xExtremum").toBool() != point->isX) ||
                    (secondary.contains("yExtremum") && secondary.value("yExtremum").toBool() != point->isY) ||
                    !matchesOffset(secondary.value("offset").toObject(), point->point - anchor->point)) continue;
                const auto branches = secondary.value("flanks").toObject();
                if (!matchesOffset(branches, context.prev(point)->point - point->point) ||
                    !matchesOffset(branches, context.next(point)->point - point->point)) continue;
                const double distance = QLineF(anchor->point, point->point).length();
                if (distance < nearest) {
                    nearest = distance;
                    selected = point;
                    found = true;
                }
            }
            return found ? selected : context.timeSorted.end();
        };
        const auto rightSettings = settings.value("right").toObject();
        const auto leftSettings = settings.value("left").toObject();
        const auto rightAnchor = secondaryAnchor(rightSettings.value("anchor").toObject());
        const auto leftAnchor = secondaryAnchor(leftSettings.value("anchor").toObject());
        if (rightAnchor == context.timeSorted.end() || leftAnchor == context.timeSorted.end()) continue;
        bool hasFeatures = true;
        for (const auto& feature : settings.value("retain").toArray()) {
            const auto point = secondaryAnchor(feature.toObject());
            if (point == context.timeSorted.end()) { hasFeatures = false; break; }
            retainedBranchTimes.push_back(point->time);
        }
        if (!hasFeatures) continue;
        const auto left = recognizeSavedRoute(context, leftSettings, &*leftAnchor);
        for (bool reverse : {false, true}) {
            if (reverse && !rightSettings.value("probeBothSides").toBool())
                break;
            const auto right = recognizeSavedRoute(context, rightSettings, &*rightAnchor, reverse);
            if (right)
                bodies.push_back({*anchor, retainedBranchTimes, right, left});
        }
    }
    return bodies;
}

std::vector<std::pair<Stretching, Stretching>> cutPairedRoutes(SearchContext &context,
                                                                      const std::vector<PairedRouteBody> &bodies,
                                                                      const QJsonObject &body, const QJsonObject &cuts,
                                                                      const Stretching *sharedExit) {
    std::vector<std::pair<Stretching, Stretching>> candidates;
    const auto originalForm = context.form;
    const auto originalResult = context.res;
    const auto originalNext = context.nextIter;
    const auto rightSettings = mergeShapeSettings(body.value("right").toObject(), cuts.value("right").toObject());
    const auto leftSettings = mergeShapeSettings(body.value("left").toObject(), cuts.value("left").toObject());
    for (const auto &evidence : bodies) {
        const auto *anchor = &evidence.anchor;
        const auto &retainedBranchTimes = evidence.retained;
        Stretching right{}, left{};
        context.form = rightSettings; context.res = {};
        if (!projectSavedRoute(context, evidence.right, body.value("right").toObject(), cuts.value("right").toObject()))
            continue;
        right = context.res;
        context.form = leftSettings;
        context.res = {};
        if (sharedExit) left = *sharedExit;
        else {
            if (!projectSavedRoute(context, evidence.left, body.value("left").toObject(),
                                   cuts.value("left").toObject()))
                continue;
            left = context.res;
        }
        if (cuts.value("orientToBody").toBool()) {
            // "Upper" and "lower" exchange sides on a folded stroke. The
            // retained body feature determines the contour side of each cut.
            if (!getJoinTime(right, context.orientation).isLeft(anchor->time))
                std::swap(right.topBaseLine, right.bottomBaseLine);
            if (!getJoinTime(left, context.orientation).isRight(anchor->time))
                std::swap(left.topBaseLine, left.bottomBaseLine);
        }
        const auto leftOfRight = getJoinTime(right, context.orientation);
        if (!leftOfRight.isStrictLeft(left.topBaseLine.time) || !leftOfRight.isStrictLeft(left.bottomBaseLine.time)) continue;
        // A route must retain its defining landmark in the actual contour
        // interval. Bounding-box containment alone can accept a wrapped cut
        // that leaves this head on the other side of the boundary.
        if (!getJoinTime(right, context.orientation).isLeft(anchor->time) ||
            !getJoinTime(left, context.orientation).isRight(anchor->time)) continue;
        // A defining branch belongs to this body, too. Keeping just its elbow
        // can otherwise accept cuts that assign the entire rising arm elsewhere.
        if (std::any_of(retainedBranchTimes.begin(), retainedBranchTimes.end(), [&](double time) {
            return !getJoinTime(right, context.orientation).isLeft(time) ||
                !getJoinTime(left, context.orientation).isRight(time);
        })) continue;
        const auto pieces = cutRegions(context.transformedPath, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3 ||
            !pieces[1].path.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(anchor->point)) continue;
        const bool duplicate = std::any_of(candidates.begin(), candidates.end(), [&](const auto& pair) {
            return QLineF(pair.first.topBaseLine.point, right.topBaseLine.point).length() < 1e-5 &&
                QLineF(pair.second.topBaseLine.point, left.topBaseLine.point).length() < 1e-5;
        });
        if (!duplicate) candidates.emplace_back(right, left);
    }
    context.form = originalForm;
    context.res = originalResult;
    context.nextIter = originalNext;
    return candidates;
}

// Adapter for not-yet-migrated contour declarations and their direct geometry tests.
std::vector<std::pair<Stretching, Stretching>>
contourBodyBoundaries(SearchContext &context, const QJsonObject &settings, const Stretching *sharedExit) {
    return cutPairedRoutes(context, recognizePairedRoutes(inkOutline(context), settings), settings, settings,
                           sharedExit);
}

std::vector<TerminalStemBody> recognizeTerminalStemBodies(const InkOutline &outline, const QJsonObject &body) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    // The ascender is on the right of the final bowl. Its shorter upturned
    // left end is also a narrow stroke, but is not the letter's incoming stem.
    const auto bowlEdge = context.timeSorted.find(*std::prev(context.xSorted.end()));
    std::optional<QPointF> bowlBottom;
    for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
        auto bottom = bowlEdge;
        for (size_t step = 0; step < context.timeSorted.size(); ++step) {
            const auto next = (context.*advance)(bottom);
            if (next->point.y() <= bottom->point.y()) break;
            bottom = next;
        }
        if (bottom != bowlEdge && (!bowlBottom || bottom->point.y() > bowlBottom->y()))
            bowlBottom = bottom->point;
    }
    if (!bowlBottom) return {};
    std::vector<TerminalStemBody> bodies;
    for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest) {
        if (crest->point.x() <= bowlBottom->x() ||
            crest->point.y() >= context.prev(crest)->point.y() ||
            crest->point.y() >= context.next(crest)->point.y()) continue;
        for (auto advance : {&SearchContext::next, &SearchContext::prev}) {
            auto limit = crest;
            for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                auto next = (context.*advance)(limit);
                if (next->point.y() <= limit->point.y()) break;
                limit = next;
            }
            const double rise = limit->point.y() - crest->point.y();
            if (rise <= 0) continue;
            const double y = crest->point.y() + rise * 0.25;
            const auto hits = FindInterections(context.transformedPath,
                QLineF(context.bbox.left() - 1, y, context.bbox.right() + 1, y), true);
            double stroke = 0, nearest = rise;
            QPointF stemSample;
            for (size_t i = 1; i < hits.size(); ++i) {
                if (!interiorChord(context.transformedPath, hits[i - 1], hits[i])) continue;
                const QLineF chord(hits[i - 1].point, hits[i].point);
                const double distance = std::abs(chord.center().x() - crest->point.x());
                if (distance < nearest) {
                    nearest = distance;
                    stroke = chord.length();
                    stemSample = chord.center();
                }
            }
            if (stroke <= 0 || rise < body.value("minRise").toDouble(3) * stroke || nearest > stroke) continue;
            bodies.push_back(
                {*crest, *limit, *bowlEdge, *bowlBottom, stemSample, stroke, advance == &SearchContext::prev ? 1 : -1});
        }
    }
    return bodies;
}

bool cutTerminalStemBodies(SearchContext& context, const std::vector<TerminalStemBody>& bodies,
        const QJsonObject& body, const QJsonObject& policy) {
    std::optional<Stretching> best;
    double leftmost = context.bbox.right() + 1;
    for (const auto& evidence : bodies) {
        const auto& [crest, limit, bowlEdge, bowlBottom, stemSample, stroke, traversal] = evidence;
        if (crest.point.x() >= leftmost)
            continue;
        Stretching cut{};
        if (!alefTurnCut(context, limit, policy.value("turnAngle").toDouble(45), stroke, cut, &crest, traversal))
            continue;
        std::vector<Stretching> options{cut};
        // An incoming stroke may attach above the bowl. Its inward
        // corner distinguishes that neck from a chord through the stem
        // itself, which would assign the whole bowl to the kashida.
        const auto foot = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).center();
        for (auto corner = context.timeSorted.begin(); corner != context.timeSorted.end(); ++corner) {
            if (!corner->isSharp || corner->point.x() <= crest.point.x() ||
                corner->point.y() >= context.prev(corner)->point.y() ||
                corner->point.y() >= context.next(corner)->point.y() ||
                QLineF(foot, corner->point).length() > body.value("maxNeckDistance").toDouble(2) * stroke)
                continue;
            const bool increasing = context.prev(corner)->point.x() > context.next(corner)->point.x();
            Stretching neck{};
            if (contourNormalCut(context, *corner, increasing, false, neck, true)) {
                std::swap(neck.topBaseLine, neck.bottomBaseLine);
                options.push_back(neck);
            }
        }
        std::optional<Stretching> accepted;
        for (const auto& option : options) {
            const auto pieces = cutRegions(context.transformedPath, {}, {option}, context.orientation, true);
            if (pieces.size() != 2) continue;
            const auto& body = pieces[1].path;
            const auto bounds = body.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
            if (!bounds.contains(crest.point) || !bounds.contains(bowlEdge.point) || !body.contains(stemSample) ||
                !body.contains(bowlBottom - QPointF(0, .01 * stroke)))
                continue;
            accepted = option;
            break;
        }
        if (!accepted) continue;
        best = *accepted;
        leftmost = crest.point.x();
    }
    if (!best) return false;
    context.res = *best;
    context.res.topLeft = context.res.topRight = best->topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = best->bottomBaseLine;
    return true;
}

// Walk the lower side of the outline and select a cross-section near its landmark.
bool medialBodyChord(SearchContext& context, const PathExtrema& start,
                            const QJsonObject& boundary, Stretching& cut) {
    const auto steps = boundary.value("walk").toArray();
    if (steps.isEmpty()) return false;
    auto current = context.timeSorted.find(start);
    const auto advance = context.orientation == Orientation::ClockWise ? &SearchContext::prev : &SearchContext::next;
    auto landmark = current;
    int remaining = static_cast<int>(context.timeSorted.size());
    for (const auto& step : steps) {
        auto bounds = step.toObject();
        if (step.isString()) bounds.insert("direction", step);
        bool found = false;
        while (remaining-- > 0) {
            const auto next = (context.*advance)(current);
            if (matchesOffset(bounds, next->point - current->point)) {
                const auto maxStepEnd = boundary.value("maxStepEndDistance");
                const bool useEnd = boundary.value("useStepEnd").toBool() &&
                    (!maxStepEnd.isDouble() || QLineF(current->point, next->point).length() <= maxStepEnd.toDouble());
                landmark = useEnd ? next : current;
                current = next;
                found = true;
                break;
            }
            current = next;
        }
        if (!found) return false;
    }

    const double x = landmark->point.x() + boundary.value("offsetX").toDouble();
    return medialChordAt(context, x, landmark->point.y(), 1, cut, true);
}

std::optional<DescendingShaftBody> recognizeDescendingShaft(const InkOutline &outline) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    const auto& ink = context.transformedPath;
    const auto terminal = std::prev(context.xSorted.end());
    const auto box = context.bbox;
    const double y = terminal->point.y() + .45 * (box.bottom() - terminal->point.y());
    auto shaft = FindInterections(ink, QLineF(box.left() - 1, y, box.right() + 1, y), true);
    std::sort(shaft.begin(), shaft.end(), [](const auto& a, const auto& b) { return a.point.x() < b.point.x(); });
    if (shaft.size() < 2 || !interiorChord(ink, shaft[0], shaft[1]))
        return {};
    const QLineF section(shaft[0].point, shaft[1].point);
    const double width = section.length();
    if (width <= 0 || y - terminal->point.y() < 2 * width || section.center().x() - box.left() > 3 * width)
        return {};
    DescendingShaftBody body{*terminal, section, y, width, {}, {}};
    const int count = static_cast<int>(baseContourCurves(ink).size());
    for (int i : {1, 0})
        for (bool forward : {true, false}) {
            const auto& start = shaft[i];
            PathExtrema end = start, next{};
            end.time += forward ? count : -count;
            QPointF tangent;
            if (!advanceOnContour(ink, start, end, forward, .5 * width, next, tangent) || next.point.y() <= y)
                continue;
            body.flanks.push_back({start, forward});
        }
    for (const auto &foot : outline.time)
        if (foot.point.y() > y + width && foot.point.x() > section.p1().x())
            body.feet.push_back(foot);
    return body;
}

bool cutDescendingShaft(SearchContext &context, const DescendingShaftBody &body, const QJsonObject &form,
                               Stretching &result) {
    const auto &ink = context.transformedPath;
    const auto *terminal = &body.tip;
    const auto &section = body.section;
    const double y = body.y, width = body.width;
    for (const auto &flank : body.flanks) {
        const auto &start = flank.start;
        const bool forward = flank.forward;
        for (const auto &foot : body.feet) {
            Stretching cut{};
            if (!alefTurnCut(context, foot, form.value("turnAngle").toDouble(45), width, cut, &start, forward ? 1 : -1))
                continue;
            const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (line.length() < .55 * width || line.length() > 2.8 * width || line.center().y() < y + width ||
                line.center().x() > section.center().x() + 4 * width)
                continue;
            if (getJoinTime(cut, context.orientation).isStrictRight(terminal->time))
                std::swap(cut.topBaseLine, cut.bottomBaseLine);
            const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
            if (pieces.size() != 2 || !pieces[1].path.contains(section.center()) ||
                toSubpaths(pieces[1].path).size() != 1)
                continue;
            cut.referenceWidth = width;
            result = cut;
            return true;
        }
    }
    return false;
}

TailBody recognizeTailBranches(const InkOutline &outline, const QJsonObject &settings) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    TailBody body{*std::prev(context.xSorted.end()), static_cast<int>(baseContourCurves(outline.ink).size()), {}};
    const auto& ink = context.transformedPath;
    const auto tail = context.timeSorted.find(*std::prev(context.xSorted.end()));
    const auto collect = [&](double backtrackWidths) {
        for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
            auto corner = tail;
            double rightmost = tail->point.x(), lastWidth = 0;
            const bool forward = advance == &SearchContext::prev;
            for (size_t i = 1; i < context.timeSorted.size(); ++i) {
                const auto next = (context.*advance)(corner);
                const auto step = next->point - corner->point;
                if (!backtrackWidths && step.x() < -.01) break;
                Stretching first{};
                const bool landmark = (corner->isSharp || corner->isX || corner->isY) && i > 1;
                const bool normal = (landmark || backtrackWidths) &&
                    contourNormalCut(context, *corner, forward, false, first, true);
                if (backtrackWidths) {
                    rightmost = std::max(rightmost, corner->point.x());
                    if (normal) lastWidth = first.referenceWidth;
                    if (rightmost - next->point.x() > std::max(.01, backtrackWidths * lastWidth)) break;
                }
                if (landmark && normal) {
                    const double width = first.referenceWidth;
                    // A microscopic reverse bend can insert a smooth extremum
                    // immediately after the shoulder. Keep the shoulder
                    // as the anchor; judge each flank after a meaningful walk.
                    const double distance = settings.value("flankSampleWidths").toDouble(.25) * width;
                    const auto sample = [&](bool direction, double length, PathExtrema& point, QPointF& tangent) {
                        auto limit = *corner;
                        limit.time += (direction ? 1 : -1) * body.curveCount;
                        return advanceOnContour(ink, *corner, limit, direction, length, point, tangent);
                    };
                    PathExtrema incoming{}, outgoing{};
                    QPointF tangent;
                    const double run = corner->point.x() - tail->point.x();
                    const bool recovered = backtrackWidths > 0;
                    // The outgoing flank can already be turning right while
                    // its position is still slightly left of the shoulder.
                    if (sample(!forward, distance, incoming, tangent) &&
                        sample(forward, recovered ? std::max(distance, .5 * width) : distance, outgoing, tangent) &&
                        incoming.point.y() > corner->point.y() && outgoing.point.y() > corner->point.y() &&
                        (recovered ? (forward ? tangent.x() : -tangent.x()) > 0 :
                                     outgoing.point.x() > corner->point.x()) &&
                        run >= settings.value("minRun").toDouble(2) * width && run <= settings.value("maxRun").toDouble(12) * width) {
                        body.branches.push_back({*corner, forward, width, recovered});
                    }
                }
                corner = next;
            }
        }
    };
    collect(0);
    // A slight hook can turn left before the contour joins the incoming
    // stroke. Only widen the landmark search when the ordinary walk found
    // no shoulder; keep the same normal cut and ownership checks.
    const double backtrack = settings.value("maxShoulderBacktrackWidths").toDouble();
    if (body.branches.empty() && backtrack > 0)
        collect(backtrack);
    return body;
}

static bool cutNormalFromShoulder(SearchContext& context, const TailBranch& branch,
        const PathExtrema& limit, double distance, Stretching& cut) {
    const auto& corner = branch.corner;
    PathExtrema lower = corner;
    if (distance > 0) {
        QPointF tangent;
        if (!advanceOnContour(context.transformedPath, corner, limit, branch.forward, distance, lower, tangent) ||
            lower.point.y() <= corner.point.y() ||
            (branch.recovered ? (branch.forward ? tangent.x() : -tangent.x()) <= 0 :
                                lower.point.x() <= corner.point.x()))
            return false;
    }
    if (!contourNormalCut(context, lower, branch.forward, false, cut, true)) return false;
    std::swap(cut.topBaseLine, cut.bottomBaseLine);
    // At a tight reverse bend, use the direction of both stroke edges.
    return !branch.recovered || distance == 0 || strokeNormalCut(context, cut);
}

static bool retainsTailBody(const SearchContext& context, const PathExtrema& tip, const TailBranch& branch,
        const Stretching& cut, double minRise, bool atShoulder = false) {
    const auto side = getJoinTime(cut, context.orientation);
    if (!side.isStrictLeft(tip.time) ||
        !(atShoulder ? side.isLeft(branch.corner.time) : side.isStrictLeft(branch.corner.time))) return false;
    const auto pieces = cutRegions(context.transformedPath, {}, {cut}, context.orientation, true);
    return pieces.size() == 2 && toSubpaths(pieces[1].path).size() == 1 &&
        branch.corner.point.y() - pieces[1].path.boundingRect().top() >= minRise * branch.width;
}

bool cutTailBranches(SearchContext &context, const TailBody &body, const QJsonObject &settings,
                            Stretching &result, PathExtrema* shoulder) {
    for (const auto &branch : body.branches) {
        const auto *corner = &branch.corner;
        const bool forward = branch.forward;
        const double width = branch.width;
        PathExtrema limit = *corner;
        limit.time += forward ? body.curveCount : -body.curveCount;
        Stretching cut{};
        bool found;
        if (settings.contains("turnAngle")) {
            // From the bowl fork, follow the inner edge down to the foot of the shaft.
            found = alefTurnCut(context, limit, settings.value("turnAngle").toDouble(), width, cut, &*corner,
                                forward ? 1 : -1, true);
            if (found && !getJoinTime(cut, context.orientation).isStrictLeft(body.tip.time))
                std::swap(cut.topBaseLine, cut.bottomBaseLine);
            found = found && cut.topBaseLine.point.y() > corner->point.y() &&
                    cut.bottomBaseLine.point.x() > corner->point.x();
        } else {
            const double walkWidth = settings.value("strokeWidth").toDouble(width);
            const double walkDistance = settings.value("walkWidths").toDouble(1) * walkWidth;
            found = cutNormalFromShoulder(context, branch, limit, walkDistance, cut);
        }
        if (found && retainsTailBody(context, body.tip, branch, cut, settings.value("minRise").toDouble(2))) {
            result = cut;
            if (shoulder) *shoulder = branch.corner;
            return true;
        }
    }
    return false;
}

// A flank can cross the start of a closed SVG contour or contain tiny bends.
// End the range where the connected descent first turns back up or left.
static double descendingFlankLength(const SearchContext& context, const TailBranch& branch,
        const std::vector<QBezier>& curves, double tolerance) {
    const auto start = context.timeSorted.find(branch.corner);
    if (start == context.timeSorted.end() || curves.empty()) return 0;
    const auto step = branch.forward ? &SearchContext::prev : &SearchContext::next;
    auto end = start;
    QPointF furthest = start->point;
    for (auto next = (context.*step)(end); next != start; next = (context.*step)(end)) {
        if (next->point.x() < furthest.x() - tolerance || next->point.y() < furthest.y() - tolerance) break;
        furthest.setX(std::max(furthest.x(), next->point.x()));
        furthest.setY(std::max(furthest.y(), next->point.y()));
        end = next;
    }
    if (end == start) return 0;
    const int count = static_cast<int>(curves.size());
    double finish = end->time;
    if (branch.forward && finish < start->time) finish += count;
    if (!branch.forward && finish > start->time) finish -= count;
    const double low = std::min(start->time, finish), high = std::max(start->time, finish);
    double length = 0;
    for (int i = int(std::floor(low)); i < int(std::ceil(high)); ++i) {
        const auto& curve = curves[(i % count + count) % count];
        length += curve.getSubRange(std::max(0., low - i), std::min(1., high - i)).length(.001);
    }
    return length;
}

std::vector<std::optional<Stretching>> contourShoulderCuts(SearchContext& context, const TailBody& body,
        const QJsonObject& shoulder, const QJsonObject& range) {
    const auto curves = baseContourCurves(context.transformedPath);
    const double minimum = range.value("minWidths").toDouble();
    const double preferred = range.value("preferredWidths").toDouble();
    const double minRise = shoulder.value("minRise").toDouble(2);
    struct Candidate { double error; Stretching cut; };
    std::vector<Candidate> candidates;
    for (const auto& branch : body.branches) {
        const double tolerance = shoulder.value("flankSampleWidths").toDouble(.25) * branch.width;
        const double flankEnd = descendingFlankLength(context, branch, curves, tolerance) / branch.width;
        const double maximum = std::min(flankEnd, range.value("maxWidths").toDouble(flankEnd));
        if (maximum < minimum || flankEnd <= 0) continue;

        const double target = std::clamp(preferred, minimum, maximum);
        std::vector<double> offsets{target};
        // Sample each side of the target, including the bounds. Keep the search
        // finite even when a connecting stroke makes the flank unusually long.
        for (double bound : {minimum, maximum})
            for (int i = 1; i <= 4; ++i)
                if (bound != target) offsets.push_back(std::lerp(target, bound, i / 4.));

        auto limit = branch.corner;
        limit.time += branch.forward ? body.curveCount : -body.curveCount;
        for (double offset : offsets) {
            Stretching cut{};
            if (cutNormalFromShoulder(context, branch, limit, offset * branch.width, cut) &&
                retainsTailBody(context, body.tip, branch, cut, minRise, offset == 0))
                candidates.push_back({std::abs(offset - preferred), cut});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.error < b.error;
    });
    // Slot zero is the preferred shoulder placement. If unavailable, the
    // ordinary route remains eligible before trying the sampled offsets.
    std::vector<std::optional<Stretching>> result(1);
    for (const auto& candidate : candidates) {
        if (!result.front() && candidate.error < 1e-9) {
            result.front() = candidate.cut;
            continue;
        }
        const auto alreadyTried = std::any_of(result.begin(), result.end(), [&](const auto& cut) {
            return cut && cut->topBaseLine.time == candidate.cut.topBaseLine.time &&
                          cut->bottomBaseLine.time == candidate.cut.bottomBaseLine.time;
        });
        if (!alreadyTried) result.emplace_back(candidate.cut);
    }
    return result;
}

static std::vector<StemFork> recognizeStemForks(const SearchContext& context, double flankSpan) {
    std::vector<StemFork> bodies;
    for (auto tip = context.timeSorted.begin(); tip != context.timeSorted.end(); ++tip) {
        const auto flank = [&](bool previous) {
            auto at = previous ? context.prev(tip) : context.next(tip);
            const bool tiny = QLineF(at->point, tip->point).length() < flankSpan;
            for (size_t n = 0; n + 1 < context.timeSorted.size() &&
                    QLineF(at->point, tip->point).length() < flankSpan; ++n)
                at = previous ? context.prev(at) : context.next(at);
            if (tiny && at->point.y() <= tip->point.y()) {
                // Look past short contour pieces, but not past the descending
                // flank itself. A distant landmark can already be rising into
                // another letter; sample the local curve in that case.
                auto limit = *tip;
                limit.time += (previous ? 1 : -1) * int(baseContourCurves(context.transformedPath).size());
                PathExtrema sample;
                QPointF tangent;
                if (advanceOnContour(context.transformedPath, *tip, limit, previous, flankSpan, sample, tangent))
                    return sample.point - tip->point;
            }
            return at->point - tip->point;
        };
        // Short contour pieces and duplicate extrema can subdivide a flank.
        // Recognize the two branches across that noise, keeping the actual tip.
        const auto a = flank(true), b = flank(false);
        if (a.y() <= 0 || b.y() <= 0) continue;
        // The fork is a concave turn of the outer contour, not the convex
        // top of another stroke. Use winding instead of a fragile ink probe
        // just beside a nearly tangent Bezier corner.
        const double turn = a.x() * b.y() - a.y() * b.x();
        if (std::abs(turn) <= 1e-8) continue;
        const bool fork = a.x() * b.x() < 0;
        // Keep the intrinsic turn sign. A recovered/scaled outline can resolve
        // a degenerate winding estimate differently; that must not require a
        // second landmark search or discard the real fork from the inventory.
        bodies.push_back({*tip, a, b, a.x() > b.x(), fork, turn > 0});
    }
    return bodies;
}

// Cache intrinsic body evidence independently of route and cut-policy alternatives.
StemForkEvidence recognizeStemForkEvidence(const SearchContext& context, double flankSpan, bool allowLevelCrest) {
    StemForkEvidence result;
    result.forks = recognizeStemForks(context, flankSpan);
    for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest) {
        const double y = crest->point.y();
        if (y >= context.prev(crest)->point.y()) continue;
        auto edge = context.next(crest);
        // A level nib can have several contour nodes at its top. Keep
        // its first node if the outline descends on both sides of the cap.
        while (allowLevelCrest && edge != crest && edge->point.y() == y) edge = context.next(edge);
        if (y < edge->point.y()) result.crests.push_back(*crest);
    }
    return result;
}

// The lower contour of a forked stem rises to a tip under the shaft. Follow
// each flank away from that tip and cut across the same strip of ink.
std::optional<Stretching> cutForkFlank(const SearchContext& context, const PathExtrema& tip,
                                               std::optional<PathExtrema> end, bool forward, int side, double distance) {
    auto anchor = tip;
    if (distance > 0) {
        auto limit = end.value_or(tip);
        if (end && limit.time == tip.time) return {};
        if (!end || forward != (limit.time > tip.time))
            limit.time += (forward ? 1 : -1) * int(baseContourCurves(context.transformedPath).size());
        QPointF tangent;
        if (!advanceOnContour(context.transformedPath, tip, limit, forward, distance, anchor, tangent)) return {};
    }
    Stretching cut{};
    if (!contourNormalCut(context, anchor, forward, false, cut, distance == 0)) return {};
    std::swap(cut.topBaseLine, cut.bottomBaseLine);
    if (distance == 0 && side * (cut.topBaseLine.point.x() - tip.point.x()) <= 0) return {};
    return cut;
}

bool forkAnchorWithinWalk(const SearchContext& context, const PathExtrema& tip,
                               const Stretching& cut, bool forward, double distance) {
    auto end = cut.bottomBaseLine;
    if (end.pathIndex != 0 || QLineF(tip.point, end.point).length() > distance + 1e-5) return false;
    if (QLineF(tip.point, end.point).length() < 1e-5) return true;
    const double count = baseContourCurves(context.transformedPath).size();
    if (count <= 0) return false;
    if (forward != (end.time > tip.time)) end.time += (forward ? 1 : -1) * count;
    PathExtrema point;
    QPointF tangent;
    return !advanceOnContour(context.transformedPath, tip, end, forward, distance + 1e-5, point, tangent);
}

// Follow only the adjacent lower flank of the same fork. Its intersection at
// the exit's height fixes the lower endpoint; the local tangent fixes the cut.
std::optional<Stretching> levelForkEntry(SearchContext& context, const PathExtrema& corner,
                                               const Stretching& exit) {
    const auto level = exit.bottomBaseLine.point.y();
    const auto start = context.timeSorted.find(corner);
    if (start == context.timeSorted.end() || level <= corner.point.y() ||
        exit.bottomBaseLine.point.x() >= corner.point.x()) return {};
    for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
        auto current = start;
        for (size_t i = 0; i < context.timeSorted.size(); ++i) {
            const auto next = (context.*advance)(current);
            if (next->point.x() < current->point.x() || next->point.y() < current->point.y()) break;
            if (next->point.y() >= level) {
                const auto hits = FindInterections(context.transformedPath,
                    QLineF(current->point.x(), level, next->point.x(), level), true);
                // Time increases for prev() in the descending contour set.
                const bool increasing = advance == &SearchContext::prev;
                const double lo = increasing ? current->time : next->time;
                const double hi = increasing ? next->time : current->time;
                for (const auto& hit : hits) {
                    const bool onArc = lo <= hi ? lo <= hit.time && hit.time <= hi : hit.time >= lo || hit.time <= hi;
                    if (hit.pathIndex != 0 || !onArc || hit.point.x() <= corner.point.x()) continue;
                    Stretching entry{};
                    if (!contourNormalCut(context, hit, increasing, false, entry, true) ||
                        entry.bottomBaseLine.point.x() <= corner.point.x()) continue;
                    std::swap(entry.topBaseLine, entry.bottomBaseLine);
                    return entry;
                }
                break;
            }
            current = next;
        }
    }
    return {};
}

StemEdges traceStemEdges(const SearchContext& context, const PathExtrema& crest,
                                       bool allowLevel) {
    StemEdges edges;
    const auto cap = context.timeSorted.find(crest);
    int side = 0;
    for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
        auto foot = cap;
        auto point = cap;
        for (size_t step = 0; step < context.timeSorted.size(); ++step) {
            const auto next = (context.*advance)(point);
            if (next == cap) break;
            const double fall = next->point.y() - foot->point.y();
            // Several almost coincident extrema can describe a rounded nib.
            // Follow through that cap before measuring the descending edge.
            if (allowLevel) {
                if (fall < -1e-8 && QLineF(cap->point, next->point).length() > .05) break;
            } else if (fall <= 0) break;
            if (fall >= 0) foot = next;
            point = next;
        }
        if (foot != cap) edges[side] = *foot;
        ++side;
    }
    return edges;
}

// Follow the two contour arcs down from the cap. A fork must lead into the
// ink strip bounded by those same arcs, even when another stroke is nearby.
static bool forkBelongsToStem(const SearchContext& context, const PathExtrema& crest,
                                const StemEdges& edges, const PathExtrema& tip,
                                const std::vector<QBezier>& curves) {
    if (!edges[0] || !edges[1] || curves.empty() || tip.point.y() <= crest.point.y()) return false;
    const double foot = std::min({edges[0]->point.y(), edges[1]->point.y(), tip.point.y()});
    if (foot <= crest.point.y()) return false;
    const double period = curves.size();
    const auto edgeAt = [&](double y, int side) {
        const int direction = side == 0 ? -1 : 1;
        double low = 0, high = std::fmod(direction * (edges[side]->time - crest.time) + period, period);
        // Bracket the crossing on this edge. A whole-outline polynomial
        // probe can lose a root on a nearly straight Bézier segment.
        QPointF point;
        for (int step = 0; step < 36; ++step) {
            const double middle = (low + high) / 2;
            const double time = std::fmod(crest.time + direction * middle + period, period);
            point = curves[size_t(time)].pointAt(time - std::floor(time));
            (point.y() < y ? low : high) = middle;
        }
        return point;
    };
    const auto section = [&](double y) -> QLineF {
        const QLineF span(edgeAt(y, 0), edgeAt(y, 1));
        const auto [left, right] = std::minmax({span.x1(), span.x2()});
        if (right - left <= 1e-6 || !context.transformedPath.contains(span.center())) return {};
        const auto hits = FindInterections(context.transformedPath, QLineF(left, y, right, y), true);
        if (std::ranges::any_of(hits, [&](const PathExtrema& hit) {
                return hit.point.x() > left + 1e-6 && hit.point.x() < right - 1e-6;
            })) return {};
        return span;
    };
    const double tracedRise = foot - crest.point.y();
    const auto sample = [&](double preferred, double alternate) {
        const auto span = section(crest.point.y() + preferred * tracedRise);
        return span.isNull() ? section(crest.point.y() + alternate * tracedRise) : span;
    };
    // A slice can pass through a turn or a tangent on one edge. Sample
    // just above it, keeping the same two contour arcs.
    const auto upper = sample(.5, .375);
    const auto lower = sample(.75, .625);
    if (upper.isNull() || lower.isNull()) return false;
    const double width = std::max(upper.length(), lower.length());
    if (tracedRise < 2 * width) return false;
    const auto axis = QLineF(upper.center(), lower.center());
    const double atTip = (tip.point.y() - axis.y1()) / axis.dy();
    const double remainingFall = tip.point.y() - lower.center().y();
    if (std::abs(axis.pointAt(atTip).x() - tip.point.x()) > width + .15 * remainingFall) return false;
    // A counter or a bent foot can turn one edge out before the other.
    // Check the ink from the remaining traced strip to the fork itself.
    const std::array shaftProbes{.5, .25, .75};
    return std::ranges::any_of(shaftProbes, [&](double t) {
        return interiorInkSegment(context.transformedPath, lower.pointAt(t), tip.point);
    });
}

std::vector<size_t> stemForks(const SearchContext& context, const PathExtrema& crest,
                                        const std::vector<StemFork>& forks,
                                        const std::vector<QBezier>& curves) {
    const auto edges = traceStemEdges(context, crest, true);
    std::vector<size_t> owned;
    for (size_t i = 0; i < forks.size(); ++i) {
        const auto& fork = forks[i];
        if ((fork.fork || fork.folded()) &&
            forkBelongsToStem(context, crest, edges, fork.tip, curves)) owned.push_back(i);
    }
    return owned;
}

// The incoming branch leaves a notch between the upstroke and the bowl.
// A horizontal slice above that notch must cross only the slim upright.
bool cutTerminalStemFork(SearchContext &ctx, const QJsonObject &form, Stretching &best) {
    const auto &ink = ctx.transformedPath;
    const auto tail = std::prev(ctx.xSorted.end());
    const double pen = form.value("penWidth").toDouble(.5);
    const double inset = form.value("walkWidths").toDouble(.5) * pen;
    const int count = int(baseContourCurves(ink).size());
    double score = std::numeric_limits<double>::infinity();
    for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p) {
        if (p->point.x() < tail->point.x() + 2 * pen)
            continue;
        const auto flank = [&](auto advance) {
            auto step = p;
            do {
                step = (ctx.*advance)(step);
            } while (step != p && QLineF(p->point, step->point).length() < .25 * pen);
            return step->point - p->point;
        };
        const auto a = flank(&SearchContext::prev), b = flank(&SearchContext::next);
        if (std::min(a.y(), b.y()) < -.15 * pen || std::max(a.x(), b.x()) < pen)
            continue;
        const bool forward = a.x() > b.x();
        PathExtrema limit = *p, anchor{};
        limit.time += (forward ? 1 : -1) * count;
        QPointF tangent;
        std::vector<Stretching> proposals;
        Stretching normal{};
        if (advanceOnContour(ink, *p, limit, forward, inset, anchor, tangent) && anchor.point.x() > p->point.x() &&
            contourNormalCut(ctx, anchor, forward, false, normal, true))
            proposals.push_back(normal);
        // A head touching the upright creates a second notch on the other
        // edge of the attachment. Both notches stay on the shared cut.
        for (const auto &q : ctx.timeSorted) {
            const auto d = q.point - p->point;
            if (d.y() > -.4 * pen || d.y() < -4.5 * pen || d.x() < -.4 * pen || d.x() > 3.5 * pen ||
                !interiorChord(ink, q, *p))
                continue;
            Stretching bridge{};
            bridge.topBaseLine = q;
            bridge.bottomBaseLine = *p;
            proposals.push_back(bridge);
        }
        for (auto cut : proposals) {
            orientSection(ctx, cut, tail->time, true);
            const QLineF chord(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (chord.length() > 5 * pen || QLineF(chord.center(), p->point).length() > 4 * pen)
                continue;
            const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
            if (parts.size() != 2 || toSubpaths(parts[1].path).size() != 1)
                continue;
            const auto &body = parts[1].path;
            const auto box = body.boundingRect();
            const double rise = p->point.y() - box.top();
            if (rise < form.value("minRise").toDouble(3) || box.bottom() - p->point.y() < pen ||
                std::abs(box.left() - tail->point.x()) > pen || box.right() > chord.center().x() + 3 * pen)
                continue;
            bool slender = true;
            for (double fraction : {.3, .65}) {
                const double y = box.top() + fraction * rise;
                const auto hits = FindInterections(body, QLineF(box.left() - 1, y, box.right() + 1, y));
                if (hits.size() != 2) {
                    slender = false;
                    break;
                }
                const QLineF band(hits.front().point, hits.back().point);
                if (band.length() > .4 * rise || std::abs(band.center().x() - p->point.x()) > .5 * rise) {
                    slender = false;
                    break;
                }
            }
            if (!slender)
                continue;
            const double fit = chord.length() + .45 * QLineF(chord.center(), p->point).length();
            if (fit < score) {
                score = fit;
                best = cut;
            }
        }
    }
    return std::isfinite(score);
}

// Follow the descending edge from the cap. The first steep-to-shallow
// transition is a body landmark, including when it falls within a Bézier.
static std::optional<PathExtrema> descendingShaftTurn(const std::vector<QBezier>& curves,
        const PathExtrema& crest, int direction, double minRise, double slope) {
    const int count = static_cast<int>(curves.size());
    const int first = direction > 0 ? int(std::floor(crest.time)) : int(std::ceil(crest.time)) - 1;
    double length = 0;
    bool descending = false;
    // The start curve is split at the crest. Its far side bounds the walk
    // if the contour closes before a turn has been found.
    for (int n = 0; n <= count; ++n) {
        const int index = first + direction * n;
        const int wrapped = (index % count + count) % count;
        const auto& curve = curves[wrapped];
        const auto a = direction * (curve.pt2() - curve.pt1());
        const auto b = direction * (curve.pt3() - curve.pt2());
        const auto c = direction * (curve.pt4() - curve.pt3());
        const double start = n == 0 ? crest.time - index : direction > 0 ? 0. : 1.;
        const double end = n == count ? crest.time - first : direction > 0 ? 1. : 0.;
        std::array<double, 6> stops{start, end};
        size_t stopCount = 2;
        for (int sign : {-1, 1}) {
            const auto slopeDifference = [&](const QPointF& delta) { return delta.y() + sign * slope * delta.x(); };
            std::array<double, 3> storage;
            for (double root : cubicRoots(0, slopeDifference(a - 2 * b + c), 2 * slopeDifference(b - a),
                                          slopeDifference(a), storage))
                if (direction * (root - start) > 1e-8 && direction * (end - root) > 1e-8)
                    stops[stopCount++] = root;
        }
        std::sort(stops.begin(), stops.begin() + stopCount,
                  [=](double lhs, double rhs) { return direction * lhs < direction * rhs; });
        for (size_t i = 1; i < stopCount; ++i) {
            const double from = stops[i - 1], to = stops[i];
            if (std::abs(from - to) <= 1e-8) continue;
            const auto point = curve.pointAt(from);
            const double fall = point.y() - crest.point.y();
            if (length > minRise * 7 || (fall < minRise * .65 && length > minRise * 1.7)) return {};
            const double t = (from + to) / 2, u = 1 - t;
            const auto tangent = u * u * a + 2 * t * u * b + t * t * c;
            if (std::hypot(tangent.x(), tangent.y()) <= 1e-9) continue;
            const bool steep = tangent.y() > slope * std::abs(tangent.x());
            if (descending && !steep && fall >= minRise)
                return PathExtrema{0, std::fmod(wrapped + from, count), point, false, false, false};
            descending = steep;
            length += curve.getSubRange(std::min(from, to), std::max(from, to)).length(.01);
        }
    }
    return {};
}

std::vector<TurningShaftBody> recognizeTurningShafts(const InkOutline &outline, const QJsonObject &body) {
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    std::vector<TurningShaftBody> bodies;
    const auto& ink = context.transformedPath;
    const auto curves = baseContourCurves(ink);
    const double period = curves.size();
    if (curves.empty()) return bodies;
    const double minRise = body.value("minRise").toDouble(3);
    const double slope = std::tan(qDegreesToRadians(body.value("turnAngle").toDouble(50)));
    for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest) {
        if (context.prev(crest)->point.y() < crest->point.y() - 1e-7 ||
            context.next(crest)->point.y() < crest->point.y() - 1e-7) continue;
        const auto forward = descendingShaftTurn(curves, *crest, 1, minRise, slope);
        const auto backward = descendingShaftTurn(curves, *crest, -1, minRise, slope);
        if (!forward || !backward) continue;
        const double rise = std::min(forward->point.y(), backward->point.y()) - crest->point.y();
        const double y = crest->point.y() + rise / 2;
        const double forwardEnd = std::fmod(forward->time - crest->time + period, period);
        const double backwardEnd = std::fmod(crest->time - backward->time + period, period);
        auto hits = FindInterections(ink, QLineF(context.bbox.left() - 1, y, context.bbox.right() + 1, y),
                                            true, RootSearch::BracketAllCrossings);
        std::ranges::sort(hits, {}, [](const auto& hit) { return hit.point.x(); });
        const auto alongShaft = [&](const PathExtrema& a, const PathExtrema& b) {
            return std::fmod(a.time - crest->time + period, period) < forwardEnd &&
                std::fmod(crest->time - b.time + period, period) < backwardEnd;
        };
        for (size_t i = 1; i < hits.size(); ++i) {
            const auto& a = hits[i - 1];
            const auto& b = hits[i];
            const double width = QLineF(a.point, b.point).length();
            if (width <= .06 || width * 2 >= rise || (!alongShaft(a, b) && !alongShaft(b, a)) ||
                !interiorChord(ink, a, b)) continue;
            bodies.push_back({*crest, width, {*forward, *backward}});
            break;
        }
    }
    return bodies;
}

std::vector<TaperedArmBody> recognizeTaperedArms(const InkOutline &outline, const QJsonObject &form) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    const auto& ink = ctx.transformedPath;
    const double minRise = form.value("minRise").toDouble(3.);
    const double maxNeck = form.value("maxRootWidth").toDouble(1.6);
    const auto curves = baseContourCurves(ink);
    if (curves.empty())
        return {};
    std::set<double> attachmentCorners;
    for (const auto& point : ctx.timeSorted) {
        if (!point.isSharp || std::abs(point.time - std::round(point.time)) > 1e-8) continue;
        const int index = int(std::round(point.time)) % int(curves.size());
        const auto& before = curves[(index + curves.size() - 1) % curves.size()];
        const auto& after = curves[index];
        auto back = before.pt3() - before.pt4(), forward = after.pt2() - after.pt1();
        if (std::hypot(back.x(), back.y()) < 1e-9) back = before.pointAt(.9999) - before.pt4();
        if (std::hypot(forward.x(), forward.y()) < 1e-9) forward = after.pointAt(.0001) - after.pt1();
        const double turn = forward.x() * back.y() - forward.y() * back.x();
        if (std::abs(turn) > 1e-10 && (turn > 0) == (ctx.orientation == Orientation::CounterClockwise))
            attachmentCorners.insert(point.time);
    }
    std::vector<TaperedArmBody> bodies;
    for (auto it = ctx.timeSorted.begin(); it != ctx.timeSorted.end(); ++it) {
        if (!it->isSharp || it->point.x() > ctx.bbox.left() + 1.35 * ctx.bbox.height())
            continue;
        if (it->point.y() - ctx.bbox.top() < minRise)
            continue;
        auto sections = cornerSections(ctx, *it);
        for (const auto& other : ctx.timeSorted) {
            if (!other.isSharp || other.time <= it->time || QLineF(other.point, it->point).length() > maxNeck ||
                !interiorChord(ink, *it, other))
                continue;
            Stretching cut{};
            cut.topBaseLine = *it;
            cut.bottomBaseLine = other;
            sections.push_back(cut);
        }
        for (auto cut : sections) {
            const QLineF neck(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (neck.length() > maxNeck)
                continue;
            for (int orientation = 0; orientation < 2; ++orientation) {
                std::swap(cut.topBaseLine, cut.bottomBaseLine);
                const auto pieces = cutRegions(ink, {}, {cut}, ctx.orientation, true);
                if (pieces.size() != 2)
                    continue;
                const auto& arm = pieces[1].path;
                const auto& rest = pieces[0].path;
                const auto box = arm.boundingRect();
                const auto root = neck.center();
                if (toSubpaths(arm).size() != 1 || box.height() < minRise || box.width() < .38 * box.height() ||
                    box.width() > 1.45 * box.height() || box.right() - root.x() > .65 ||
                    box.bottom() - root.y() > .65 || box.left() > ctx.bbox.left() + .35 * box.height() ||
                    rest.boundingRect().top() > box.top() + .25 * box.height() ||
                    rest.boundingRect().bottom() < root.y() + .18 * box.height())
                    continue;
                std::array<QLineF, 3> cross;
                bool valid = true;
                for (int j = 0; j < 3; ++j) {
                    const auto y = box.top() + box.height() * (.22 + .32 * j);
                    auto hits = FindInterections(arm, QLineF(box.left() - 1, y, box.right() + 1, y), true);
                    if (hits.size() != 2) {
                        valid = false;
                        break;
                    }
                    cross[j] = QLineF(hits.front().point, hits.back().point);
                }
                if (!valid || cross[0].center().x() + .15 * box.width() > cross[1].center().x() ||
                    cross[1].center().x() + .15 * box.width() > cross[2].center().x() ||
                    cross[0].length() < 1.2 * cross[2].length() || cross[2].length() > .3 * box.width())
                    continue;
                // Prefer the actual junction only after validating the complete
                // tapered arm above. Smooth roots retain the cross-section search.
                const bool junction = attachmentCorners.count(cut.topBaseLine.time) &&
                    attachmentCorners.count(cut.bottomBaseLine.time);
                const double fit = neck.length() + 1.5 * (box.bottom() - root.y()) + 1.5 * (box.right() - root.x()) -
                                   .12 * box.height();
                bodies.push_back({cut.topBaseLine, cut.bottomBaseLine, cross, box, neck.length(), junction, fit});
            }
        }
    }
    return bodies;
}

bool cutTaperedArms(const std::vector<TaperedArmBody> &bodies, const QJsonObject &policy, Stretching &result,
                           bool *cornerPair) {
    if (cornerPair)
        *cornerPair = false;
    const TaperedArmBody *best = nullptr;
    std::pair<int, double> score{2, std::numeric_limits<double>::infinity()};
    for (const auto &body : bodies) {
        const auto value = std::pair{policy.value("preferJunction").toBool(true) && !body.junction ? 1 : 0, body.fit};
        if (value < score) {
            score = value;
            best = &body;
        }
    }
    if (!best)
        return false;
    result = {};
    result.topBaseLine = result.topLeft = result.topRight = best->first;
    result.bottomBaseLine = result.bottomLeft = result.bottomRight = best->second;
    result.referenceWidth = best->width;
    result.fixedRightBoundary = true;
    result.kashidaOverrides.insert("enabled", false);
    if (cornerPair)
        *cornerPair = best->junction;
    return true;
}

bool declaredArmRoot(SearchContext &context, const QJsonObject &form, Stretching &result, bool *cornerPair) {
    return cutTaperedArms(
        recognizeTaperedArms(inkOutline(context), form.value("body").toObject().value("arm").toObject()),
        form.value("cuts").toObject().value("arm").toObject(), result, cornerPair);
}

} // namespace MedinaCutter
