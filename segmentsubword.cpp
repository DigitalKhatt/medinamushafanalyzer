#include "cutter/detail.h"

using namespace MedinaCutter;

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

bool PageAnalysisResult::segmentSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont*,
                                        bool refresh, const QJsonObject* draft, const AnchorPartitionCheck& acceptAnchors) {
    CutGeometryMemoScope geometryMemo;
    resolveCompoundBody(pageIndex, lineIndex, wordIndex);
    const auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = page.lines[lineIndex].words[wordIndex].wordResultInfo.subWords[subWordIndex];
    if (!refresh && !sub.baseGlyphs.empty()) return sub.correct;
    sub.baseGlyphs.clear();
    sub.joins.clear();
    sub.kashidaCuts.clear();
    sub.kashidaPaths.clear();
    sub.extrema.clear();
    sub.letterDiagnostics = {};
    sub.templateTrace.clear();
    sub.matchedTemplates.clear();
    sub.correct = false;
    if (sub.paths.empty() || sub.paths.front() < 0 || sub.paths.front() >= int(word.paths.size()) ||
        word.paths[sub.paths.front()].path.isEmpty()) return false;

    const auto letters = bodyLetters(sub.text);
    if (letters.isEmpty()) {
        sub.baseGlyphs = {{letters, word.bodyShape(sub).path}};
        sub.templateTrace.append("No body letter in transcript · needs review");
        return false;
    }
    if (letters.size() == 1) {
        sub.baseGlyphs = {{letters, word.bodyShape(sub).path}};
        sub.letterDiagnostics.append(QJsonObject{{"letterIndex", 1}, {"character", letters},
            {"position", "isolated"}, {"status", "not-required"}, {"template", ""}, {"candidates", QJsonArray{}}});
        sub.correct = true;
        return true;
    }

    struct SearchForms {
        QJsonObject primary, requested, expanded, continuation, preciseExpanded, preciseContinuation;
        AnchorForms forms, expandedForms, continuationForms, preciseForms, preciseExpandedForms, preciseContinuationForms;
    };
    const auto& requestedForms = draft ? *draft : letterForms();
    auto& geometry = *cutGeometryMemo;
    const auto assemble = [&]() -> bool {
        const auto& sourceConfig = geometryMemo.owned ? primaryLetterForms(requestedForms) : requestedForms;
        const auto ligature = segmentDeclaredLigature(word, sub, sourceConfig);
        if (ligature == LigatureMatch::SharedCut || ligature == LigatureMatch::Failed) {
            if (geometry.profiling) geometry.anchorSearch = "declared-ligature";
            return ligature == LigatureMatch::SharedCut;
        }
        static thread_local SearchForms compiled;
        if (compiled.primary != sourceConfig || compiled.requested != requestedForms) {
            compiled.primary = sourceConfig;
            compiled.requested = requestedForms;
            compileAnchorForms(sourceConfig, compiled.forms);
            compiled.expanded = {};
            compiled.continuation = {};
            if (sourceConfig.contains("fixedAnchorExpansion")) {
                compiled.expanded = expandedAnchorConfig(sourceConfig);
                compileAnchorForms(compiled.expanded, compiled.expandedForms);
                if (sourceConfig.value("fixedAnchorExpansion").toObject().value("continuation").toBool()) {
                    compiled.continuation = expandedAnchorConfig(sourceConfig, true);
                    compileAnchorForms(compiled.continuation, compiled.continuationForms);
                    compileAnchorForms(requestedForms, compiled.preciseForms);
                    compiled.preciseExpanded = expandedAnchorConfig(requestedForms);
                    compileAnchorForms(compiled.preciseExpanded, compiled.preciseExpandedForms);
                    compiled.preciseContinuation = expandedAnchorConfig(requestedForms, true);
                    compileAnchorForms(compiled.preciseContinuation, compiled.preciseContinuationForms);
                }
            }
        }
        if (geometry.profiling && geometry.anchorSearch.isEmpty())
            geometry.anchorSearch = sub.secondaryBodyPath >= 0 ? "compound-body" : "ineligible";
        const bool verify = geometry.verifyContourGeometry;
        std::vector<AnchorPriority> priorities{{sourceConfig, compiled.forms, verify}};
        if (!compiled.expanded.isEmpty()) priorities.push_back({compiled.expanded, compiled.expandedForms, verify});
        if (!compiled.continuation.isEmpty()) {
            if (!verify && sourceConfig != requestedForms)
                priorities.push_back({requestedForms, compiled.preciseForms, true});
            priorities.push_back({compiled.continuation, compiled.continuationForms, verify});
            if (!verify) {
                priorities.push_back({compiled.preciseExpanded, compiled.preciseExpandedForms, true});
                priorities.push_back({compiled.preciseContinuation, compiled.preciseContinuationForms, true});
            }
        }
        if (auto partition = segmentWithAnchors(word, sub, priorities, acceptAnchors)) {
            sub = std::move(*partition);
            return true;
        }
        if (sub.secondaryBodyPath >= 0 && !sub.bodyCounter) {
            auto component = sub;
            if ((assembleDetachedTerminalArm(word, component, sourceConfig) ||
                 assembleDetachedHeadEntry(word, component, sourceConfig)) &&
                coversOriginalInk(word, component) && (!acceptAnchors || acceptAnchors(component))) {
                sub = std::move(component);
                if (geometry.profiling) geometry.anchorSearch = "matched-component";
                return true;
            }
        }
        return false;
    };
    if (assemble()) {
        collectUsedTemplates(sub);
        return true;
    }
    if (sub.letterDiagnostics.isEmpty()) {
        const auto expanded = expandedAnchorConfig(requestedForms, true);
        AnchorForms forms;
        compileAnchorForms(expanded, forms);
        const std::array phases{"initial", "medial", "final"};
        for (int slot = 0; slot < letters.size(); ++slot) {
            const int position = slot == 0 ? 0 : slot + 1 == letters.size() ? 2 : 1;
            QJsonArray candidates;
            for (const auto& form : forms[position].value(letters[slot])) candidates.append(form.value("id"));
            sub.letterDiagnostics.append(QJsonObject{{"letterIndex", slot + 1}, {"character", letters.mid(slot, 1)},
                {"position", phases[position]}, {"status", candidates.isEmpty() ? "no-template" : "no-match"},
                {"template", ""}, {"candidates", candidates}});
        }
        sub.templateTrace.append("No compatible anchor partition");
    }
    sub.correct = false;
    collectUsedTemplates(sub);
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
    if (result.landmarks.isEmpty() && sharedShapeDescription(result.body) && !result.ink.isEmpty()) {
        // A missing join can prevent assembly from visiting this form. Its
        // landmarks are still available directly from the uncut outline.
        WordResultInfo::SubWordInfo geometryOnly;
        OutlineSearchCache cache;
        SearchContext source{.transformedPath = result.ink, .bbox = result.ink.boundingRect(),
                             .subword = geometryOnly, .outlineCache = &cache};
        if (preparePassOutline(source, cache)) {
            if (!result.attempted && !result.complete)
                result.assembly.append("Body landmarks from the source outline. No compatible anchor partition.");
            InspectFormScope inspection(form, result.ink);
            inspectSharedShape(*recognizedSharedShape(source, result.body));
        }
    }
    if (sub.matchedTemplates.contains(id)) result.attempted = true;
    if (!result.attempted) result.assembly.append("The search did not reach this template in this run.");
    if (result.assembly.isEmpty()) result.assembly.append("No letter selected this template in the assembled result.");
    // Keep the full trace: later recovery/assembly can reject a valid local
    // proposal. A failed alternative is never presented as the selected cut.
    result.trace = sub.templateTrace;
    return result;
}
