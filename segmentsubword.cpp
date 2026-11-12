#include "cutter/detail.h"

using namespace MedinaCutter;

static void reportFinalRetry(WordResultInfo::SubWordInfo& sub, const QHash<QString, QString>& parents) {
    if (parents.isEmpty()) return;
    bool matchedRetry = false;
    for (auto& id : sub.matchedTemplates) {
        if (!parents.contains(id)) continue;
        id = parents.value(id);
        matchedRetry = true;
    }
    if (matchedRetry) sub.matchedTemplates.removeDuplicates();
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
        const auto inferred = slot.value("inferredFrom").toString();
        if (parents.contains(inferred)) {
            slot.insert("inferredFrom", parents.value(inferred));
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

bool PageAnalysisResult::segmentSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                        bool refresh, const QJsonObject* draft) {
    CutGeometryMemoScope geometryMemo;
    resolveCompoundBody(pageIndex, lineIndex, wordIndex);
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!refresh && !sub.baseGlyphs.empty())
        return sub.correct;
    const auto& sourceConfig = draft ? *draft : letterForms();
    const auto ligature = segmentDeclaredLigature(word, sub, sourceConfig);
    if (ligature == LigatureMatch::Failed) return false;
    if (ligature == LigatureMatch::SharedCut)
        return pageIndex < 0 || verifyBodyLandmarkOwnership(word, sub, sourceConfig);
    static thread_local QJsonObject previousConfig, compiledConfig;
    static thread_local QHash<QString, QString> compiledRetryParents;
    if (previousConfig != sourceConfig) {
        previousConfig = compiledConfig = sourceConfig;
        const auto finals = finalRetryCandidates(sourceConfig.value("final").toArray(), compiledRetryParents);
        if (!compiledRetryParents.isEmpty()) compiledConfig.insert("final", finals);
    }
    auto requested = compiledConfig;
    const auto retryParents = compiledRetryParents;
    auto recognition = recognizeSubwordBodies(word, sub, requested);

    // The shared collector combines declared lām candidates with the existing
    // matchers for other letters. Lām never enters their method dispatcher.
    // Resolve the declared policies before committing the partition.
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
    // A scaled or split recovery trial is only a proposal. Verify the
    // assembled masks on the source artwork when returning a page result.
    if (valid && pageIndex >= 0) {
        valid = verifyBodyLandmarkOwnership(word, plan.partition, requested);
        if (valid) {
            applyTerminalJunction(word, plan.partition, requested, TerminalEntryPolicy::LowerShoulder);
            applyInitialToothExit(word, plan.partition, requested);
        }
    }
    sub = std::move(plan.partition);
    reportFinalRetry(sub, retryParents);
    return valid;
}

bool PageAnalysisResult::collectLegacyCutProposals(int pageIndex, int lineIndex, int wordIndex,
        int subWordIndex, AFont* font, QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested)) {
        const auto letters = bodyLetters(sub.text);
        if (!medialLamShortExit(requested).value("retryOnUnresolved").toBool() ||
            letters.size() < 3 || !QStringView(letters).mid(1, letters.size() - 2).contains(QChar(0x0644)))
            return false;

        // Reassemble the whole subword: a shorter exit can invalidate a neighbouring cut.
        auto partial = sub;
        requested.insert("_disableLamShortExit", true);
        if (!segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested) ||
            !coversOriginalInk(word, sub)) {
            sub = std::move(partial);
            return false;
        }
        sub.templateTrace.append("Complete partition recovered with the original lam exit");
    }
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
    if (!segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, repair)) return false;
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

bool PageAnalysisResult::segmentWithRecovery(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                            const QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!requested.contains("retryUnresolvedForms"))
        return segmentConfigured(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested);

    // Preserve established cuts. An unresolved body enters the wider contour search.
    static thread_local QJsonObject lastRequested, usual, extended;
    if (lastRequested != requested) {
        lastRequested = requested;
        usual = requested;
        usual.remove("retryUnresolvedForms");
        extended = usual;
        auto recovery = extended.value("recoveryChecks").toObject();
        auto baseChecks = recovery;
        QJsonArray baseExtents;
        for (const auto& item : recovery.value("bodyExtents").toArray()) {
            if (!item.toObject().value("onUnresolved").toBool()) baseExtents.append(item);
        }
        baseChecks.insert("bodyExtents", baseExtents);
        usual.insert("recoveryChecks", baseChecks);
        recovery.insert("initialShaft", requested.value("unresolvedInitialShaft"));
        extended.insert("recoveryChecks", recovery);
        auto scales = extended.value("reconsiderScale").toObject();
        scales.insert("alternateScales", requested.value("unresolvedScaleCenters"));
        scales.insert("tilts", requested.value("unresolvedTiltAngles"));
        extended.insert("reconsiderScale", scales);
        QJsonObject selectors, headRetries;
        for (const auto* position : {"initial", "medial", "final"}) {
            QJsonArray original, searchable, headForms;
            QString characters;
            for (const auto& value : requested.value(position).toArray()) {
                // Resolve references before recovery-only parents leave the active
                // form list. Eligibility controls execution, not shared geometry.
                const auto form = resolvedBodyForm(requested, value.toObject());
                if (form.value("onHeadConflict").toBool()) headForms.append(form);
                if (!hasCounterOutline(form)) {
                    auto retry = form;
                    if (form.contains("recoveryVisibleFrom"))
                        retry.insert("visibleFrom", form.value("recoveryVisibleFrom"));
                    searchable.append(retry);
                }
                if (!form.value("onUnresolved").toBool())
                    original.append(form);
                for (auto ch : form.value("current").toString())
                    if (!characters.contains(ch))
                        characters.append(ch);
            }
            usual.insert(position, original);
            extended.insert(position, searchable);
            selectors.insert(position, characters);
            if (!headForms.isEmpty()) headRetries.insert(position, headForms);
        }
        usual.insert("_headRetryForms", headRetries);
        extended.insert("_headRetryForms", headRetries);
        extended.insert("reconsiderForms", selectors);
        extended.insert("reconsiderFinalAttachments", selectors.value("final"));
    }
    const auto run = [&](const QJsonObject& forms) {
        return segmentConfigured(pageIndex, lineIndex, wordIndex, subWordIndex, font, forms);
    };
    if (run(usual)) {
        // An accepted generic walk can cut through the folded bowl itself.
        // Keep an established result unless the independently found bowl
        // reaches beyond the final mask by a substantial part of the join.
        if (sub.baseGlyphs.size() < 2) return true;
        const auto letters = bodyLetters(sub.text);
        auto finals = requested.value("final").toArray();
        for (qsizetype i = 0; i < finals.size(); ++i) {
            auto form = finals.at(i).toObject();
            if (form.value("body").toObject().value("shape") != "folded-return" ||
                !form.value("onUnresolved").toBool() || !form.value("current").toString().contains(letters.back()))
                continue;
            const auto shape = word.bodyShape(sub);
            const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
            SearchContext outline{.transformedPath = mapped(shape.path), .subword = sub};
            Stretching seam{};
            double shoulderX = 0;
            if (!readTerminalOutline(outline))
                break;
            const auto declared = resolvedBodyForm(requested, form);
            const auto evidence = recognizedSharedShape(outline, declared.value("body").toObject());
            if (!evidence->returnSeam ||
                !cutReturnSeam(outline, *evidence->returnSeam,
                               declared.value("cuts").toObject().value("return").toObject(), seam, &shoulderX))
                break;
            const double width = QLineF(seam.topBaseLine.point, seam.bottomBaseLine.point).length();
            const auto retainsBowl = [&](double margin) {
                const auto bounds = mapped(sub.baseGlyphs.back().path).boundingRect();
                return bounds.left() <= outline.bbox.left() + margin && bounds.right() >= shoulderX - margin;
            };
            if (retainsBowl(.5 * width)) break;
            auto original = sub;
            auto retry = requested;
            form.remove("onUnresolved");
            finals.replace(i, form);
            retry.insert("final", finals);
            if (segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, retry) &&
                sub.baseGlyphs.size() == original.baseGlyphs.size() && retainsBowl(.05 * width) &&
                coversOriginalInk(word, sub)) {
                sub.templateTrace.append("Terminal bowl recovered at its first fold");
            } else sub = std::move(original);
            break;
        }
        return true;
    }
    if (!requested.value("retryUnresolvedForms").toBool() || bodyLetters(sub.text).size() < 2 || sub.paths.empty())
        return false;
    auto original = sub;
    if (run(extended) &&
        plausibleRecoveredBodies(word, sub, extended.value("recoveryChecks").toObject()) &&
        coversOriginalInk(word, sub)) {
        sub.templateTrace.append(QStringLiteral("Unresolved form matched by contour recovery"));
        return true;
    }
    const auto checks = extended.value("recoveryChecks").toObject();
    if (recoverStackedEntry(word, sub, requested, checks) ||
        confirmCounterOutlines(word, sub, requested, checks) || recoverSharedTerminal(word, sub, requested, checks))
        return true;
    sub = std::move(original);
    if (confirmCounterOutlines(word, sub, requested, checks) || recoverSharedTerminal(word, sub, requested, checks) ||
        recoverExposedHead(word, sub, requested, checks)) return true;
    const auto frames = requested.value("unresolvedShapeFrames").toArray();
    if (frames.isEmpty()) return false;
    auto usualWithChecks = usual;
    usualWithChecks.insert("recoveryChecks", checks);
    return stableTemplateCuts(word, sub, usualWithChecks, frames) || stableTemplateCuts(word, sub, extended, frames);
}

bool PageAnalysisResult::segmentConfigured(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                           const QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    // Preserve established initial/medial recoveries. A new final attachment
    // gets a vote only when the first unresolved body is an eligible final.
    static thread_local QJsonObject previousConfig, primary, established;
    if (previousConfig != requested) {
        previousConfig = requested;
        primary = established = requested;
        established.remove("reconsiderFinalAttachments");
        for (const auto* position : {"initial", "medial", "final"}) {
            QJsonArray firstPass, usualRecovery;
            for (const auto& value : requested.value(position).toArray()) {
                auto form = value.toObject();
                if (form.value("onFinalRetry").toBool()) continue;
                usualRecovery.append(form);
                if (!form.value("onIncomplete").toBool()) {
                    form.remove("retryWithVariants");
                    firstPass.append(form);
                }
            }
            primary.insert(position, firstPass);
            established.insert(position, usualRecovery);
        }
    }
    if (segmentWithLetterForms(pageIndex, lineIndex, wordIndex, subWordIndex, font, primary, false))
        return true;
    const auto first = std::find_if(sub.letterDiagnostics.constBegin(), sub.letterDiagnostics.constEnd(),
        [](const auto& value) { return value.toObject().value("status") != "matched"; });
    if (first == sub.letterDiagnostics.constEnd()) return false;
    const auto slot = first->toObject();
    const auto position = slot.value("position").toString(), ch = slot.value("character").toString();
    const bool finalRetry = position == "final" && requested.value("reconsiderFinalAttachments").toString().contains(ch);
    // Detach the choice from the thread-local cache before analysing a folded prefix recursively.
    const QJsonObject config = finalRetry ? requested : established;
    if (!finalRetry && !config.value("reconsiderForms").toObject().value(position).toString().contains(ch)) {
        const auto& forms = bodyCutProgram(config).forms;
        const auto candidates = slot.value("candidates").toArray();
        const bool hasCutRange = std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
            const auto form = forms.constFind(candidate.toString());
            return form != forms.cend() && form->body.value("shape") == "contour-route" &&
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
    if (segmentWithLetterForms(pageIndex, lineIndex, wordIndex, subWordIndex, font, config, true)) {
        if (coversOriginalInk(word, sub))
            return true;
        sub = std::move(previous);
        sub.templateTrace.append("Recovered boundaries failed the ink ownership check");
        return false;
    }
    // Only retain a partial retry that actually resolved an additional letter
    // while preserving all previously recognized letters.
    int gained = 0;
    for (int i = 0; i < previous.letterDiagnostics.size(); ++i) {
        const bool was = previous.letterDiagnostics[i].toObject().value("status") == "matched";
        const bool now = sub.letterDiagnostics[i].toObject().value("status") == "matched";
        if (was && !now) {
            gained = 0;
            break;
        }
        if (!was && now)
            ++gained;
    }
    auto folded = sub;
    if (recoverFoldedArm(word, folded, config, true) &&
        plausibleRecoveredBodies(word, folded, config.value("recoveryChecks").toObject()) &&
        coversOriginalInk(word, folded)) {
        sub = std::move(folded);
        return true;
    }
    if (detachedHeadEntry(word, sub, config))
        return true;
    const bool everySlotMatched =
        std::all_of(sub.letterDiagnostics.begin(), sub.letterDiagnostics.end(),
                    [](const auto& value) { return value.toObject().value("status") == "matched"; });
    if (!gained || everySlotMatched)
        sub = std::move(previous);
    return stableTemplateCuts(word, sub, config);
}

bool PageAnalysisResult::segmentWithLetterForms(int pageIndex, int lineIndex, int wordIndex, int subWordIndex,
                                                AFont* font, const QJsonObject& active, bool expand) {
    auto passConfig = active;
    OutlineSearchCache outlineCache;
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
                if (!retry.supports(seenSettings(form)))
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
        ToothSearchPlan plan(initial.size());
        const auto run = [&] {
            if (segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true,
                                   &config, true, outlineCache, &plan, seenExit)) return true;
            if (!seenExit) consider();
            return false;
        };
        // Exhaust the body placements at the preferred route position before
        // trying the next position. Reuse the cached bodies and contour cuts.
        do {
            for (plan.stage = 1; plan.stage <= 3; ++plan.stage) {
                if (plan.stage > 1) {
                    if (std::none_of(forms.begin(), forms.end(), [&](const auto& value) {
                            return value.toObject().value("recoveryStage").toInt() == plan.stage;
                        })) continue;
                }
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
            return applicable(rule, slot) && seenExit.supports(seenSettings(rule.toObject()));
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
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    const auto joins = std::vector<Stretching>(sub.joins.begin(), sub.joins.end());
    for (int index = 0; index < sub.letterDiagnostics.size(); ++index) {
        const auto slot = sub.letterDiagnostics[index].toObject();
        const auto selected = slot.value("template").toString();
        if (selected != id && !slot.value("candidates").toArray().contains(id)) continue;
        result.assembly.append(QString("Letter %1 · %2 · %3 · %4%5")
            .arg(index + 1).arg(slot.value("character").toString(), slot.value("position").toString(),
                 slot.value("status").toString(), selected.isEmpty() ? QString{} : " · selected " + selected));
        if (selected != id) continue;
        if (index < int(sub.baseGlyphs.size()))
            result.selectedBody.addPath(shape.transform.map(sub.baseGlyphs[index].path).translated(shape.pos));
        LetterFormInspection::Candidate selectedCuts{QString("Selected letter %1 (assembled)").arg(index + 1), {}};
        const auto appendBoundary = [&](int joinIndex, bool receiving) {
            auto boundary = joins[joinIndex];
            if (joinIndex < int(sub.kashidaPaths.size()) && !sub.kashidaPaths[joinIndex].isEmpty()) {
                const auto& caps = sub.kashidaCuts[joinIndex];
                const auto& actual = receiving ? caps.second : caps.first;
                boundary.topBaseLine.point = actual.p1();
                boundary.bottomBaseLine.point = actual.p2();
            }
            for (const auto& cut : boundary.cutSegments()) selectedCuts.cuts.append(cut);
        };
        if (index > 0 && index - 1 < int(joins.size())) appendBoundary(index - 1, true);
        if (index < letters.size() - 1 && index < int(joins.size())) appendBoundary(index, false);
        result.candidates.prepend(selectedCuts);
        for (const auto* field : {"bodyForm", "variant", "cutPolicy", "entryPolicy", "junctionPolicy", "entryFork", "entryWalk"})
            if (slot.contains(field)) result.assembly.append(QString::fromLatin1(field) + ": " +
                slot.value(field).toVariant().toString());
    }
    if (!result.attempted) result.assembly.append("This template was not reached in this run (position, priority or recovery stage). This is not a recognition rejection.");
    if (result.assembly.isEmpty()) result.assembly.append("No letter selected this template in the assembled result.");
    // Keep the full trace: later recovery/assembly can reject a valid local
    // proposal. A failed alternative is never presented as the selected cut.
    result.trace = sub.templateTrace;
    return result;
}
