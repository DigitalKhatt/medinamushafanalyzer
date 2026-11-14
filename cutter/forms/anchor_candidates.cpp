#include "../detail.h"

namespace MedinaCutter {

static std::optional<Stretching> initialFootExit(const SearchContext& source, const Stretching& section,
                                                double penWidth) {
    const auto axis = section.bottomBaseLine.point - section.topBaseLine.point;
    if (std::abs(axis.y()) >= std::abs(axis.x()) || QLineF(QPointF{}, axis).length() >= .75 * penWidth)
        return {};
    const auto side = getJoinTime(section, source.orientation);
    const auto& bend = section.topBaseLine;
    const bool forward = side.startAtLeft != (bend.time == side.minTime);
    auto limit = bend;
    limit.time += (forward ? 1 : -1) * int(baseContourCurves(source.transformedPath).size());
    // The inner bend can be vertical at the landmark itself. A short
    // walk exposes the lower edge of the foot to the same downward probe.
    for (const double walk : {0., .25, .5}) {
        auto top = bend;
        QPointF tangent;
        if (walk > 0 && !advanceOnContour(source.transformedPath, bend, limit, forward, walk * penWidth, top, tangent))
            continue;
        const QLineF ray(top.point, {top.point.x(), source.bbox.bottom() + 1});
        const auto hits = FindInterections(source.transformedPath, ray, true);
        for (const auto& bottom : hits) {
            if (bottom.point.y() <= top.point.y() + 1e-5) continue;
            if (bottom.point.y() - top.point.y() < .75 * penWidth ||
                bottom.point.y() - top.point.y() > 2 * penWidth ||
                !interiorChord(source.transformedPath, top, bottom)) break;
            auto cut = section;
            cut.topBaseLine = top;
            cut.bottomBaseLine = bottom;
            cut.referenceWidth = bottom.point.y() - top.point.y();
            return cut;
        }
    }
    return {};
}

QString compileAnchorForms(const QJsonObject& config, AnchorForms& forms) {
    forms = {};
    AnchorForms compiled;
    const std::array positions{"initial", "medial", "final"};
    for (size_t position = 0; position < positions.size(); ++position) {
        for (const auto& value : config.value(positions[position]).toArray()) {
            const auto declaration = value.toObject();
            const auto enabled = declaration.value("fixedAnchors");
            const auto id = declaration.value("id").toString();
            if (enabled.isUndefined()) continue;
            if (!enabled.isBool()) return id + ": fixedAnchors must be a boolean";
            if (!enabled.toBool()) continue;
            const auto maxLetters = declaration.value("fixedAnchorsMaxLetters");
            if (!maxLetters.isUndefined() && maxLetters.toInt() < 2)
                return id + ": fixedAnchorsMaxLetters must be an integer of at least two";
            const auto form = bodyConstraintForm(config, declaration);
            const auto body = form.value("body").toObject();
            const auto shape = body.value("shape");
            const bool supported = position == 0 ? shape == "contour-route" || shape == "horizontal-counters" ||
                shape == "suspended-loop" || shape == "counter-neck" || shape == "counter-loop" ||
                shape == "profile-neck" :
                position == 1 ? toothDescription(body) || shape == "stacked-counters" || shape == "counter-loop" ||
                shape == "open-head" || shape == "crested-loop" || shape == "paired-route" ||
                shape == "forked-stem" || (shape == "tooth-cluster" &&
                    form.value("cuts").toObject().value("cluster").toObject().value("entryAtJunction").toBool() &&
                    form.value("cuts").toObject().value("cluster").toObject().value("exitAtHollow").toBool()) :
                shape == "stemmed-bowl" || shape == "tail-branch" || shape == "contour-route" ||
                shape == "counter-neck" || shape == "counter-shoulder" || shape == "bowl-turn" ||
                shape == "upright-stem" || shape == "open-bowl";
            if (!supported)
                return id + ": this body does not supply fixed anchors for its position";
            for (const auto letter : form.value("current").toString())
                compiled[position][letter].push_back(form);
        }
    }
    forms = std::move(compiled);
    return {};
}

std::vector<AnchoredBody> anchoredBodies(SearchContext& source, const QJsonObject& form, size_t position, double penWidth) {
    const auto id = form.value("id").toString();
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto shape = body.value("shape");
    std::vector<AnchoredBody> result;
    InspectFormScope inspection(form, source.transformedPath);
    source.res = {};
    source.nextIter = std::next(source.xSorted.begin());
    if (shape == "contour-route") {
        const bool initial = position == 0;
        if (declaredSharedBoundary(source, form) &&
            matchesOffset(form.value("chord").toObject(), source.res.bottomBaseLine.point - source.res.topBaseLine.point) &&
            matchesBoundaryBody(source, form, source.res, initial)) {
            if (form.contains("bodyCounters")) {
                const auto pieces = cutRegions(source.transformedPath, {}, {source.res}, source.orientation, true);
                if (pieces.size() != 2 || toSubpaths(pieces[initial ? 0 : 1].path).size() - 1 !=
                    form.value("bodyCounters").toInt()) return result;
            }
            if (initial) {
                const auto projected = source.res;
                const QLineF original(projected.topBaseLine.point, projected.bottomBaseLine.point);
                if (cuts.value("preferFootExit").toBool()) {
                    if (const auto foot = initialFootExit(source, projected, penWidth);
                        foot && matchesBoundaryBody(source, form, *foot, true))
                        result.push_back({.formId = id, .exit = foot, .landmarks = {*source.xSorted.begin()},
                                          .footExitCandidate = true});
                }
                if (cuts.value("preferFlankExit").toBool()) {
                    for (const auto& exit : declaredFlankExits(source, body, cuts, penWidth)) {
                        const QLineF shorter(exit.topBaseLine.point, exit.bottomBaseLine.point);
                        // Use a flank exit to reclaim a connector. Sub-stroke
                        // shifts at the foot belong to the contour route.
                        if (shorter.center().x() - original.center().x() < shorter.length() ||
                            !cutPrecedes(exit, projected, source.orientation) ||
                            !matchesBoundaryBody(source, form, exit, true)) continue;
                        result.push_back({.formId = id, .exit = exit, .landmarks = {*source.xSorted.begin()}});
                    }
                }
                result.push_back({.formId = id, .exit = projected, .landmarks = {*source.xSorted.begin()}});
            } else
                result.push_back({.formId = id, .entry = source.res, .landmarks = {*source.xSorted.rbegin()}});
        }
    } else if (shape == "horizontal-counters" || shape == "suspended-loop" ||
               (position == 0 && shape == "counter-neck")) {
        const auto evidence = recognizedSharedShape(source, body);
        QVector<size_t> counters;
        if (evidence->horizontalPair) {
            const auto& pair = *evidence->horizontalPair;
            counters = {pair.right.index, pair.left.index};
        } else if (evidence->head) {
            counters = {evidence->head->counter.index};
        } else if (evidence->initialNeck) {
            counters = {evidence->initialNeck->counter.index};
        }
        if (!counters.isEmpty() && declaredSharedBoundary(source, form) &&
            matchesBoundaryBody(source, form, source.res, true)) {
            result.push_back({.formId = id, .exit = source.res, .landmarks = {*source.xSorted.begin()},
                              .counters = std::move(counters)});
        }
    } else if (shape == "stacked-counters") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = cuts.value("pair").toObject();
        for (const auto& pair : evidence->stackedPairs)
            if (const auto sections = cutStackedPair(source, pair, policy))
                result.push_back({.formId = id, .entry = sections->first, .exit = sections->second,
                                  .counters = {pair.upper.index, pair.lower.index}});
    } else if (shape == "counter-loop") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto settings = shapeSettings(form, "loop");
        const auto rimPolicy = cuts.value("entryRim").toObject();
        auto loops = evidence->loops;
        std::stable_sort(loops.begin(), loops.end(), [](const auto& a, const auto& b) {
            return a.anchor.box.center().x() > b.anchor.box.center().x();
        });
        for (const auto& loop : loops) {
            if (settings.value("counterPair") == "vertical" && !loop.stackedFeet) continue;
            const auto sections = cutLoopBody(source, loop, settings, position == 1);
            if (!sections) continue;
            QVector<size_t> counters{loop.anchor.index};
            if (loop.horizontalPartner) counters.append(*loop.horizontalPartner);
            if (loop.stackedFeet) counters = {loop.stackedFeet->upper.index, loop.stackedFeet->lower.index};
            if (position == 1 && counters.size() == 1) {
                const auto& entry = sections->first;
                const QLineF chord(entry.topBaseLine.point, entry.bottomBaseLine.point);
                if (needsCounterRim(chord, rimPolicy, penWidth)) {
                    for (const auto& rim : counterRimEntryCuts(source, loop.anchor.box, entry))
                        result.push_back({.formId = id, .entry = rim, .exit = sections->second,
                                          .counters = counters, .retainsCounterRim = true,
                                          .sharesRimEntry = form.value("tightJoin").toBool()});
                }
            }
            result.push_back({.formId = id,
                              .entry = position == 1 ? std::optional{sections->first} : std::nullopt,
                              .exit = sections->second,
                              .counters = std::move(counters)});
        }
    } else if (position == 0 && shape == "profile-neck") {
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, true))
            result.push_back({.formId = id, .exit = source.res, .landmarks = {*source.xSorted.begin()}});
    } else if (shape == "open-head" || shape == "crested-loop") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = cuts.value("head").toObject();
        if (shape == "crested-loop") {
            for (const auto& head : evidence->crestedHeads)
                for (const auto& [entry, exit] : cutCrestedLoops(source, {head}, policy))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .landmarks = {head.crest}, .counters = {head.counter.index}});
        } else {
            for (const auto& head : evidence->openHeads)
                for (const auto& [entry, exit] : cutOpenHeads(source, {head}, policy))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .landmarks = {head.crest, head.rightEdge, head.leftEdge}});
        }
        std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return Stretching::cmpbyx{}(*a.entry, *b.entry);
        });
    } else if (shape == "forked-stem") {
        auto forkBody = body;
        forkBody.remove("entryRoute");
        forkBody.remove("exitRoutes");
        const auto evidence = recognizedSharedShape(source, forkBody);
        const double offset = cuts.value("forkOffset").toDouble();
        if (!evidence->medialStem || offset <= 0) return result;
        const auto& stems = *evidence->medialStem;
        for (const auto& stem : stems.stems) {
            for (const auto index : stem.forks) {
                const auto& fork = stems.features.forks[index];
                for (const double walk : {offset, 0.}) {
                    if (walk == 0 && cuts.value("tightJoin") != "tip-normal") continue;
                    const auto right = cutForkFlank(source, fork.tip, {}, fork.rightForward, 1, walk);
                    const auto left = cutForkFlank(source, fork.tip, {}, !fork.rightForward, -1, walk);
                    if (!right || !left) continue;
                    const auto pieces = cutRegions(source.transformedPath, {}, {*right, *left}, source.orientation, true);
                    if (pieces.size() != 3) continue;
                    const auto& mask = pieces[1].path;
                    const auto bounds = mask.boundingRect();
                    const double stroke = std::min(right->referenceWidth, left->referenceWidth);
                    if (stroke <= 1e-6 || std::abs(bounds.top() - stem.crest.point.y()) > .01 ||
                        fork.tip.point.y() - stem.crest.point.y() < body.value("minRise").toDouble() * stroke ||
                        bounds.width() > body.value("maxWidthToHeight").toDouble() * bounds.height() ||
                        (body.contains("counters") && toSubpaths(mask).size() - 1 !=
                            body.value("counters").toInt())) continue;
                    result.push_back({.formId = id, .entry = *right, .exit = *left, .landmarks = {stem.crest},
                                      .preservesTerminalBody = true,
                                      .sharedFold = cuts.value("sharedFold").toObject()});
                }
            }
        }
    } else if (shape == "paired-route") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto route = body.value("routes").toObject();
        const auto policy = cuts.value("routes").toObject();
        for (const auto& candidate : evidence->pairedRoutes) {
            for (const auto& [entry, exit] : cutPairedRoutes(source, {candidate}, route, policy)) {
                if (form.contains("bodyCounters")) {
                    const auto pieces = cutRegions(source.transformedPath, {}, {entry, exit}, source.orientation, true);
                    if (pieces.size() != 3 || toSubpaths(pieces[1].path).size() - 1 !=
                        form.value("bodyCounters").toInt()) continue;
                }
                result.push_back({.formId = id, .entry = entry, .exit = exit, .landmarks = {candidate.anchor},
                                  .sharesExitAtCorners = cuts.value("sharedExit") == "receiving-corners"});
            }
        }
    } else if (shape == "tooth-cluster") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = cuts.value("cluster").toObject();
        for (const auto& cluster : evidence->clusters)
            if (const auto sections = medialClusterAnchors(source, cluster, evidence->clusters,
                                                          body.value("cluster").toObject(), policy, penWidth))
                result.push_back({.formId = id, .entry = sections->first, .exit = sections->second,
                                  .landmarks = {cluster.turns.begin(), cluster.turns.end()},
                                  .sharesJunctionEntry = form.value("sharedEntry").toBool(),
                                  .keepsEntryJunction = true, .keepsExitHollow = true});
        std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.landmarks.front().point.x() > b.landmarks.front().point.x();
        });
    } else if (toothDescription(body)) {
        auto teeth = declaredToothBoundaries(source, form);
        // Keep the established reading order among placements of one form.
        std::stable_sort(teeth.begin(), teeth.end(), [](const auto& a, const auto& b) {
            return a.tip.point.x() > b.tip.point.x();
        });
        for (const auto& tooth : teeth) {
            if (!tooth.right) continue;
            const QLineF axis(tooth.tip.point, tooth.bend.point);
            result.push_back({id, tooth.right, tooth.left, {tooth.tip, tooth.bend},
                tooth.curvedAxis.isEmpty() ? QVector<QPointF>{axis.pointAt(.05), axis.pointAt(.95)} : tooth.curvedAxis});
            result.back().sharesIncoming = cuts.value("tightJoin") == "shared-boundary";
        }
    } else if (shape == "upright-stem") {
        const auto evidence = recognizedSharedShape(source, body);
        if (evidence->uprightStem && declaredSharedBoundary(source, form)) {
            auto entry = source.res;
            if (cuts.value("turnBoundaries").toObject().value("right") == "stem") {
                const QLineF cap(entry.topBaseLine.point, entry.bottomBaseLine.point);
                if (const auto foot = uprightStemFoot(source, entry, penWidth)) {
                    entry = *foot;
                } else if (std::abs(cap.dy()) <= .05 * cap.length() && evidence->descendingShaft &&
                           cap.length() > penWidth) {
                    Stretching lower{};
                    if (cutDescendingShaft(source, *evidence->descendingShaft, cuts.value("stem").toObject(), lower) &&
                        cutPrecedes(lower, entry, source.orientation))
                        result.push_back({.formId = id, .entry = lower, .landmarks = {evidence->uprightStem->tip},
                                          .stemFootCandidate = true});
                }
            }
            result.push_back({.formId = id, .entry = entry, .landmarks = {evidence->uprightStem->tip}});
        }
    } else if (shape == "stemmed-bowl") {
        const auto evidence = recognizedSharedShape(source, body);
        auto stems = evidence->terminalStems;
        std::stable_sort(stems.begin(), stems.end(), [](const auto& a, const auto& b) {
            return a.crest.point.x() < b.crest.point.x();
        });
        const auto settings = shapeSettings(form, "stem"), policy = cuts.value("stem").toObject();
        for (const auto& stem : stems) {
            if (!cutTerminalStemBodies(source, {stem}, settings, policy)) continue;
            result.push_back({.formId = id, .entry = source.res, .landmarks = {stem.crest, stem.bowlEdge},
                .samples = {stem.sample, stem.bowlBottom - QPointF(0, .01 * stem.stroke)}});
        }
    } else if (shape == "tail-branch") {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->tail) return result;
        const auto& tail = *evidence->tail;
        const auto settings = shapeSettings(form, "tail");
        for (const auto& branch : tail.branches) {
            Stretching entry{};
            if (!cutTailBranches(source, {tail.tip, tail.curveCount, {branch}}, settings, entry)) continue;
            result.push_back({.formId = id, .entry = entry, .landmarks = {tail.tip, branch.corner}});
        }
    } else if (shape == "open-bowl") {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->openBowl || !cutOpenBowl(source, *evidence->openBowl, cuts.value("bowl").toObject()))
            return result;
        const auto& bowl = *evidence->openBowl;
        const auto junction = cuts.value("junction").toObject();
        auto entry = source.res;
        if (!junction.isEmpty()) {
            // The rounded bowl turn and its short corner can occupy nearly
            // the same spot. Use the full contour inventory to refine the cut.
            SearchContext outline{.transformedPath = source.transformedPath, .res = entry,
                                  .subword = source.subword};
            if (readTerminalOutline(outline) && refineContourJunction(outline, junction) &&
                retainsOpenBowl(source, bowl, outline.res))
                entry = outline.res;
        }
        if (!matchesBoundaryBody(source, form, entry, false)) return result;
        QVector<PathExtrema> landmarks{bowl.tip, bowl.floor};
        if (bowl.crest) landmarks.append(*bowl.crest);
        result.push_back({.formId = id, .entry = entry, .landmarks = std::move(landmarks)});
    } else if (shape == "bowl-turn") {
        const auto evidence = recognizedSharedShape(source, body);
        if (evidence->bowlTurn && declaredSharedBoundary(source, form) &&
            matchesBoundaryBody(source, form, source.res, false))
            result.push_back({.formId = id, .entry = source.res, .landmarks = {evidence->bowlTurn->tip}});
    } else if (shape == "counter-neck" || shape == "counter-shoulder") {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->boundaryCounter || !declaredSharedBoundary(source, form)) return result;
        const auto& counter = *evidence->boundaryCounter;
        const auto entry = source.res;
        const auto rim = cuts.value("entryRim").toObject();
        const QLineF section(entry.topBaseLine.point, entry.bottomBaseLine.point);
        const auto add = [&](const Stretching& boundary, bool retainsRim) {
            if (matchesBoundaryBody(source, form, boundary, false))
                result.push_back({.formId = id, .entry = boundary, .landmarks = {*source.xSorted.rbegin()},
                                  .counters = {counter.index}, .retainsCounterRim = retainsRim,
                                  .sharesRimEntry = retainsRim && form.value("tightJoin").toBool(),
                                  .tightEntryAfter = cuts.value("tightEntryAfter").toString()});
        };
        if (needsCounterRim(section, rim, penWidth))
            for (const auto& boundary : counterRimEntryCuts(source, counter.box, entry))
                add(boundary, true);
        if (result.empty()) add(entry, false);
    }
    if (inspectingForm) for (const auto& candidate : result) {
        for (const auto& mark : candidate.landmarks) inspectLandmark("Body landmark", mark);
        if (candidate.entry) inspectCandidate("Entry anchor", *candidate.entry);
        if (candidate.exit) inspectCandidate("Exit anchor", *candidate.exit);
    }
    return result;
}

} // namespace MedinaCutter
