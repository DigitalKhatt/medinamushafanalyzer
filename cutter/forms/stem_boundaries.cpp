#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

MedialStemEvidence recognizeMedialStems(const InkOutline &outline, const QJsonObject &body) {
    MedialStemEvidence result;
    result.source = outline.ink;
    const bool turningShaft = body.value("shape") == "turning-shaft";
    if (!turningShaft && body.value("shape") != "forked-stem") return result;
    if (turningShaft) result.turningShafts = recognizeTurningShafts(outline, body.value("shaft").toObject());
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    result.features = recognizeStemForkEvidence(geometry, body.value("minFlankSpan").toDouble(), true);
    const auto routes = body.value("exitRoutes").toArray();
    const auto curves = baseContourCurves(geometry.transformedPath);
    for (const auto& crest : result.features.crests) {
        MedialStem stem{crest, {}, {}, {}, stemForks(geometry, crest, result.features.forks, curves)};
        std::erase_if(stem.forks, [&](size_t i) {
            return !result.features.forks[i].isConcave(geometry.orientation);
        });
        if (turningShaft) {
            if (!stem.forks.empty()) result.stems.push_back(std::move(stem));
            continue;
        }
        stem.descendingFlanks = traceStemEdges(geometry, crest, false);
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

void inspectStemEvidence(const MedialStemEvidence& evidence) {
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

// A shaft and its own fork supply the only anchors. Cut policies can walk
// from those landmarks or use the turn directly at a tight attachment.
static std::vector<MedialCandidate> turningShaftCuts(SearchContext& context,
        const MedialStemEvidence& evidence, const QJsonObject& shape, const QJsonObject& cuts) {
    std::vector<MedialCandidate> candidates;
    const auto& ink = context.transformedPath;
    const auto policy = cuts.value("shaft").toObject();
    const auto maxAspect = shape.value("maxWidthToHeight").toDouble();
    const auto minRise = shape.value("minRise").toDouble();
    const auto forkWalk = cuts.value("forkOffset").toDouble();
    const auto walkWidths = policy.value("walkWidths").toDouble();
    const int count = int(baseContourCurves(ink).size());
    const int rightFlank = context.orientation == Orientation::ClockWise ? 0 : 1;
    const auto add = [&](const PathExtrema& crest, Stretching right, Stretching left,
                         const PathExtrema* fork = nullptr) {
        orientSection(context, right, crest.time, true);
        orientSection(context, left, crest.time, false);
        if (!cutPrecedes(right, left, context.orientation)) return;
        const QLineF entry(right.topBaseLine.point, right.bottomBaseLine.point);
        const QLineF exit(left.topBaseLine.point, left.bottomBaseLine.point);
        if (!sharesLowerForkTip(right, left) && entry.intersects(exit, nullptr) == QLineF::BoundedIntersection) return;
        const auto rightSide = getJoinTime(right, context.orientation);
        const auto leftSide = getJoinTime(left, context.orientation);
        if (!rightSide.isStrictLeft(crest.time) || !leftSide.isStrictRight(crest.time) ||
            (fork && (!rightSide.isLeft(fork->time) || !leftSide.isRight(fork->time)))) return;
        const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3 || toSubpaths(pieces[1].path).size() != 1) return;
        const auto bounds = pieces[1].path.boundingRect();
        if (bounds.width() > maxAspect * bounds.height() || std::abs(bounds.top() - crest.point.y()) > 1e-6) return;
        candidates.emplace_back(right, left, false, std::nullopt, crest,
                                fork ? std::optional{*fork} : std::nullopt);
    };

    for (const auto& stem : evidence.stems) {
        for (const auto index : stem.forks) {
            const auto& fork = evidence.features.forks[index];
            if (fork.tip.point.y() - stem.crest.point.y() < minRise) continue;
            const auto placeFork = [&](double distance) {
                const auto right = cutForkFlank(context, fork.tip, {}, fork.rightForward, 1, distance);
                const auto left = cutForkFlank(context, fork.tip, {}, !fork.rightForward, -1, distance);
                if (right && left) add(stem.crest, *right, *left, &fork.tip);
            };
            if (forkWalk > 0) placeFork(forkWalk);
            if (cuts.value("tightJoin") == "tip-normal") placeFork(0);
        }
    }
    for (const auto& shaft : evidence.turningShafts) {
        const auto normal = [&](int side, double distance) -> std::optional<Stretching> {
            auto anchor = *shaft.turns[side];
            const bool forward = side == 0;
            if (distance > 0) {
                auto limit = anchor;
                limit.time += forward ? count : -count;
                QPointF tangent;
                if (!advanceOnContour(ink, anchor, limit, forward, distance, anchor, tangent)) return {};
            }
            Stretching section{};
            if (!contourNormalCut(context, anchor, forward, false, section) ||
                section.referenceWidth < .4 * shaft.stroke || section.referenceWidth > 3.2 * shaft.stroke) return {};
            return section;
        };
        const double walk = walkWidths * shaft.stroke;
        const auto exit = normal(1 - rightFlank, walk);
        const auto entry = normal(rightFlank, walk);
        if (entry && exit) add(shaft.crest, *entry, *exit);
        if (walkWidths > 0 && policy.value("atTurnWhenTight").toBool()) {
            const auto right = normal(rightFlank, 0);
            const auto left = normal(1 - rightFlank, 0);
            if (right && left) add(shaft.crest, *right, *left);
        }
        if (!exit || !policy.value("forkJunction").toBool()) continue;
        const auto stem = std::ranges::find_if(evidence.stems, [&](const auto& stem) {
            return stem.crest.time == shaft.crest.time;
        });
        if (stem == evidence.stems.end()) continue;
        for (const auto index : stem->forks) {
            const auto& upper = *shaft.turns[rightFlank];
            const auto& lower = evidence.features.forks[index].tip;
            const auto width = QLineF(upper.point, lower.point).length();
            if (width < .4 * shaft.stroke || width > 3.2 * shaft.stroke || !interiorChord(ink, upper, lower)) continue;
            Stretching junction{};
            junction.topBaseLine = upper;
            junction.bottomBaseLine = lower;
            junction.referenceWidth = width;
            add(shaft.crest, junction, *exit, &lower);
        }
    }
    return candidates;
}

// Assembly owns all neighbor compatibility, contextual scale, priority and
// shared-attachment checks. Its input inventory is already recognized and
// remains unchanged when incoming/outgoing joins or cut policies change.
std::vector<MedialCandidate> computeStemBoundaries(SearchContext& context,
        const QJsonObject& config, const QJsonObject& form, const MedialStemEvidence& evidence,
        const Stretching& incoming,
        const QPointF& entry, const QPointF& exit, double stroke, bool open, bool recoverTeeth,
        bool allowFork, const std::optional<Stretching>& sharedExit) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto body = formDescription(config, form, "body");
    const auto cuts = formDescription(config, form, "cuts");
    std::vector<MedialCandidate> candidates;
    if (body.value("shape") == "turning-shaft") {
        candidates = turningShaftCuts(context, evidence, body.value("shaft").toObject(), cuts);
        if (inspectingForm) for (const auto& item : candidates) inspectCandidate("Turning shaft", item.left, item.right);
        if (candidates.empty()) inspectReason("Body / cuts", "No turning shaft passed the rise, width and section checks.");
        return candidates;
    }
    if (body.value("shape") != "forked-stem") return candidates;
    const auto placement = cuts.value("placement").toObject();
    const auto before = [&](const Stretching& a, const Stretching& b) {
        return cutPrecedes(a, b, context.orientation);
    };
    const auto center = [](const Stretching& cut) {
        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).center();
    };
    const auto shortExit = cuts.value("shortExit").toObject();
    const double minExitRun = shortExit.value("minHorizontalRun").toDouble();
    const double maxExitEntryGap = shortExit.value("maxEntryGap").toDouble();
    const double minExitLengthRatio = shortExit.value("minLengthRatio").toDouble();
    const double exitAnchorTolerance = shortExit.value("anchorTolerance").toDouble();
    const bool cutsAroundHollow = shortExit.value("cutsAroundHollow").toBool();
    const double maxEntrySlope = std::tan(qDegreesToRadians(shortExit.value("entryMaxTilt").toDouble(12)));
    const auto originalForm = context.form;
    const auto originalResult = context.res;
    const auto originalNext = context.nextIter;
    const auto routes = body.value("exitRoutes").toArray();
    const auto routeCuts = cuts.value("exitRoutes").toArray();
    const auto& bodyEvidence = evidence.features;
    // This is a placement gate, not the body recognizer: all stem candidates
    // above exist even when this join rejects their position or height.
    const auto fitsSlot = [&](const MedialStem& stem) {
        const auto& crest = stem.crest;
        const bool fits = crest.point.x() < entry.x() &&
            (open ? entry.y() : std::min(entry.y(), exit.y())) - crest.point.y() >=
                body.value("minRise").toDouble() * stroke &&
            (sharedExit || getJoinTime(incoming, context.orientation).isStrictLeft(crest.time));
        if (!fits) inspectReason("Assembly", "Recognized stem is outside this slot's position, height or contour-order limits.");
        if (cutGeometryMemo && cutGeometryMemo->activeForm) {
            ++cutGeometryMemo->activeForm->placementChecks;
            cutGeometryMemo->activeForm->placementRejected += !fits;
        }
        return fits;
    };
    double firstStemX = -std::numeric_limits<double>::infinity();
    std::vector<const MedialStem*> eligible;
    for (const auto& stem : evidence.stems) {
        if (!fitsSlot(stem)) continue;
        firstStemX = std::max(firstStemX, stem.crest.point.x());
        eligible.push_back(&stem);
    }
    // The rightmost crest is the usual starting point for a route. A low
    // inward fork can itself register as a crest to the right of its shaft.
    const auto routeStem = [&](const MedialStem& stem) {
        return sharedExit || stem.crest.point.x() >= firstStemX - 1e-6;
    };
    std::vector<MedialCandidate> forkCandidates;
    const double forkOffset = cuts.value("forkOffset").toDouble();
    const auto proposeForks = [&](bool useRouteStem) {
        if (!allowFork || forkOffset <= 0 || sharedExit || incoming.counterEntry.pathIndex > 0) return;
        const auto entrySide = getJoinTime(incoming, context.orientation);
        const auto& recognizedBodies = bodyEvidence.forks;
        for (const auto* selected : eligible) {
            const auto& stem = *selected;
            if (routeStem(stem) != useRouteStem) continue;
            const auto* crest = &stem.crest;
            for (const auto index : stem.forks) {
                const auto& fork = recognizedBodies[index];
                const auto* tip = &fork.tip;
                if (tip->point.y() - crest->point.y() < body.value("minRise").toDouble() * stroke ||
                    std::abs(tip->point.x() - crest->point.x()) > 3 * stroke ||
                    std::abs(tip->point.y() - entry.y()) > 4 * stroke ||
                    !entrySide.isStrictLeft(tip->time)) continue;
                const bool rightForward = fork.rightForward;
                // Small extrema can subdivide one flank. Walk through
                // them and let the body and join checks bound the result.
                for (double distance : {forkOffset, 0.}) {
                    if (distance == 0 && cuts.value("tightJoin") != "tip-normal") continue;
                    const auto right = cutForkFlank(context, *tip,
                        std::nullopt, rightForward, 1, distance);
                    const auto left = cutForkFlank(context, *tip, std::nullopt, !rightForward, -1, distance);
                    if (!right || !left || !before(incoming, *right) || !before(*right, *left) ||
                        std::min(right->referenceWidth, left->referenceWidth) <= 1e-6 ||
                        (distance == 0 && std::max(right->referenceWidth, left->referenceWidth) > 2.5 * stroke)) continue;
                    const auto parts = cutRegions(context.transformedPath, {}, {*right, *left}, context.orientation, true);
                    if (parts.size() != 3) continue;
                    const auto& mask = parts[1].path;
                    const auto bounds = mask.boundingRect();
                    if (std::abs(bounds.top() - crest->point.y()) > .01 ||
                        bounds.width() > body.value("maxWidthToHeight").toDouble() * bounds.height() ||
                        (form.contains("bodyCounters") &&
                         static_cast<int>(toSubpaths(mask).size()) - 1 != form.value("bodyCounters").toInt())) continue;
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
            if (body.value("entryRoute").isObject()) {

                for (bool reverse : {false, true}) {
                    context.res = {};
                    if (projectSavedRoute(context, stem.entryRoutes[reverse], body.value("entryRoute").toObject(),
                                          cuts.value("entryRoute").toObject()))
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
                    // The receiving form can supply a shared seam. The
                    // partition must keep this stem's crest and return.
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
                    if (!sharedExit && body.contains("maxWidthToHeight") && bodyBounds.width() >
                        body.value("maxWidthToHeight").toDouble() * bodyBounds.height()) continue;
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
                    if (!sharedExit && !shortExit.isEmpty()) {
                        const auto top = context.res.topBaseLine;
                        const auto bottom = context.res.bottomBaseLine;
                        const double entryBottomX = bodyEntry.bottomBaseLine.point.x();
                        // On a folded stem foot a normal can cross the whole bend
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
    // valid route around this stem and put its shaft in a join.
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
            return a.stemCrest->point.x() > b.stemCrest->point.x();
        });
    }
    candidates.insert(candidates.begin(), forkCandidates.begin(), forkCandidates.end());
    if (inspectingForm) for (const auto& item : candidates) inspectCandidate(item.stemFork ? "Fork flanks" : "Stem route", item.left, item.right);
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

std::vector<MedialCandidate> stemBoundaryCandidates(SearchContext& context,
        const QJsonObject& config, const QJsonObject& form, const MedialStemEvidence& evidence,
        const Stretching& incoming, const QPointF& entry, const QPointF& exit, double stroke,
        bool open, bool recoverTeeth, bool allowFork, const std::optional<Stretching>& sharedExit) {
    CandidateSearchScope measured(form);
    StemCandidateMemo* memo = nullptr;
    QByteArray key;
    if (context.outlineCache && !formInspection) {
        auto& saved = context.outlineCache->stemCandidates;
        if (!saved || saved->source != context.transformedPath ||
            saved->source.fillRule() != context.transformedPath.fillRule() || saved->orientation != context.orientation) {
            saved = std::make_shared<StemCandidateMemo>();
            saved->source = context.transformedPath;
            saved->orientation = context.orientation;
        }
        memo = saved.get();
        appendGeometryKey(key, memo->objectId(config));
        appendGeometryKey(key, memo->objectId(form));
        if (form.value("body").toObject().value("shape") != "turning-shaft") {
            appendCandidateBoundary(key, incoming);
            appendGeometryPoint(key, entry); appendGeometryPoint(key, exit);
            appendGeometryKey(key, stroke); appendGeometryKey(key, context.pairedStrokeWidth);
            appendGeometryKey(key, open); appendGeometryKey(key, recoverTeeth); appendGeometryKey(key, allowFork);
            appendGeometryKey(key, bool(sharedExit));
            if (sharedExit) appendCandidateBoundary(key, *sharedExit);
        }
        if (const auto* hit = memo->candidates.object(key)) return *hit;
    }
    if (measured.stats) {
        ++measured.stats->builds;
        measured.stats->bodies += evidence.stems.size() + evidence.turningShafts.size();
    }
    auto result = computeStemBoundaries(context, config, form, evidence, incoming, entry, exit, stroke,
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
