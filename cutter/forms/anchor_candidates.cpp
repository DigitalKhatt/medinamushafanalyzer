#include "../detail.h"

namespace MedinaCutter {

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

            const auto form = bodyConstraintForm(config, declaration);
            const auto body = form.value("body").toObject();
            const auto shape = body.value("shape");
            const bool supported = position == 0 ? shape == "contour-route" || shape == "horizontal-counters" :
                position == 1 ? toothDescription(body) :
                shape == "stemmed-bowl" || shape == "tail-branch" || shape == "contour-route" ||
                shape == "counter-neck" || shape == "counter-shoulder";
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
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, initial)) {
            if (initial)
                result.push_back({.formId = id, .exit = source.res, .landmarks = {*source.xSorted.begin()}});
            else
                result.push_back({.formId = id, .entry = source.res, .landmarks = {*source.xSorted.rbegin()}});
        }
    } else if (shape == "horizontal-counters") {
        const auto evidence = recognizedSharedShape(source, body);
        if (evidence->horizontalPair && declaredSharedBoundary(source, form) &&
            matchesBoundaryBody(source, form, source.res, true)) {
            const auto& pair = *evidence->horizontalPair;
            result.push_back({.formId = id, .exit = source.res, .landmarks = {*source.xSorted.begin()},
                              .counters = {pair.right.index, pair.left.index}});
        }
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
                                  .counters = {counter.index}, .retainsCounterRim = retainsRim});
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
