#include "../../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

static std::vector<LamForkBody> recognizeLamForkBodies(const SearchContext& context, double flankSpan) {
    std::vector<LamForkBody> bodies;
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
LamBodyEvidence recognizeLamBodies(const SearchContext& context, double flankSpan, bool allowLevelCrest) {
    LamBodyEvidence result;
    result.forks = recognizeLamForkBodies(context, flankSpan);
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

// The lower contour of a forked lam rises to a tip under the stem. Follow
// each flank away from that tip and cut across the same strip of ink.
std::optional<Stretching> cutLamForkFlank(const SearchContext& context, const PathExtrema& tip,
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

bool lamAnchorWithinWalk(const SearchContext& context, const PathExtrema& tip,
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

static LamShaftEdges traceLamShaftEdges(const SearchContext& context, const PathExtrema& crest,
                                       bool allowLevel) {
    LamShaftEdges edges;
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
static bool forkBelongsToLamShaft(const SearchContext& context, const PathExtrema& crest,
                                const LamShaftEdges& edges, const PathExtrema& tip,
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

std::vector<size_t> lamShaftForks(const SearchContext& context, const PathExtrema& crest,
                                        const std::vector<LamForkBody>& forks,
                                        const std::vector<QBezier>& curves) {
    const auto edges = traceLamShaftEdges(context, crest, true);
    std::vector<size_t> owned;
    for (size_t i = 0; i < forks.size(); ++i) {
        const auto& fork = forks[i];
        if ((fork.fork || fork.folded()) &&
            forkBelongsToLamShaft(context, crest, edges, fork.tip, curves)) owned.push_back(i);
    }
    return owned;
}

// The incoming branch leaves a notch between the upstroke and the bowl.
// A horizontal slice above that notch must cross only the slim upright.
static bool terminalStemFork(SearchContext &ctx, const QJsonObject &form, Stretching &best) {
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

// Initial/final lām declarations are evaluated here, before the legacy
// dispatcher. A failed declaration is a miss, never a legacy matcher fallback.
bool declaredLamBoundary(SearchContext& context, const QJsonObject& config, const QJsonObject& form) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto body = formDescription(config, form, "body");
    const auto cuts = formDescription(config, form, "cuts");
    const auto shape = body.value("shape").toString();
    bool found = false;
    if (shape == "contour-stem")
        found = declaredContourRouteCut(context, body.value("route").toObject(), cuts.value("route").toObject());
    else if (shape == "descending-stem")
        found = descendingStemExit(context, body.value("stem").toObject(), context.res);
    else if (shape == "terminal-bowl") {
        const auto evidence = recognizeTerminalStemBodies(inkOutline(context), body.value("stem").toObject());
        if (inspectingForm) for (const auto& item : evidence) {
                inspectLandmark("Stem crest", item.crest);
                inspectLandmark("Stem foot", item.limit);
                inspectLandmark("Bowl edge", item.bowlEdge);
        }
        if (evidence.empty()) inspectReason("Body", "No terminal shaft passed the crest, rise and local width constraints.");
        found = cutTerminalStemBodies(context, evidence, body.value("stem").toObject(), cuts.value("section").toObject());
        if (!found && !evidence.empty()) inspectReason("Cuts", "Terminal shaft recognized; no entry section retained the bowl and stem.");
    } else if (shape == "terminal-fork")
        found = terminalStemFork(context, withOverrides(body.value("stem").toObject(), cuts.value("section").toObject()), context.res);
    else if (shape == "terminal-return")
        found = terminalTailEntry(context, withOverrides(body.value("tail").toObject(), cuts.value("section").toObject()), context.res);
    if (found) inspectCandidate("Boundary proposal", context.res);
    else if (shape != "contour-stem" && shape != "terminal-bowl")
        inspectReason("Body / cuts", "No candidate passed this shape's combined attachment checks.");
    return found;
}

LamMedialBodyEvidence recognizeMedialLamBodies(const InkOutline &outline, const QJsonObject &body) {
    LamMedialBodyEvidence result;
    result.source = outline.ink;
    if (body.value("shape") == "turning-shaft") {
        result.turningShafts = recognizeTurningShafts(outline, body.value("shaft").toObject());
        return result;
    }
    if (body.value("shape") != "forked-stem") return result;
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    result.features = recognizeLamBodies(geometry, body.value("minFlankSpan").toDouble(), true);
    const auto routes = body.value("exitRoutes").toArray();
    const auto curves = baseContourCurves(geometry.transformedPath);
    for (const auto& crest : result.features.crests) {
        LamStemBody stem{crest, traceLamShaftEdges(geometry, crest, false), {}, {},
                         lamShaftForks(geometry, crest, result.features.forks, curves)};
        std::erase_if(stem.forks, [&](size_t i) {
            return !result.features.forks[i].isConcave(geometry.orientation);
        });
        if (body.value("entryRoute").isObject())
            for (int reverse = 0; reverse < 2; ++reverse)
                stem.entryRoutes[reverse] =
                    recognizeSavedRoute(geometry, body.value("entryRoute").toObject(), &crest, reverse);
        for (const auto& route : routes) {
            std::array<std::optional<SavedContourRoute>, 2> alternatives;
            for (int reverse = 0; reverse < 2; ++reverse)
                alternatives[reverse] = recognizeSavedRoute(geometry, route.toObject(), &crest, reverse);
            stem.exitRoutes.push_back(std::move(alternatives));
        }
        result.stems.push_back(std::move(stem));
    }
    return result;
}

std::shared_ptr<const LamMedialBodyEvidence> recognizedMedialLamBodies(
        SearchContext& context, const QJsonObject& body) {
    const auto generate = [&] {
        return std::make_shared<const LamMedialBodyEvidence>(recognizeMedialLamBodies(inkOutline(context), body));
    };
    if (!context.outlineCache) return generate();
    const auto key = std::make_pair(bodyDescriptionKey(body), static_cast<int>(context.orientation));
    auto& cached = context.outlineCache->medialLamBodies[key];
    if (!cached || cached->source != context.transformedPath) cached = generate();
    return cached;
}

void inspectLamBodyEvidence(const LamMedialBodyEvidence& evidence) {
    if (!inspectingForm) return;
    for (const auto& crest : evidence.features.crests) inspectLandmark("Stem crest", crest);
    for (const auto& fork : evidence.features.forks) inspectLandmark("Fork candidate", fork.tip);
    for (const auto& stem : evidence.stems) {
        for (const auto& foot : stem.descendingFlanks) if (foot) inspectLandmark("Flank foot", *foot);
        const auto route = [](const auto& r) {
            if (r) inspectLandmark("Route anchor", r->anchor);
        };
        for (const auto& r : stem.entryRoutes) route(r);
        for (const auto& alternatives : stem.exitRoutes) for (const auto& r : alternatives) route(r);
    }
    for (const auto& shaft : evidence.turningShafts) {
        inspectLandmark("Shaft crest", shaft.crest);
        for (const auto& turn : shaft.turns) if (turn) inspectLandmark("Flank turn", *turn);
    }
}

// Assembly owns all neighbor compatibility, contextual scale, priority and
// shared-attachment checks. Its input inventory is already recognized and
// remains unchanged when incoming/outgoing joins or cut policies change.
std::vector<MedialCandidate> computeMedialLamCuts(SearchContext& context,
        const QJsonObject& config, const QJsonObject& form, const LamMedialBodyEvidence& evidence,
        const Stretching& incoming,
        const QPointF& entry, const QPointF& exit, double stroke, bool open, bool recoverTeeth,
        bool allowFork, const std::optional<Stretching>& sharedExit) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto lamBody = formDescription(config, form, "body");
    const auto lam = formDescription(config, form, "cuts");
    std::vector<MedialCandidate> candidates;
    if (lamBody.value("shape") == "turning-shaft") {
        for (const auto& [right, left] : cutTurningShafts(context, evidence.turningShafts, lamBody.value("shaft").toObject()))
            candidates.emplace_back(right, left);
        if (inspectingForm) for (const auto& item : candidates) inspectCandidate("Turning shaft", item.left, item.right);
        if (candidates.empty()) inspectReason("Body / cuts", "No turning shaft passed the rise, width and section checks.");
        return candidates;
    }
    if (lamBody.value("shape") != "forked-stem") return candidates;
    const auto placement = lam.value("placement").toObject();
    const auto before = [&](const Stretching& a, const Stretching& b) {
        return cutPrecedes(a, b, context.orientation);
    };
    const auto center = [](const Stretching& cut) {
        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).center();
    };
    const auto shortLamExit = medialLamShortExit(config);
    const double minExitRun = shortLamExit.value("minHorizontalRun").toDouble();
    const double maxExitEntryGap = shortLamExit.value("maxEntryGap").toDouble();
    const double minExitLengthRatio = shortLamExit.value("minLengthRatio").toDouble();
    const double exitAnchorTolerance = shortLamExit.value("anchorTolerance").toDouble();
    const bool cutsAroundHollow = shortLamExit.value("cutsAroundHollow").toBool();
    const double maxEntrySlope = std::tan(qDegreesToRadians(shortLamExit.value("entryMaxTilt").toDouble(12)));
    const auto originalForm = context.form;
    const auto originalResult = context.res;
    const auto originalNext = context.nextIter;
    const auto routes = lamBody.value("exitRoutes").toArray();
    const auto routeCuts = lam.value("exitRoutes").toArray();
    const auto& bodyEvidence = evidence.features;
    // This is a placement gate, not the body recognizer: all stem candidates
    // above exist even when this join rejects their position or height.
    const auto fitsSlot = [&](const LamStemBody& stem) {
        const auto& crest = stem.crest;
        const bool fits = crest.point.x() < entry.x() &&
            (open ? entry.y() : std::min(entry.y(), exit.y())) - crest.point.y() >=
                lamBody.value("minRise").toDouble() * stroke &&
            (sharedExit || getJoinTime(incoming, context.orientation).isStrictLeft(crest.time));
        if (!fits) inspectReason("Assembly", "Recognized stem is outside this slot's position, height or contour-order limits.");
        if (cutGeometryMemo && cutGeometryMemo->activeForm) {
            ++cutGeometryMemo->activeForm->placementChecks;
            cutGeometryMemo->activeForm->placementRejected += !fits;
        }
        return fits;
    };
    double firstStemX = -std::numeric_limits<double>::infinity();
    std::vector<const LamStemBody*> eligible;
    for (const auto& stem : evidence.stems) {
        if (!fitsSlot(stem)) continue;
        firstStemX = std::max(firstStemX, stem.crest.point.x());
        eligible.push_back(&stem);
    }
    // The rightmost crest is the usual starting point for a route. A low
    // inward fork can itself register as a crest to the right of its shaft.
    const auto routeStem = [&](const LamStemBody& stem) {
        return sharedExit || stem.crest.point.x() >= firstStemX - 1e-6;
    };
    std::vector<MedialCandidate> forkCandidates;
    const double forkOffset = lam.value("forkOffset").toDouble();
    const auto proposeForks = [&](bool useRouteStem) {
        if (!allowFork || forkOffset <= 0 || sharedExit || incoming.counterEntry.pathIndex > 0) return;
        const auto entrySide = getJoinTime(incoming, context.orientation);
        const auto& recognizedBodies = bodyEvidence.forks;
        for (const auto* selected : eligible) {
            const auto& stem = *selected;
            if (routeStem(stem) != useRouteStem) continue;
            const auto* crest = &stem.crest;
            for (const auto index : stem.forks) {
                const auto& body = recognizedBodies[index];
                const auto* tip = &body.tip;
                if (tip->point.y() - crest->point.y() < lamBody.value("minRise").toDouble() * stroke ||
                    std::abs(tip->point.x() - crest->point.x()) > 3 * stroke ||
                    std::abs(tip->point.y() - entry.y()) > 4 * stroke ||
                    !entrySide.isStrictLeft(tip->time)) continue;
                const bool rightForward = body.rightForward;
                // Small extrema can subdivide one flank. Walk through
                // them and let the body and join checks bound the result.
                for (double distance : {forkOffset, 0.}) {
                    const auto right = cutLamForkFlank(context, *tip,
                        std::nullopt, rightForward, 1, distance);
                    const auto left = cutLamForkFlank(context, *tip, std::nullopt, !rightForward, -1, distance);
                    if (!right || !left || !before(incoming, *right) || !before(*right, *left) ||
                        std::min(right->referenceWidth, left->referenceWidth) <= 1e-6 ||
                        (distance == 0 && std::max(right->referenceWidth, left->referenceWidth) > 2.5 * stroke)) continue;
                    const auto parts = cutRegions(context.transformedPath, {}, {*right, *left}, context.orientation, true);
                    if (parts.size() != 3) continue;
                    const auto& body = parts[1].path;
                    const auto bounds = body.boundingRect();
                    if (std::abs(bounds.top() - crest->point.y()) > .01 ||
                        bounds.width() > lamBody.value("maxWidthToHeight").toDouble() * bounds.height() ||
                        (form.contains("bodyCounters") &&
                         static_cast<int>(toSubpaths(body).size()) - 1 != form.value("bodyCounters").toInt())) continue;
                    forkCandidates.push_back({*right, *left, false, {}, *crest, *tip});
                }
            }
        }
    };
    proposeForks(true);
    for (qsizetype routeIndex = 0; routeIndex < routes.size(); ++routeIndex) {
        const auto route = routes[routeIndex];
        const auto routePolicy = routeCuts[routeIndex].toObject();
        const size_t routeStart = candidates.size();

        for (const auto* selected : eligible) {
            const auto& stem = *selected;
            if (!routeStem(stem)) continue;
            const auto* crest = &stem.crest;
            // Reading-order selection belongs to assembly. Recognition keeps
            // the following stem available for its own later letter slot.
            std::vector<Stretching> rightOptions;
            if (incoming.counterEntry.pathIndex > 0) rightOptions.push_back(incoming);
            for (int flank = 0; flank < 2; ++flank) {
                if (!stem.descendingFlanks[flank]) continue;
                Stretching right{};
                const int traversal = flank == 1 ? 1 : -1;
                if (alefTurnCut(context, *stem.descendingFlanks[flank], placement.value("turnAngle").toDouble(),
                                stroke, right, crest, traversal)) rightOptions.push_back(right);
            }
            // A bowl-shaped foot can turn after the stem's 45-degree
            // normal. Its optional route stays on this stem's flank.
            const auto turnCount = rightOptions.size();
            if (lamBody.value("entryRoute").isObject()) {

                for (bool reverse : {false, true}) {
                    context.res = {};
                    if (projectSavedRoute(context, stem.entryRoutes[reverse], lamBody.value("entryRoute").toObject(),
                                          lam.value("entryRoute").toObject()))
                        rightOptions.push_back(context.res);
                }

            }
            for (auto right : rightOptions) {
                const auto incomingChord = QLineF(incoming.topBaseLine.point, incoming.bottomBaseLine.point);
                const auto rightChord = QLineF(right.topBaseLine.point, right.bottomBaseLine.point);
                bool shared = right.counterEntry.pathIndex > 0 ||
                    incomingChord.intersects(rightChord, nullptr) == QLineF::BoundedIntersection;
                if (recoverTeeth && !shared && !before(incoming,right) &&
                    getJoinTime(incoming,context.orientation).isStrictLeft(crest->time) &&
                    QLineF(entry,center(right)).length() <= placement.value("entryOverlap").toDouble()*stroke) {
                    right = incoming;
                    shared = true;
                }
                if ((!shared && !before(incoming, right)) ||
                    (shared && QLineF(entry, center(right)).length() > stroke)) continue;
                bool accepted = false;
                for (const bool reverse : {false, true}) {
                    context.res = {};
                    // In a lam-alef, the outgoing boundary is intrinsic to
                    // the ligature. Containment below rejects the diagonal
                    // alef crest and keeps the upright lam and its return.
                    std::optional<ForkRefinement> fork;
                    if (sharedExit) context.res = *sharedExit;
                    else if (!projectSavedRoute(context, stem.exitRoutes[routeIndex][reverse], route.toObject(),
                                                routePolicy, &fork))
                        continue;
                    // Moving only the exit onto its outgoing branch must
                    // not make a previously crossing entrance eligible.
                    if (fork && !before(right, fork->originalExit)) continue;
                    if (!before(right, context.res)) continue;
                    const auto pieces = cutRegions(context.transformedPath, {}, {right, context.res}, context.orientation, true);
                    const auto sample = crest->point + QPointF(0, .01 * stroke);
                    if (pieces.size()!=3) continue;
                    if (!pieces[1].path.contains(sample)) {
                        if (!recoverTeeth) continue;
                        // An inclined pen cap can leave the point directly
                        // below its crest outside the ink. Retain the
                        // center of the narrow shaft immediately below it.
                        const double y = crest->point.y() + .65 * stroke;
                        auto section = FindInterections(context.transformedPath,
                            QLineF(crest->point.x()-2*stroke,y,crest->point.x()+2*stroke,y), true);
                        std::sort(section.begin(),section.end(),[](const auto& a,const auto& b) {
                            return a.point.x()<b.point.x();
                        });
                        bool retainsShaft = false;
                        for (size_t j=1;j<section.size();++j) {
                            const QLineF cross(section[j-1].point,section[j].point);
                            if (cross.length()>2*stroke || !interiorChord(context.transformedPath,section[j-1],section[j])) continue;
                            if (pieces[1].path.contains(cross.center()) &&
                                pieces[1].path.boundingRect().adjusted(-.01,-.01,.01,.01).contains(crest->point)) retainsShaft=true;
                        }
                        if (!retainsShaft) continue;
                    }
                    // Apply body ownership before accepting a route;
                    // otherwise a route through the next loop can
                    // suppress a valid later route for this stem.
                    if (form.contains("bodyCounters") &&
                        static_cast<int>(toSubpaths(pieces[1].path).size()) - 1 !=
                            form.value("bodyCounters").toInt()) continue;
                    const auto bodyBounds = pieces[1].path.boundingRect();
                    if (!sharedExit && lamBody.contains("maxWidthToHeight") && bodyBounds.width() >
                        lamBody.value("maxWidthToHeight").toDouble() * bodyBounds.height()) continue;
                    auto bodyEntry = right;
                    if (fork && !shared && placement.value("levelForkEntry").toBool()) {
                        const auto near = levelForkEntry(context, fork->corner, context.res);
                        if (near && before(right, *near) && before(*near, context.res)) {
                            const auto local = cutRegions(context.transformedPath, {}, {*near, context.res},
                                                          context.orientation, true);
                            if (local.size() == 3 && local[1].path.contains(sample)) bodyEntry = *near;
                        }
                    }
                    std::optional<std::pair<Stretching, Stretching>> hollowCuts;
                    if (!sharedExit && !shortLamExit.isEmpty()) {
                        const auto top = context.res.topBaseLine;
                        const auto bottom = context.res.bottomBaseLine;
                        const double entryBottomX = bodyEntry.bottomBaseLine.point.x();
                        // On a folded lam foot a normal can cross the whole bend
                        // and land near the entry. Keep the same exit landmark
                        // and use the short section of ink directly beneath it.
                        if (bottom.point.x() - top.point.x() > minExitRun * stroke &&
                            entryBottomX - bottom.point.x() <= maxExitEntryGap * stroke) {
                            auto local = context.res;
                            if (medialChordAt(context, top.point.x(), top.point.y(), stroke,
                                              local, false, top.point.y() - exitAnchorTolerance) &&
                                std::abs(local.topBaseLine.point.y() - top.point.y()) < exitAnchorTolerance &&
                                QLineF(top.point, bottom.point).length() >
                                    minExitLengthRatio * QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length() &&
                                before(bodyEntry, local)) {
                                local.topBaseLine = top;
                                local.topLeft = local.topRight = top;
                                local.bottomLeft = local.bottomRight = local.bottomBaseLine;
                                local.referenceWidth = QLineF(top.point, local.bottomBaseLine.point).length();
                                local.length = 0;
                                const auto entryChord = bodyEntry.bottomBaseLine.point - bodyEntry.topBaseLine.point;
                                if (!shared && cutsAroundHollow &&
                                    std::abs(entryChord.x()) <= maxEntrySlope * std::abs(entryChord.y())) {
                                    // A turn at the hollow can cross the original long exit.
                                    // Recheck it against the shortened exit of the same foot.
                                    for (size_t turn = 0; turn < turnCount; ++turn) {
                                        const auto& hollow = rightOptions[turn];
                                        if (!before(bodyEntry, hollow) || !before(hollow, local)) continue;
                                        const auto axis = center(hollow);
                                        const double width = QLineF(hollow.topBaseLine.point, hollow.bottomBaseLine.point).length();
                                        if (width <= 0) continue;
                                        auto hollowRight = bodyEntry, hollowLeft = local;
                                        const auto sectionAt = [&](Stretching& boundary, double x, double y, bool normal) {
                                            if (!medialChordAt(context, x, y, width, boundary) ||
                                                (normal && !strokeNormalCut(context, boundary))) return false;
                                            boundary.referenceWidth = QLineF(boundary.topBaseLine.point, boundary.bottomBaseLine.point).length();
                                            return boundary.referenceWidth <= 2 * width;
                                        };
                                        // The existing cuts bound the space available on each side.
                                        const double rightX = axis.x() + width, leftX = axis.x() - width;
                                        if (rightX < center(bodyEntry).x() &&
                                            (!sectionAt(hollowRight, rightX, center(bodyEntry).y(), true) ||
                                             !before(bodyEntry, hollowRight) || !before(hollowRight, hollow)))
                                            continue;
                                        if (leftX > center(local).x() &&
                                            (!sectionAt(hollowLeft, leftX, center(local).y(), false) ||
                                             !before(hollow, hollowLeft) || !before(hollowLeft, local)))
                                            continue;
                                        const auto hollowPieces = cutRegions(context.transformedPath, {}, {hollowRight, hollowLeft},
                                                                             context.orientation, true);
                                        if (hollowPieces.size() != 3 || !hollowPieces[1].path.contains(sample) ||
                                            toSubpaths(hollowPieces[1].path).size() != toSubpaths(pieces[1].path).size())
                                            continue;
                                        hollowCuts = std::pair{hollowRight, hollowLeft};
                                        break;
                                    }
                                }
                                context.res = local;
                            }
                        }
                    }
                    candidates.push_back({bodyEntry, context.res, shared, hollowCuts, *crest}); accepted = true;
                    if (sharedExit) break;
                }
                if (accepted) break;
            }
        }
        // Preserve route priority, but let the assembler try the
        // next route when this one conflicts with the final body.
        std::stable_sort(candidates.begin() + routeStart, candidates.end(), [](const auto& a, const auto& b) {
            return Stretching::cmpbyx{}(a.right, b.right);
        });
    }
    // Rank occurrences before route alternatives. Otherwise an
    // early route through the following stem can hide a later
    // valid route around this lam and put its shaft in a join.
    // A folded ligature already fixes the outgoing seam.
    // Preserve its established route priority rather than
    // moving the incoming seam to a different flank.
    if (!sharedExit)
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return Stretching::cmpbyx{}(a.right, b.right);
        });
    if (candidates.empty() && forkCandidates.empty()) {
        proposeForks(false);
        std::stable_sort(forkCandidates.begin(), forkCandidates.end(), [](const auto& a, const auto& b) {
            return a.lamCrest->point.x() > b.lamCrest->point.x();
        });
    }
    candidates.insert(candidates.begin(), forkCandidates.begin(), forkCandidates.end());
    if (inspectingForm) for (const auto& item : candidates) inspectCandidate(item.lamFork ? "Fork flanks" : "Stem route", item.left, item.right);
    if (candidates.empty()) inspectReason("Assembly", "No recognized shaft produced compatible cuts after rise, proportions, ordering and attachment checks.");
    context.form = originalForm;
    context.res = originalResult;
    context.nextIter = originalNext;
    return candidates;
}

void appendCandidateBoundary(QByteArray& key, const Stretching& cut) {
    for (const auto* p : {&cut.topBaseLine, &cut.bottomBaseLine, &cut.topLeft, &cut.topRight,
                          &cut.bottomLeft, &cut.bottomRight, &cut.counterEntry, &cut.counterExit}) {
        appendGeometryAnchor(key, *p);
        appendGeometryKey(key, p->isX); appendGeometryKey(key, p->isY); appendGeometryKey(key, p->isSharp);
    }
    appendGeometryKey(key, cut.length);
    appendGeometryKey(key, cut.clockWise);
    appendGeometryKey(key, cut.fixedRightBoundary);
    appendGeometryKey(key, cut.fixedLeftBoundary);
    appendGeometryKey(key, cut.topEndpointTolerance);
    appendGeometryKey(key, cut.referenceWidth);
    appendGeometryKey(key, cut.readingOrder);
    appendGeometryPoint(key, cut.counterBendWeights);
    const auto settings = QJsonDocument(cut.kashidaOverrides).toJson(QJsonDocument::Compact);
    appendGeometryKey(key, settings.size());
    key.append(settings);
}

std::vector<MedialCandidate> assembleMedialLamCuts(SearchContext& context,
        const QJsonObject& config, const QJsonObject& form, const LamMedialBodyEvidence& evidence,
        const Stretching& incoming, const QPointF& entry, const QPointF& exit, double stroke,
        bool open, bool recoverTeeth, bool allowFork, const std::optional<Stretching>& sharedExit) {
    CandidateSearchScope measured(form);
    LamCandidateMemo* memo = nullptr;
    QByteArray key;
    if (context.outlineCache && !formInspection) {
        auto& saved = context.outlineCache->lamCandidates;
        if (!saved || saved->source != context.transformedPath ||
            saved->source.fillRule() != context.transformedPath.fillRule() || saved->orientation != context.orientation) {
            saved = std::make_shared<LamCandidateMemo>();
            saved->source = context.transformedPath;
            saved->orientation = context.orientation;
        }
        memo = saved.get();
        appendGeometryKey(key, memo->objectId(config));
        appendGeometryKey(key, memo->objectId(form));
        appendCandidateBoundary(key, incoming);
        appendGeometryPoint(key, entry); appendGeometryPoint(key, exit);
        appendGeometryKey(key, stroke); appendGeometryKey(key, context.pairedStrokeWidth);
        appendGeometryKey(key, open); appendGeometryKey(key, recoverTeeth); appendGeometryKey(key, allowFork);
        appendGeometryKey(key, bool(sharedExit));
        if (sharedExit) appendCandidateBoundary(key, *sharedExit);
        if (const auto* hit = memo->candidates.object(key)) return *hit;
    }
    if (measured.stats) {
        ++measured.stats->builds;
        measured.stats->bodies += evidence.stems.size() + evidence.turningShafts.size();
    }
    auto result = computeMedialLamCuts(context, config, form, evidence, incoming, entry, exit, stroke,
                                     open, recoverTeeth, allowFork, sharedExit);
    if (measured.stats) measured.stats->cutPairs += result.size();
    if (memo) {
        const size_t cost = 128 + key.size() + sizeof(MedialCandidate) * result.size();
        if (cost <= size_t(memo->candidates.maxCost()))
            memo->candidates.insert(key, new std::vector<MedialCandidate>(result), int(cost));
    }
    return result;
}

} // namespace MedinaCutter
