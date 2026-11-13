#include "cutter/detail.h"

using namespace MedinaCutter;

static void reportFinalRetry(WordResultInfo::SubWordInfo& sub, const QHash<QString, QString>& parents) {
    if (parents.isEmpty()) return;
    for (auto& trace : sub.templateTrace)
        for (auto parent = parents.cbegin(); parent != parents.cend(); ++parent)
            trace.replace(parent.key(), parent.value() + " (final retry)");
    for (qsizetype i = 0; i < sub.letterDiagnostics.size(); ++i) {
        auto slot = sub.letterDiagnostics.at(i).toObject();
        bool changed = false;
        const auto selected = slot.value("template").toString();
        if (parents.contains(selected)) {
            slot.insert("template", parents.value(selected));
            slot.insert("cutPolicy", "final-retry");
            changed = true;
        }
        const auto original = slot;
        for (auto field = original.constBegin(); field != original.constEnd(); ++field) {
            if (field.key() != "inferredFrom" && !field.key().endsWith("Template")) continue;
            const auto referenced = field.value().toString();
            if (!parents.contains(referenced)) continue;
            slot.insert(field.key(), parents.value(referenced));
            changed = true;
        }
        const auto candidates = slot.value("candidates").toArray();
        if (std::any_of(candidates.begin(), candidates.end(), [&](const auto& value) {
                return parents.contains(value.toString());
            })) {
            QJsonArray labels;
            for (const auto& value : candidates) {
                const auto id = value.toString();
                const auto label = parents.value(id, id);
                if (!labels.contains(label)) labels.append(label);
            }
            slot.insert("candidates", labels);
            changed = true;
        }
        if (changed) sub.letterDiagnostics[i] = slot;
    }
}

static const QJsonObject& primaryLetterForms(const QJsonObject& config) {
    static thread_local QJsonObject source, primary;
    if (source != config) {
        source = primary = config;
        for (const auto* position : {"initial", "medial", "final"}) {
            const auto forms = config.value(position).toArray();
            QJsonArray eligible;
            for (const auto& form : forms)
                if (!form.toObject().value("onUnresolvedPartition").toBool()) eligible.append(form);
            if (eligible.size() != forms.size()) primary.insert(position, eligible);
        }
    }
    return primary;
}

bool PageAnalysisResult::segmentSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                        bool refresh, const QJsonObject* draft, const AnchorPartitionCheck& acceptAnchors) {
    CutGeometryMemoScope geometryMemo;
    resolveCompoundBody(pageIndex, lineIndex, wordIndex);
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!refresh && !sub.baseGlyphs.empty())
        return sub.correct;
    const auto finish = [&](bool complete) {
        collectUsedTemplates(sub);
        return complete;
    };
    const auto& requestedForms = draft ? *draft : letterForms();
    const auto source = sub;
    const auto attempt = [&](const QJsonObject& sourceConfig) -> bool {
        const auto ligature = segmentDeclaredLigature(word, sub, sourceConfig);
        if (ligature == LigatureMatch::Failed) return false;
        if (ligature == LigatureMatch::SharedCut)
            return pageIndex < 0 || assembleToothClusterCuts(word, sub, sourceConfig);
        static thread_local QJsonObject previousConfig, compiledConfig;
        static thread_local QHash<QString, QString> compiledRetryParents;
        static thread_local AnchorForms anchorForms;
        if (previousConfig != sourceConfig) {
            previousConfig = compiledConfig = sourceConfig;
            compileAnchorForms(sourceConfig, anchorForms);
            const auto finals = finalRetryCandidates(sourceConfig.value("final").toArray(), compiledRetryParents);
            if (!compiledRetryParents.isEmpty()) compiledConfig.insert("final", finals);
        }
        auto anchored = pageIndex >= 0 ? segmentWithAnchors(word, sub, sourceConfig, anchorForms, acceptAnchors) : std::nullopt;
        if (anchored && (!cutGeometryMemo->profiling || !qEnvironmentVariableIsSet("MEDINA_COMPARE_ANCHORS"))) {
            sub = std::move(*anchored);
            return true;
        }

        auto requested = compiledConfig;
        const auto retryParents = compiledRetryParents;
        auto recognition = recognizeSubwordBodies(word, sub, requested);

        // Collect the templates' boundary proposals, then apply their shared
        // attachment policies before committing the partition.
        const bool proposed = collectLegacyCutProposals(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested);
        auto plan = makeSubwordCutPlan(std::move(sub), requested);
        bool valid = proposed;
        if (valid && pageIndex >= 0 && plan.partition.baseGlyphs.size() >= 2) {
            // A recovery can select a different original contour (e.g. a detached
            // arm). Recognize that source, never a generated letter mask.
            if (recognition.valid) {
                const auto source = word.bodyShape(plan.partition);
                if (recognition.source != source.transform.map(source.path).translated(source.pos))
                    recognition = recognizeSubwordBodies(word, plan.partition, requested);
            }
            valid = resolveSubwordCutPlan(word, recognition, plan, requested);
            if (valid) applyTerminalJunction(word, plan.partition, requested, TerminalEntryPolicy::InwardCorners);
            if (valid && ligature != LigatureMatch::SeparateConnector)
                applySharedReturnEntry(word, plan.partition, requested);
        }
        if (valid && pageIndex >= 0) valid = finalizeBodyCuts(word, plan.partition, requested);
        sub = std::move(plan.partition);
        reportFinalRetry(sub, retryParents);
        if (anchored) {
            cutGeometryMemo->anchorSearch = valid && sameAnchoredPartition(*anchored, sub) ? "same-cuts" : "different-cuts";
            sub = std::move(*anchored);
            return true;
        }
        return valid;
    };
    const auto& primary = geometryMemo.owned ? primaryLetterForms(requestedForms) : requestedForms;
    if (attempt(primary)) return finish(true);
    if (!geometryMemo.owned) return finish(false);

    // Validate the original curves with bracketed intersections. Allow the
    // remaining shape variants only after the established search failed.
    // Geometry cached with the first solver cannot validate these sections.
    auto incomplete = std::exchange(sub, source);
    auto& geometry = *geometryMemo.owned;
    geometry.partitions.clear();
    geometry.inkSegments.clear();
    geometry.verifyContourGeometry = true;
    if (attempt(requestedForms)) {
        sub.templateTrace.append("Unresolved partition matched on the original contours");
        return finish(true);
    }
    sub = std::move(incomplete);
    return finish(false);
}

bool PageAnalysisResult::collectLegacyCutProposals(int pageIndex, int lineIndex, int wordIndex,
        int subWordIndex, AFont* font, QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!searchSubword(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested)) return false;
    if (pageIndex < 0 || sub.baseGlyphs.size() < 2) return true;
    const auto refine = [&] {
        refineLipCuts(lineIndex, wordIndex, subWordIndex, requested);
        refineBodyOwnership(lineIndex, wordIndex, subWordIndex, requested);
        if (sub.joins.empty() || sub.kashidaPaths.empty() || sub.kashidaPaths.back().isEmpty() ||
            sub.letterDiagnostics.isEmpty()) return;
        const auto last = std::prev(sub.joins.end());
        if (!last->fixedLeftBoundary) return;
        const auto id = sub.letterDiagnostics.last().toObject().value("template");
        const auto finals = requested.value("final").toArray();
        const auto found = std::find_if(finals.begin(), finals.end(), [&](const auto& item) {
            const auto rule = item.toObject();
            return rule.value("id") == id && rule.value("retainEntryCrest").toBool();
        });
        if (found == finals.end()) return;
        const auto shape = word.bodyShape(sub);
        SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                              .subword = sub};
        if (!readTerminalOutline(context)) return;
        auto seam = *last;
        seam.topBaseLine = last->topLeft;
        seam.bottomBaseLine = last->bottomLeft;
        const auto boundary = cutAfterEntryCrest(context, seam);
        if (!boundary || !matchesBoundaryBody(context, found->toObject(), *boundary, false)) return;

        // The terminal boundary may move only inside its existing connector.
        // Recut from the same source; keep the other boundary and every prior mask.
        const auto previous = sub;
        auto join = *last;
        join.topLeft = boundary->topBaseLine;
        join.bottomLeft = boundary->bottomBaseLine;
        sub.joins.erase(last);
        sub.joins.insert(join);
        bool valid = cutSubword(lineIndex, wordIndex, subWordIndex, requested) &&
            sub.baseGlyphs.size() == previous.baseGlyphs.size() &&
            sub.kashidaPaths.size() == previous.kashidaPaths.size() &&
            !sub.kashidaPaths.back().isEmpty() &&
            sub.kashidaCuts.back().first == previous.kashidaCuts.back().first &&
            sub.kashidaCuts.back().second != previous.kashidaCuts.back().second;
        for (size_t i = 0; valid && i + 1 < sub.baseGlyphs.size(); ++i)
            valid = sub.baseGlyphs[i].path == previous.baseGlyphs[i].path;
        for (size_t i = 0; valid && i + 1 < sub.kashidaPaths.size(); ++i)
            valid = sub.kashidaPaths[i] == previous.kashidaPaths[i];
        if (!valid || !coversOriginalInk(word, sub)) sub = previous;
        else sub.templateTrace.append(id.toString() + " · raised entrance retained in terminal body");
    };
    const auto letters = bodyLetters(sub.text);
    QJsonObject checks, connectorLips;
    for (const auto& entry : requested.value("recoveryChecks").toObject().value("bodyExtents").toArray()) {
        const auto spec = entry.toObject();
        if (!spec.value("protectHead").toBool()) continue;
        const auto chars = spec.value("letters").toString();
        if (spec.value("position") == "initial" && chars.contains(letters.front()))
            checks.insert("_initialHeadLips", spec.value("lips"));
        if (spec.value("position") == "medial")
            for (QChar ch : chars)
                if (QStringView(letters).mid(1, letters.size() - 2).contains(ch))
                    connectorLips.insert(QString(ch), spec.value("lips"));
    }
    if (!connectorLips.isEmpty()) checks.insert("_connectorHeadLips", connectorLips);
    const auto missing = missingHeadInk(word, sub, letters, checks);
    if (missing.empty()) {
        refine();
        return true;
    }

    auto repair = requested;
    for (auto it = checks.begin(); it != checks.end(); ++it) repair.insert(it.key(), it.value());
    QJsonObject evidence;
    for (const auto& [slot, lips] : missing)
        if (!lips.isNull()) evidence.insert(QString::number(slot), QJsonArray{lips.x1(), lips.y1(), lips.x2(), lips.y2()});
    repair.insert("_observedHeadInk", evidence);
    if (!searchSubword(pageIndex, lineIndex, wordIndex, subWordIndex, font, repair)) return false;
    auto failed = missingHeadInk(word, sub, letters, checks);
    const auto shape = word.bodyShape(sub);
    for (const auto& [slot, lips] : missing) {
        if (lips.isNull()) continue;
        const auto body = shape.transform.map(sub.baseGlyphs[slot].path).translated(shape.pos);
        if (!body.contains(lips.p1()) || !body.contains(lips.p2())) failed.emplace(slot, lips);
    }
    if (failed.empty() && coversOriginalInk(word, sub)) {
        refine();
        return true;
    }
    sub.correct = false;
    sub.templateTrace.append("Head ownership could not be verified after recutting");
    for (const auto& [slot, _] : failed) {
        auto diagnosis = sub.letterDiagnostics[slot].toObject();
        diagnosis.insert("status", "no-match");
        sub.letterDiagnostics[slot] = diagnosis;
    }
    return false;
}

namespace {

struct PlacementTemplates {
    QJsonObject all;
    QJsonObject primary;
    QJsonObject deferred;
};

PlacementTemplates preparePlacements(const QJsonObject& config) {
    PlacementTemplates templates{config, config, config};
    templates.deferred.remove("reconsiderFinalAttachments");
    for (const auto* position : {"initial", "medial", "final"}) {
        QJsonArray primary, deferred;
        for (const auto& value : config.value(position).toArray()) {
            auto form = value.toObject();
            if (form.value("onFinalRetry").toBool()) continue;
            deferred.append(form);
            if (!form.value("onIncomplete").toBool()) {
                form.remove("retryWithVariants");
                primary.append(form);
            }
        }
        templates.primary.insert(position, primary);
        templates.deferred.insert(position, deferred);
    }
    return templates;
}

struct SearchTemplates {
    PlacementTemplates ordinary;
    PlacementTemplates wider;
    QJsonObject headOverlap;
    QJsonObject terminalBoundaries;
    QString terminalLetters;
};

SearchTemplates prepareSearchTemplates(const QJsonObject& requested) {
    if (!requested.contains("retryUnresolvedForms"))
        return {preparePlacements(requested), {}};

    auto ordinary = requested;
    ordinary.remove("retryUnresolvedForms");
    auto wider = ordinary;
    auto widerChecks = wider.value("recoveryChecks").toObject();
    auto ordinaryChecks = widerChecks;
    QJsonArray ordinaryExtents;
    for (const auto& item : widerChecks.value("bodyExtents").toArray()) {
        if (!item.toObject().value("onUnresolved").toBool()) ordinaryExtents.append(item);
    }
    ordinaryChecks.insert("bodyExtents", ordinaryExtents);
    ordinary.insert("recoveryChecks", ordinaryChecks);
    widerChecks.insert("initialShaft", requested.value("unresolvedInitialShaft"));
    wider.insert("recoveryChecks", widerChecks);

    QJsonObject selectors, headRetries;
    QJsonArray terminalReturns;
    for (const auto* position : {"initial", "medial", "final"}) {
        const bool final = QLatin1String(position) == QLatin1String("final");
        QJsonArray ordinaryForms, widerForms, headForms;
        QString characters;
        for (const auto& value : requested.value(position).toArray()) {
            // Inherited body and cut settings may live on a form deferred
            // to a later priority. Resolve them before filtering the lists.
            const auto form = resolvedBodyForm(requested, value.toObject());
            if (form.value("onHeadConflict").toBool()) headForms.append(form);
            if (final &&
                form.value("cuts").toObject().value("return").toObject().contains("maxClipWidths"))
                terminalReturns.append(form);
            if (!hasCounterOutline(form)) widerForms.append(form);
            if (!form.value("onUnresolved").toBool()) ordinaryForms.append(form);
            for (auto ch : form.value("current").toString())
                if (!characters.contains(ch)) characters.append(ch);
        }
        ordinary.insert(position, ordinaryForms);
        wider.insert(position, widerForms);
        selectors.insert(position, characters);
        if (!headForms.isEmpty()) headRetries.insert(position, headForms);
    }
    ordinary.insert("_headRetryForms", headRetries);
    wider.insert("_headRetryForms", headRetries);
    ordinary.insert("_terminalReturns", terminalReturns);
    wider.insert("_terminalReturns", terminalReturns);
    wider.insert("reconsiderForms", selectors);
    wider.insert("reconsiderFinalAttachments", selectors.value("final"));
    QJsonArray preferred, remaining;
    for (const auto& value : wider.value("medial").toArray()) {
        auto form = value.toObject();
        if (form.value("preferAtHeadOverlap").toBool()) {
            preferred.append(form);
        } else remaining.append(form);
    }
    QJsonObject headOverlap;
    if (!preferred.isEmpty()) {
        for (const auto& form : remaining) preferred.append(form);
        headOverlap = wider;
        headOverlap.insert("medial", preferred);
    }
    QJsonObject terminalBoundaries;
    QString terminalLetters;
    QJsonArray finalForms, attachments;
    for (const auto& value : wider.value("final").toArray()) {
        const auto form = value.toObject();
        if (form.value("terminalAttachment").toBool() && form.value("onFinalRetry").toBool()) {
            attachments.append(form);
            terminalLetters += form.value("current").toString();
        } else finalForms.append(form);
    }
    if (!attachments.isEmpty()) {
        for (const auto& form : attachments) finalForms.append(form);
        terminalBoundaries = wider;
        terminalBoundaries.insert("final", finalForms);
    }
    return {preparePlacements(ordinary), preparePlacements(wider), headOverlap, terminalBoundaries, terminalLetters};
}

bool precedingCutCoversOpenHead(const Word& word, WordResultInfo::SubWordInfo& sub,
        const QJsonObject& config, OutlineSearchCache& cache) {
    const auto failed = std::find_if(sub.letterDiagnostics.cbegin(), sub.letterDiagnostics.cend(),
        [](const auto& value) { return value.toObject().value("status") != "matched"; });
    const auto slot = std::distance(sub.letterDiagnostics.cbegin(), failed);
    if (failed == sub.letterDiagnostics.cend() || slot <= 0 || size_t(slot) > sub.joins.size())
        return false;
    const auto diagnostic = failed->toObject();
    if (diagnostic.value("position") != "medial" || diagnostic.value("status") != "no-match")
        return false;
    const auto selectors = diagnostic.value("candidates").toArray();
    const auto incoming = std::next(sub.joins.begin(), slot - 1);
    for (const auto& value : config.value("medial").toArray()) {
        const auto form = value.toObject();
        if (form.value("body").toObject().value("shape") != "open-head" ||
            !selectors.contains(form.value("id"))) continue;
        const auto shape = word.bodyShape(sub);
        auto ink = shape.transform.map(shape.path).translated(shape.pos);
        ink.setFillRule(Qt::WindingFill);
        WordResultInfo::SubWordInfo geometryOnly{};
        SearchContext source{.transformedPath = ink, .bbox = ink.boundingRect(), .subword = geometryOnly,
                             .outlineCache = &cache};
        if (!preparePassOutline(source, cache)) return false;
        for (const auto& [entry, exit] : declaredOpenHeadCuts(source, form, source.bbox.left(), source.bbox.right())) {
            // The preceding body ends at this head's own exit. Reconsider
            // earlier placements only when the whole head lies before it.
            if (QLineF(incoming->topBaseLine.point, exit.topBaseLine.point).length() < 1e-5 &&
                QLineF(incoming->bottomBaseLine.point, exit.bottomBaseLine.point).length() < 1e-5 &&
                cutPrecedes(entry, exit, source.orientation))
                return true;
        }
    }
    return false;
}

} // namespace

bool PageAnalysisResult::searchSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                       const QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    const bool useUnresolvedRules = requested.contains("retryUnresolvedForms");
    // Component matching can supply already filtered forms.
    static thread_local std::array<std::pair<QJsonObject, SearchTemplates>, 2> cachedTemplates;
    auto& [previousConfig, prepared] = cachedTemplates[useUnresolvedRules];
    if (previousConfig != requested) {
        previousConfig = requested;
        prepared = prepareSearchTemplates(requested);
    }
    // A folded prefix or exposed head may run this search recursively
    // with different declarations. Keep this subword's views for the loop.
    const auto templates = prepared;
    OutlineSearchCache outlineCache;

    const auto retryPlacements = [&](const PlacementTemplates& forms) {
        const auto first = std::find_if(sub.letterDiagnostics.constBegin(), sub.letterDiagnostics.constEnd(),
            [](const auto& value) { return value.toObject().value("status") != "matched"; });
        if (first == sub.letterDiagnostics.constEnd()) return false;
        const auto slot = first->toObject();
        const auto position = slot.value("position").toString(), ch = slot.value("character").toString();
        const bool finalRetry = position == "final" && forms.all.value("reconsiderFinalAttachments").toString().contains(ch);
        const auto& config = finalRetry ? forms.all : forms.deferred;
        if (!finalRetry && !config.value("reconsiderForms").toObject().value(position).toString().contains(ch)) {
            const auto& declarations = bodyCutProgram(config).forms;
            const auto candidates = slot.value("candidates").toArray();
            const bool hasCutRange = std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                const auto form = declarations.constFind(candidate.toString());
                return form != declarations.cend() && form->body.value("shape") == "contour-route" &&
                    form->cuts.value("route").toObject().value("range").isObject();
            });
            if (!hasCutRange) return false;
        }
        auto previous = sub;
        if (recoverFoldedArm(word, sub, config, false)) {
            if (plausibleRecoveredBodies(word, sub, config.value("recoveryChecks").toObject()) &&
                coversOriginalInk(word, sub))
                return true;
            sub = previous;
        }
        if (segmentWithLetterForms(pageIndex, lineIndex, wordIndex, subWordIndex, font, config, outlineCache, true)) {
            if (coversOriginalInk(word, sub)) return true;
            sub = std::move(previous);
            sub.templateTrace.append("Recovered boundaries failed the ink ownership check");
            return false;
        }
        // Preserve the earlier partial partition unless the retry resolved
        // another letter and kept every previously recognized letter.
        int gained = 0;
        for (int i = 0; i < previous.letterDiagnostics.size(); ++i) {
            const bool was = previous.letterDiagnostics[i].toObject().value("status") == "matched";
            const bool now = sub.letterDiagnostics[i].toObject().value("status") == "matched";
            if (was && !now) {
                gained = 0;
                break;
            }
            if (!was && now) ++gained;
        }
        auto folded = sub;
        if (recoverFoldedArm(word, folded, config, true) &&
            plausibleRecoveredBodies(word, folded, config.value("recoveryChecks").toObject()) &&
            coversOriginalInk(word, folded)) {
            sub = std::move(folded);
            return true;
        }
        if (detachedHeadEntry(word, sub, config)) return true;
        const bool everySlotMatched =
            std::all_of(sub.letterDiagnostics.begin(), sub.letterDiagnostics.end(),
                        [](const auto& value) { return value.toObject().value("status") == "matched"; });
        if (!gained || everySlotMatched) sub = std::move(previous);
        return false;
    };

    enum class Step { primary, deferred, wider, widerDeferred, assembly };
    WordResultInfo::SubWordInfo ordinaryPartial;
    bool widerPlacementRejected = false;
    const auto widerChecks = templates.wider.all.value("recoveryChecks").toObject();
    for (const auto step : {Step::primary, Step::deferred, Step::wider, Step::widerDeferred, Step::assembly}) {
        if (step == Step::assembly) {
            auto sharedEntry = sub;
            if (assembleBodyPartition(word, sharedEntry, requested, ConnectorAssembly::ShareTightEntry) &&
                plausibleRecoveredBodies(word, sharedEntry, widerChecks) && coversOriginalInk(word, sharedEntry)) {
                sharedEntry.correct = true;
                sub = std::move(sharedEntry);
                return true;
            }
            if (completeBodyPartition(word, sub, requested, widerChecks)) return true;
            // These alternatives may need the attachments from the ordinary
            // partial partition rather than those from the wider search.
            sub = std::move(ordinaryPartial);
            if (completeBodyPartition(word, sub, requested, widerChecks) ||
                assembleSplitEntry(word, sub, requested, widerChecks) ||
                detachedHeadEntry(word, sub, requested)) return true;
            const auto tryPlacement = [&](const QJsonObject& config, bool expand, const QString& trace) {
                auto partial = sub;
                if (segmentWithLetterForms(pageIndex, lineIndex, wordIndex, subWordIndex, font, config,
                                           outlineCache, expand) &&
                    plausibleRecoveredBodies(word, sub, widerChecks) && coversOriginalInk(word, sub)) {
                    sub.templateTrace.append(trace);
                    return true;
                }
                sub = std::move(partial);
                return false;
            };
            const auto& preferred = templates.headOverlap;
            if (!preferred.isEmpty() && precedingCutCoversOpenHead(word, sub, preferred, outlineCache) &&
                tryPlacement(preferred, false, "Recognized head retained after reconsidering the preceding boundary"))
                return true;
            // A terminal attachment already defines a body and a section.
            // Try it alongside the other final boundaries after the existing
            // placements fail, preserving the ordinary final template order.
            const auto letters = bodyLetters(sub.text);
            return !templates.terminalBoundaries.isEmpty() && !letters.isEmpty() &&
                templates.terminalLetters.contains(letters.back()) &&
                tryPlacement(templates.terminalBoundaries, true, "Final attachment matched at its own boundary");
        }
        if (step == Step::wider) {
            if (!requested.value("retryUnresolvedForms").toBool() || bodyLetters(sub.text).size() < 2 || sub.paths.empty())
                return false;
            ordinaryPartial = sub;
        }
        if (step == Step::widerDeferred && widerPlacementRejected) continue;

        const bool wider = step == Step::wider || step == Step::widerDeferred;
        const auto& forms = wider ? templates.wider : templates.ordinary;
        const bool deferred = step == Step::deferred || step == Step::widerDeferred;
        const bool complete = deferred ? retryPlacements(forms) :
            segmentWithLetterForms(pageIndex, lineIndex, wordIndex, subWordIndex, font, forms.primary, outlineCache, false);
        if (!complete) continue;
        if (!wider) return true;
        if (plausibleRecoveredBodies(word, sub, widerChecks) && coversOriginalInk(word, sub)) {
            sub.templateTrace.append(QStringLiteral("Unresolved form matched by contour recovery"));
            return true;
        }
        widerPlacementRejected = true;
    }
    return false;
}

bool PageAnalysisResult::segmentWithLetterForms(int pageIndex, int lineIndex, int wordIndex, int subWordIndex,
        AFont* font, const QJsonObject& active, OutlineSearchCache& outlineCache, bool expand) {
    auto passConfig = active;
    // These proposals belong to the placement pass. The recognized bodies
    // have their own source and declaration keys and survive a priority change.
    outlineCache.terminalCuts.clear();
    outlineCache.contourBodies.clear();
    outlineCache.routeCuts.clear();
    outlineCache.normalsByCenter.clear();
    outlineCache.misplacedHeads.clear();
    if (pageIndex >= 0) {
        const auto observed = active.value("_observedHeadInk").toObject();
        for (auto it = observed.begin(); it != observed.end(); ++it) {
            const auto p = it.value().toArray();
            outlineCache.misplacedHeads.emplace(it.key().toInt(), QLineF(p[0].toDouble(), p[1].toDouble(),
                                                                        p[2].toDouble(), p[3].toDouble()));
        }
    }
    const auto& text = page.lines[lineIndex].words[wordIndex].wordResultInfo.subWords[subWordIndex].text;
    const auto letters = bodyLetters(text);
    if (letters.size() >= 2) {
        // All placement retries have the same letter selectors. Keep the original
        // priority and let the cutter test the remaining shape and position rules.
        // Resolve declarations before filtering by transcript position. A final
        // or initial lām may inherit from a medial form even when no medial lām
        // occurs in this subword. The execution plan carries only resolved data.
        const auto& declarations = bodyCutProgram(active);
        const auto select = [&](const char* position, QStringView slotLetters) {
            QJsonArray rules;
            for (const auto& item : active.value(position).toArray()) {
                auto rule = item.toObject();
                const auto selector = rule.value("current");
                const auto characters = selector.toString();
                if (selector.isString() && std::none_of(slotLetters.begin(), slotLetters.end(),
                        [&](QChar letter) { return characters.contains(letter); })) continue;
                const auto declaration = declarations.forms.constFind(rule.value("id").toString());
                if (declaration != declarations.forms.cend()) {
                    rule.insert("body", declaration->body);
                    rule.insert("cuts", declaration->cuts);
                }
                rules.append(rule);
            }
            passConfig.insert(position, rules);
        };
        const QStringView all(letters);
        select("initial", all.left(1));
        select("medial", all.mid(1, all.size() - 2));
        select("final", all.right(1));
    }
    if (expand)
        passConfig.insert("_checkRecoveredCuts", true);
    const auto* draft = &passConfig;
    const bool matched = segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true, draft, false, outlineCache);
    auto& sub = page.lines[lineIndex].words[wordIndex].wordResultInfo.subWords[subWordIndex];
    if (!outlineCache.misplacedHeads.empty()) {
        // Refit an observed head at that same location. A terminal bowl's own fork
        // can also have been inside the same connector as the medial head.
        auto preferred = passConfig;
        const auto retries = active.value("_headRetryForms").toObject();
        for (auto phase = retries.begin(); phase != retries.end(); ++phase) {
            QJsonArray first;
            QStringList promoted;
            for (const auto& item : phase.value().toArray()) {
                auto rule = item.toObject();
                // The observed head was in connecting ink. Its recovery must
                // respect the preceding body's independently matched exit.
                rule.remove("reclaimEntry");
                first.append(rule);
                promoted.append(rule.value("id").toString());
            }
            for (const auto& rule : preferred.value(phase.key()).toArray())
                if (!promoted.contains(rule.toObject().value("id").toString())) first.append(rule);
            preferred.insert(phase.key(), first);
        }
        auto previous = sub;
        if (!retries.isEmpty() && segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true,
                &preferred, false, outlineCache) && coversOriginalInk(page.lines[lineIndex].words[wordIndex], sub)) {
            sub.templateTrace.append("Recovered the letter head from connecting ink");
            return true;
        }
        sub = std::move(previous);
    }
    if (matched) return true;
    const auto& config = draft ? *draft : letterForms();
    const auto forms = config.value("medial").toArray();
    const auto needsRecovery = [&](const QJsonValue& diagnosis) {
        const auto slot = diagnosis.toObject();
        if (slot.value("position") != "medial" || slot.value("status") != "no-match") return false;
        return std::any_of(forms.begin(), forms.end(), [&](const auto& value) {
            const auto form = value.toObject();
            return form.value("recoveryStage").toInt() > 0 &&
                   form.value("current").toString().contains(slot.value("character").toString());
        });
    };
    const auto trySeenExit = [&](SeenExitMode mode, bool wideLastPitch = false) {
        const bool checkAssembly = mode == SeenExitMode::AlongContour &&
            std::all_of(sub.letterDiagnostics.begin(), sub.letterDiagnostics.end(),
                [](const auto& value) { return value.toObject().value("status") == "matched"; });
        for (int i = 0; i + 1 < sub.letterDiagnostics.size(); ++i) {
            const SeenExitRetry retry{i, mode, wideLastPitch};
            const auto slot = sub.letterDiagnostics[i].toObject();
            const bool unresolved = slot.value("status") == "no-match";
            if (!unresolved && (slot.value("status") != "matched" ||
                (!checkAssembly && sub.letterDiagnostics[i + 1].toObject().value("status") != "no-match"))) continue;
            const bool medial = i > 0;
            const auto rules = medial ? forms : config.value("initial").toArray();
            QStringList eligible;
            for (const auto& rule : rules) {
                const auto form = rule.toObject();
                const auto id = form.value("id").toString();
                const bool reconsiderForm = unresolved || wideLastPitch || mode == SeenExitMode::AlongContour;
                if (reconsiderForm ? !slot.value("candidates").toArray().contains(id) : slot.value("template") != id)
                    continue;
                if (!retry.supports(toothClusterSettings(form)))
                    continue;
                eligible.append(id);
                if (mode == SeenExitMode::ShiftedBend && !wideLastPitch) break;
            }
            if (eligible.isEmpty()) continue;
            auto partial = sub;
            if (segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true, &config, false, outlineCache, nullptr, &retry)) {
                const auto id = sub.letterDiagnostics[i].toObject().value("template").toString();
                if (eligible.contains(id)) {
                    if (mode != SeenExitMode::Configured)
                        sub.templateTrace.prepend(id + (mode == SeenExitMode::AlongContour
                            ? " · exit follows the contour from the third-tooth bend"
                            : " · exit at the bend after the third tooth"));
                    if (wideLastPitch)
                        sub.templateTrace.prepend(id + " · wider spacing before the third tooth");
                    return true;
                }
            }
            sub = std::move(partial);
        }
        return false;
    };
    const auto retrySeenExit = [&] {
        if (trySeenExit(SeenExitMode::ShiftedBend) || trySeenExit(SeenExitMode::AlongContour)) return true;
        for (auto mode : {SeenExitMode::Configured, SeenExitMode::ShiftedBend, SeenExitMode::AlongContour})
            if (trySeenExit(mode, true)) return true;
        return false;
    };
    const auto initial = sub.letterDiagnostics;
    auto best = sub;
    int bestScore = 0;
    const auto consider = [&] {
        int gain = 0;
        for (int i = 0; i < initial.size(); ++i) {
            const bool had = initial[i].toObject().value("status") == "matched";
            const bool has = sub.letterDiagnostics[i].toObject().value("status") == "matched";
            if (had && !has) return;
            if (!had && has) gain += needsRecovery(initial[i]) ? initial.size() + 1 : 1;
        }
        if (gain > bestScore) {
            bestScore = gain;
            best = sub;
        }
    };
    const auto searchPlacements = [&](const SeenExitRetry* seenExit) {
        PlacementSearch plan(initial.size());
        std::set<int> stages{1};
        for (const auto& form : forms) {
            const int stage = form.toObject().value("recoveryStage").toInt();
            if (stage > 1) stages.insert(stage);
        }
        const auto run = [&] {
            if (segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true,
                                   &config, true, outlineCache, &plan, seenExit)) return true;
            if (!seenExit) consider();
            return false;
        };
        // Exhaust the body placements at the preferred route position before
        // trying the next position. Reuse the cached bodies and contour cuts.
        do {
            for (const int stage : stages) {
                plan.stage = stage;
                plan.enumerate = false;
                if (run()) return true;
                plan.enumerate = true;
                plan.skip.fill(0);
                plan.choices.fill(0);
                for (auto& seen : plan.encountered) seen.clear();
                for (int trial = 0; trial < 64; ++trial) {
                    if (run()) return true;
                    int failed = 0;
                    while (failed < sub.letterDiagnostics.size() &&
                           sub.letterDiagnostics[failed].toObject().value("status") == "matched")
                        ++failed;
                    if (!plan.advance(failed)) break;
                }
            }
        } while (plan.advanceRoute());
        return false;
    };
    if (expand || std::any_of(initial.begin(),initial.end(),needsRecovery)) {
        if (searchPlacements(nullptr)) return true;
        sub = std::move(best);
    }
    if (retrySeenExit()) return true;

    // An earlier plausible cut can consume the first of three teeth, or stop
    // before the seen is reached. Revisit those choices only when the terminal
    // supplies an independent tail and the seen can end at its own third bend.
    if (initial.size() < 3) return false;
    const int slot = static_cast<int>(initial.size()) - 2;
    const SeenExitRetry seenExit{slot, SeenExitMode::AlongContour};
    const auto applicable = [&](const QJsonValue& rule, int letter) {
        return initial[letter].toObject().value("candidates").toArray().contains(rule.toObject().value("id"));
    };
    const auto finalForms = config.value("final").toArray();
    if (!std::any_of(finalForms.begin(), finalForms.end(),
                     [&](const auto &rule) {
                         return applicable(rule, slot + 1) && rule.toObject().value("ownsLeftTip").toBool();
                     }) ||
        !std::any_of(forms.begin(), forms.end(), [&](const auto &rule) {
            return applicable(rule, slot) && seenExit.supports(toothClusterSettings(rule.toObject()));
        }))
        return false;
    auto previous = sub;
    if (searchPlacements(&seenExit)) {
        const auto id = sub.letterDiagnostics[slot].toObject().value("template").toString();
        sub.templateTrace.prepend(id + " · earlier cuts reconsidered with the exit at the third-tooth bend");
        return true;
    }
    sub = std::move(previous);
    return false;
}

LetterFormInspection inspectLetterForm(PageAnalysisResult& sample, int page, int line, int wordIndex,
        int subwordIndex, AFont* font, const QJsonObject& config, const QString& id) {
    LetterFormInspection result;
    result.formId = id;
    const auto form = resolvedLetterForm(config, id);
    result.body = form.value("body").toObject();
    result.cuts = form.value("cuts").toObject();
    const auto& word = sample.page.lines[line].words[wordIndex];
    const auto& sub = word.wordResultInfo.subWords[subwordIndex];
    const auto shape = word.bodyShape(sub);
    result.ink = shape.transform.map(shape.path).translated(shape.pos);
    result.ink.setFillRule(Qt::WindingFill);
    struct Capture {
        LetterFormInspection* previous = formInspection;
        bool active = inspectingForm;
        bool coordinates = inspectionCoordinates;
        explicit Capture(LetterFormInspection& result) {
            formInspection = &result; inspectingForm = false; inspectionCoordinates = true;
        }
        ~Capture() { formInspection = previous; inspectingForm = active; inspectionCoordinates = coordinates; }
    } capture(result);
    result.complete = sample.segmentSubword(page, line, wordIndex, subwordIndex, font, true, &config);
    const auto joins = std::vector<Stretching>(sub.joins.begin(), sub.joins.end());
    // A separate contour has no chord at its entry. Map each assembled join
    // to the actual letter indices before displaying its boundary source.
    QVector<int> incoming(sub.letterDiagnostics.size(), -1), outgoing(incoming.size(), -1);
    int joinIndex = 0;
    for (int index = 1; index < sub.letterDiagnostics.size() && joinIndex < int(joins.size()); ++index) {
        if (sub.letterDiagnostics[index].toObject().value("boundaryKind") == "separate-contours") continue;
        incoming[index] = outgoing[index - 1] = joinIndex++;
    }
    for (int index = 0; index < sub.letterDiagnostics.size(); ++index) {
        const auto slot = sub.letterDiagnostics[index].toObject();
        const auto selected = slot.value("template").toString();
        const auto entry = slot.value("entryTemplate").toString(selected);
        const auto exit = slot.value("exitTemplate").toString(selected);
        const bool selectedBody = selected == id && slot.value("status") == "matched";
        const bool usesEntry = !id.isEmpty() && entry == id && incoming[index] >= 0;
        const bool usesExit = !id.isEmpty() && exit == id && outgoing[index] >= 0;
        QStringList references;
        for (const auto* field : {"prefixTemplate", "headTemplate", "componentTemplate"})
            if (slot.value(field) == id) references.append(QString::fromLatin1(field));
        if (!selectedBody && !usesEntry && !usesExit && references.isEmpty() &&
            !slot.value("candidates").toArray().contains(id)) continue;
        result.assembly.append(QString("Letter %1 · %2 · %3 · %4%5")
            .arg(index + 1).arg(slot.value("character").toString(), slot.value("position").toString(),
                 slot.value("status").toString(), selected.isEmpty() ? QString{} : " · selected " + selected));
        if (selectedBody && index < int(sub.baseGlyphs.size()))
            result.selectedBody.addPath(shape.transform.map(sub.baseGlyphs[index].path).translated(shape.pos));
        LetterFormInspection::Candidate selectedCuts{QString("Assembled cuts · letter %1").arg(index + 1), {}};
        const auto appendBoundary = [&](int gap, bool receiving, const QString& source) {
            auto boundary = joins[gap];
            if (gap < int(sub.kashidaPaths.size()) && !sub.kashidaPaths[gap].isEmpty()) {
                const auto& caps = sub.kashidaCuts[gap];
                const auto& actual = receiving ? caps.second : caps.first;
                boundary.topBaseLine.point = actual.p1();
                boundary.bottomBaseLine.point = actual.p2();
            }
            for (const auto& cut : boundary.cutSegments()) selectedCuts.cuts.append(cut);
            result.assembly.append((receiving ? QStringLiteral("Entry cut · ") : QStringLiteral("Exit cut · ")) + source);
        };
        if (usesEntry || (selectedBody && incoming[index] >= 0)) appendBoundary(incoming[index], true, entry);
        if (usesExit || (selectedBody && outgoing[index] >= 0)) appendBoundary(outgoing[index], false, exit);
        if (!selectedCuts.cuts.isEmpty()) result.candidates.prepend(selectedCuts);
        for (const auto& field : references) result.assembly.append(field + " · " + id);
        if (!selectedBody) continue;
        for (const auto* field : {"bodyForm", "variant", "cutPolicy", "entryPolicy", "junctionPolicy", "entryFork", "entryWalk"})
            if (slot.contains(field)) result.assembly.append(QString::fromLatin1(field) + ": " +
                slot.value(field).toVariant().toString());
    }
    if (sub.matchedTemplates.contains(id)) result.attempted = true;
    if (!result.attempted) result.assembly.append("The search did not reach this template in this run.");
    if (result.assembly.isEmpty()) result.assembly.append("No letter selected this template in the assembled result.");
    // Keep the full trace: later recovery/assembly can reject a valid local
    // proposal. A failed alternative is never presented as the selected cut.
    result.trace = sub.templateTrace;
    return result;
}
