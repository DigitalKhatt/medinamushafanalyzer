#include "../detail.h"

namespace MedinaCutter {

static int anchorPriority(const QJsonObject& form) {
    if (form.value("onUnresolved").toBool()) return 2;
    return form.value("onIncomplete").toBool() ? 1 : 0;
}

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
    QHash<QChar, QString> foldedToothPreceders;
    for (const auto& value : config.value("medial").toArray()) {
        const auto form = value.toObject();
        const auto finals = form.value("sharedFoldWithFinal").toString();
        if (finals.isEmpty() || formDescription(config, form, "body").value("shape") != "tooth-cluster") continue;
        for (const auto letter : finals)
            foldedToothPreceders[letter] += form.value("current").toString();
    }
    const std::array positions{"initial", "medial", "final"};
    for (size_t position = 0; position < positions.size(); ++position) {
        for (const auto& value : config.value(positions[position]).toArray()) {
            const auto declaration = value.toObject();
            const auto enabled = declaration.value("fixedAnchors");
            const auto id = declaration.value("id").toString();
            const auto anchorOnly = declaration.value("fixedAnchorsOnly");
            if (!anchorOnly.isUndefined() && (!anchorOnly.isBool() || (anchorOnly.toBool() && !enabled.toBool())))
                return id + ": fixedAnchorsOnly requires an enabled fixed-anchor form";
            if (enabled.isUndefined()) continue;
            if (!enabled.isBool()) return id + ": fixedAnchors must be a boolean";
            if (!enabled.toBool()) continue;
            const auto maxLetters = declaration.value("fixedAnchorsMaxLetters");
            if (!maxLetters.isUndefined() && maxLetters.toInt() < 2)
                return id + ": fixedAnchorsMaxLetters must be an integer of at least two";
            const auto ending = declaration.value("fixedAnchorsEnding");
            if (!ending.isUndefined() && (position == 2 || !ending.isString() || ending.toString().isEmpty()))
                return id + ": fixedAnchorsEnding needs final letters on an initial or medial form";
            const auto tatweel = declaration.value("fixedAnchorsTatweel");
            if (!tatweel.isUndefined() && (position != 2 || !tatweel.isBool()))
                return id + ": fixedAnchorsTatweel must be a boolean on a final form";
            auto form = bodyConstraintForm(config, declaration);
            const auto body = form.value("body").toObject();
            const auto shape = body.value("shape");
            const bool supported = position == 0 ? shape == "contour-route" || shape == "horizontal-counters" ||
                shape == "suspended-loop" || shape == "counter-neck" || shape == "counter-loop" ||
                shape == "profile-neck" || shape == "tooth-cluster" || shape == "layered-head" ||
                shape == "enclosed-lips" || shape == "descending-stem" || shape == "stacked-tooth" :
                position == 1 ? toothDescription(body) || shape == "stacked-counters" || shape == "counter-loop" ||
                shape == "open-head" || shape == "crested-loop" || shape == "paired-route" ||
                shape == "forked-stem" || shape == "turning-shaft" || shape == "returning-lips" ||
                shape == "enclosed-lips" || shape == "inclined-blade" || shape == "parallel-return" ||
                (shape == "tooth-cluster" &&
                    form.value("cuts").toObject().value("cluster").toObject().value("entryAtJunction").toBool() &&
                    form.value("cuts").toObject().value("cluster").toObject().value("exitAtHollow").toBool()) :
                shape == "stemmed-bowl" || shape == "tail-branch" || shape == "contour-route" ||
                shape == "counter-neck" || shape == "counter-shoulder" || shape == "bowl-turn" ||
                shape == "upright-stem" || shape == "open-bowl";
            if (!supported)
                return id + ": this body does not supply fixed anchors for its position";
            if (shape == "stacked-tooth") {
                const auto partner = body.value("attachment").toObject().value("template").toString();
                form.insert("_upperAttachment", namedLetterForm(config, partner).value("upperAttachment"));
            } else if (shape == "paired-route" && !form.contains("upperAttachment")) {
                const auto inherit = declaration.value("body").toObject().value("inherit").toString();
                if (!inherit.isEmpty()) {
                    const auto parent = namedLetterForm(config, inherit);
                    form.insert("_sharedAttachment", parent.value("upperAttachment"));
                }
            }
            for (const auto letter : form.value("current").toString()) {
                auto& entry = compiled[position][letter].emplace_back(form);
                if (position == 2 && foldedToothPreceders.contains(letter))
                    entry.insert("_foldedToothPreceders", foldedToothPreceders.value(letter));
            }
        }
    }
    for (auto& position : compiled)
        for (auto& candidates : position)
            std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                return anchorPriority(a) < anchorPriority(b);
            });
    forms = std::move(compiled);
    return {};
}

std::vector<AnchoredBody> anchoredBodies(SearchContext& source, const QJsonObject& form, size_t position,
                                        double penWidth) {
    const auto id = form.value("id").toString();
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto shape = body.value("shape");
    std::vector<AnchoredBody> result;
    InspectFormScope inspection(form, source.transformedPath);
    source.res = {};
    source.nextIter = std::next(source.xSorted.begin());
    const auto turn = cuts.value("route").toObject().value("descendingTurn");
    if (shape == "contour-route" && position == 2 && turn.isDouble()) {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->route) return result;
        const auto& route = *evidence->route;
        const auto policy = cuts.value("route").toObject();
        for (const auto& [cut, fork] : descendingRouteEntries(source, route, policy,
                evidence->tail ? &*evidence->tail : nullptr)) {
            inspectLandmark("Entry limit at fork", fork);
            if (matchesBoundaryBody(source, form, cut, false))
                result.push_back({.formId = id, .entry = cut, .landmarks = {route.origin}, .entryLimit = fork});
        }
    } else if (shape == "stacked-tooth") {
        const auto attachment = body.value("attachment").toObject();
        const auto settings = form.value("_upperAttachment").toObject();
        for (auto seam : upperAttachmentSections(source, settings)) {
            const QLineF section(seam.topBaseLine.point, seam.bottomBaseLine.point);
            // Find the crest of the upper piece. Another letter's
            // ascender can be taller than the stacked tooth.
            const double maxCrestY = section.y1() - attachment.value("minRise").toDouble();
            for (auto it = source.ySorted.rbegin(); it != source.ySorted.rend(); ++it) {
                const auto& crest = *it;
                if (crest.point.y() > maxCrestY) break;
                if (crest.point.x() < std::min(section.x1(), section.x2())) continue;
                orientSection(source, seam, crest.time, false);
                const auto pieces = cutRegions(source.transformedPath, {}, {seam}, source.orientation, true);
                if (pieces.size() != 2) continue;
                const auto& stroke = pieces.front().path;
                const auto bounds = stroke.boundingRect();
                if (toSubpaths(stroke).size() != 1 || crest.point.y() - bounds.top() > 1.5 * penWidth ||
                    std::abs(bounds.bottom() - section.center().y()) > 1e-5 ||
                    bounds.width() > attachment.value("maxWidthToHeight").toDouble() * bounds.height()) continue;
                // The upper pen shape can double back. Check the descending
                // arm near the attachment, below that turn.
                const bool preferred = it == source.ySorted.rbegin() &&
                    std::abs(bounds.top() - crest.point.y()) <= 1e-5 &&
                    singleHorizontalStroke(stroke, bounds, bounds.top() + .85 * bounds.height());
                if (!preferred && (!singleHorizontalStroke(stroke, bounds, bounds.top() + .90 * bounds.height()) ||
                    !singleHorizontalStroke(stroke, bounds, bounds.top() + .95 * bounds.height()))) continue;
                result.push_back({.formId = id, .exit = seam, .landmarks = {crest},
                                  .requiresSharedExit = true, .alternateAnchor = !preferred});
                break;
            }
        }
    } else if (shape == "contour-route") {
        const bool initial = position == 0;
        if (declaredSharedBoundary(source, form) &&
            matchesOffset(form.value("chord").toObject(), source.res.bottomBaseLine.point - source.res.topBaseLine.point) &&
            matchesBoundaryBody(source, form, source.res, initial)) {
            QVector<size_t> counters;
            if (form.contains("bodyCounters")) {
                const auto pieces = cutRegions(source.transformedPath, {}, {source.res}, source.orientation, true);
                if (pieces.size() != 2) return result;
                const auto contours = toSubpaths(pieces[initial ? 0 : 1].path);
                if (contours.size() - 1 != form.value("bodyCounters").toInt()) return result;
                for (size_t i = 1; i < source.subPaths.size(); ++i)
                    if (contours.contains(source.subPaths[i])) counters.append(i);
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
                const auto start = body.value("route").toObject().value("start");
                const auto evidence = recognizedSharedShape(source, body);
                const auto defaultOrigin = start == "top" ? *source.ySorted.rbegin() : *source.xSorted.begin();
                const auto origin = start == "lower-right" && evidence->route ? evidence->route->origin : defaultOrigin;
                const auto region = getJoinTime(projected, source.orientation);
                const auto ownedCrest = cuts.value("useOwnedCrest");
                if (!region.isStrictRight(origin.time) && (ownedCrest.toBool() || ownedCrest.isString())) {
                    const auto crest = std::find_if(source.ySorted.rbegin(), source.ySorted.rend(),
                        [&](const auto& landmark) { return region.isStrictRight(landmark.time); });
                    if (crest != source.ySorted.rend())
                        result.push_back({.formId = id, .exit = projected, .landmarks = {*crest},
                                          .counters = counters, .alternateAnchor = true});
                }
                result.push_back({.formId = id, .exit = projected, .landmarks = {origin},
                                  .counters = std::move(counters)});
            } else
                result.push_back({.formId = id, .entry = source.res, .landmarks = {*source.xSorted.rbegin()}});
        }
    } else if (position == 0 && (shape == "layered-head" || shape == "enclosed-lips")) {
        const auto evidence = recognizedSharedShape(source, body);
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, true)) {
            const auto exit = source.res;
            const auto pieces = cutRegions(source.transformedPath, {}, {exit}, source.orientation, true);
            if (pieces.size() != 2) return result;
            const auto contours = toSubpaths(pieces.front().path);
            QVector<size_t> counters;
            for (size_t i = 1; i < source.subPaths.size(); ++i)
                if (contours.contains(source.subPaths[i])) counters.append(i);
            if (counters.isEmpty() || (form.contains("bodyCounters") &&
                counters.size() != form.value("bodyCounters").toInt())) return result;
            if (evidence->layeredHead && evidence->layeredHead->exitCounter &&
                std::none_of(counters.begin(), counters.end(), [&](size_t i) {
                    return source.subPaths[i].boundingRect() == *evidence->layeredHead->exitCounter;
                })) return result;
            result.push_back({.formId = id, .exit = exit, .landmarks = {*source.xSorted.begin()},
                              .counters = std::move(counters)});
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
    } else if (position == 0 && (shape == "profile-neck" || shape == "descending-stem")) {
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, true))
            result.push_back({.formId = id, .exit = source.res, .landmarks = {
                shape == "descending-stem" ? *source.ySorted.rbegin() : *source.xSorted.begin()}});
    } else if (shape == "returning-lips" || shape == "enclosed-lips" ||
               shape == "inclined-blade" || shape == "parallel-return") {
        const auto evidence = recognizedSharedShape(source, body);
        if (shape == "returning-lips") {
            for (const auto& head : evidence->mouths)
                for (const auto& [entry, exit] : cutOpenMouths(source, {head}, shapeSettings(form, "mouth"))) {
                    // On stacked ink a nose candidate can belong to the
                    // upper stroke. The notch and upper lip define the mouth.
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .landmarks = {head.lips.notch, head.lips.upper},
                                      .usesSharedEntry = form.value("sharedEntry").toBool() ||
                                                         form.value("reclaimEntry").toBool()});
                }
        } else if (shape == "enclosed-lips") {
            for (const auto& head : evidence->enclosedLips)
                for (const auto& [entry, exit] : cutEnclosedLips(source, {head}, cuts.value("head").toObject(), false))
                    for (size_t i = 1; i < source.subPaths.size(); ++i)
                        if (source.subPaths[i].boundingRect() == head.counter)
                            result.push_back({.formId = id, .entry = entry, .exit = exit,
                                              .landmarks = {head.nose}, .counters = {i}});
        } else if (shape == "inclined-blade") {
            for (const auto& blade : evidence->blades)
                for (const auto& [entry, exit] : cutBlades(source, {blade}, cuts.value("blade").toObject()))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .landmarks = {blade.elbow, blade.tip}});
        } else {
            for (const auto& fold : evidence->parallelReturns)
                for (const auto& [entry, exit] : cutParallelReturns(source, {fold}, cuts.value("bands").toObject()))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                        .samples = {fold.first.first.center(), fold.first.second.center(),
                                    fold.second.first.center(), fold.second.second.center()}});
        }
    } else if (shape == "turning-shaft") {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->medialStem) return result;
        for (const auto& candidate : turningShaftCuts(source, *evidence->medialStem, body.value("shaft").toObject(), cuts))
            result.push_back({.formId = id, .entry = candidate.right, .exit = candidate.left,
                              .landmarks = {*candidate.stemCrest},
                              .sharedEntryFrom = cuts.value("sharedEntryCorner").toObject().value("initialTemplate").toString(),
                              .preservesTerminalBody = true,
                              .entryLimit = candidate.stemFork});
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
        const bool atUpperAttachment = form.contains("upperAttachment");
        const auto attachmentShape = (atUpperAttachment ? form.value("upperAttachment")
                                                       : form.value("_sharedAttachment")).toObject();
        const auto sharedSections = upperAttachmentSections(source, attachmentShape);
        for (const auto& candidate : evidence->pairedRoutes) {
            for (const auto& [entry, exit] : cutPairedRoutes(source, {candidate}, route, policy)) {
                // The attachment corner must connect the head flank to
                // the rising stroke. The mouth can keep the same exit
                // even when its original entry walk stops above this junction.
                std::vector<Stretching> entries{entry};
                if (atUpperAttachment) entries.clear();
                for (const auto& seam : sharedSections)
                    if (getJoinTime(seam, source.orientation).isStrictLeft(candidate.anchor.time) &&
                        cutPrecedes(seam, exit, source.orientation)) entries.push_back(seam);
                for (const auto& section : entries) {
                    const bool movedEntry = section.topBaseLine.time != entry.topBaseLine.time ||
                                            section.bottomBaseLine.time != entry.bottomBaseLine.time;
                    if (form.contains("bodyCounters")) {
                        const auto pieces = cutRegions(source.transformedPath, {}, {section, exit},
                                                       source.orientation, true);
                        if (pieces.size() != 3 || toSubpaths(pieces[1].path).size() - 1 !=
                            form.value("bodyCounters").toInt()) continue;
                    }
                    result.push_back({.formId = id, .entry = section, .exit = exit, .landmarks = {candidate.anchor},
                                      .sharesExitAtCorners = cuts.value("sharedExit") == "receiving-corners",
                                      .usesSharedEntry = form.value("sharedEntry").toBool(),
                                      .requiresSharedEntry = atUpperAttachment || movedEntry,
                                      .requiresSharedSource = !atUpperAttachment && movedEntry});
                }
            }
        }
    } else if (shape == "tooth-cluster") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = cuts.value("cluster").toObject();
        const auto settings = toothClusterSettings(form);
        for (const auto& cluster : evidence->clusters) {
            if (position == 0) {
                if (policy.value("exitAtHollow").toBool())
                    if (const auto exit = clusterExitAtHollow(source, cluster, penWidth, 2 * penWidth, policy))
                        result.push_back({.formId = id, .exit = exit,
                                          .landmarks = {cluster.turns.begin(), cluster.turns.end()}});
                const auto add = [&](const SeenExitRetry* retry) {
                    Stretching exit{};
                    if (cutToothClusters(source, {cluster}, settings, source.bbox.left(), source.bbox.right() + 1e-6,
                                         nullptr, &exit, nullptr, nullptr, retry))
                        result.push_back({.formId = id, .exit = exit,
                                          .landmarks = {cluster.turns.begin(), cluster.turns.end()}});
                };
                add(nullptr);
                const SeenExitRetry bend{0, SeenExitMode::AlongContour};
                if (bend.supports(settings)) add(&bend);
            } else {
                const auto geometry = body.value("cluster").toObject();
                auto primary = policy;
                primary.remove("exitAtFlank");
                auto sections = medialClusterAnchors(source, cluster, evidence->clusters, geometry, primary, penWidth);
                const bool preferred = sections.has_value();
                if (!sections && policy.value("exitAtFlank").toBool())
                    sections = medialClusterAnchors(source, cluster, evidence->clusters, geometry, policy, penWidth);
                if (!sections) continue;
                result.push_back({.formId = id, .entry = sections->first, .exit = sections->second,
                                  .landmarks = {cluster.turns.begin(), cluster.turns.end()},
                                  .sharesJunctionEntry = form.value("sharedEntry").toBool(),
                                  .sharesLoopNeck = policy.value("shareLoopNeck").toBool(),
                                  .keepsEntryJunction = true, .keepsExitHollow = true,
                                  .alternateAnchor = !preferred,
                                  .toothExitFlank = {{cluster.turns.back(), cluster.advancePrevious}}});
            }
        }
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
            result.back().sharesLoopNeck = cuts.value("shareLoopNeck").toBool();
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
    if (form.value("fixedAnchorsOnly").toBool() && form.value("onUnresolved").toBool())
        for (auto& candidate : result) candidate.alternateAnchor = true;
    if (inspectingForm) for (const auto& candidate : result) {
        for (const auto& mark : candidate.landmarks) inspectLandmark("Body landmark", mark);
        if (candidate.entry) inspectCandidate("Entry anchor", *candidate.entry);
        if (candidate.exit) inspectCandidate("Exit anchor", *candidate.exit);
    }
    return result;
}

} // namespace MedinaCutter
