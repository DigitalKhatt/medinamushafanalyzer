#include "../detail.h"

namespace MedinaCutter {

QString compileAnchorSequences(const QJsonObject& config, AnchorSequences& sequences) {
    sequences.clear();
    const auto declared = config.value("anchorSearch");
    if (declared.isUndefined()) return {};
    if (!declared.isArray()) return "anchorSearch must be an array of template sequences";

    AnchorSequences compiled;
    const std::array positions{"initial", "medial", "final"};
    for (const auto& entry : declared.toArray()) {
        const auto positionsInSequence = entry.toArray();
        if (positionsInSequence.size() < 2) return "Each anchorSearch sequence needs an initial and a final template";
        auto& sequence = compiled.emplace_back();
        for (int slot = 0; slot < positionsInSequence.size(); ++slot) {
            const int position = slot == 0 ? 0 : slot + 1 == positionsInSequence.size() ? 2 : 1;
            const auto declarations = config.value(positions[position]).toArray();
            const auto selection = positionsInSequence.at(slot);
            const auto ids = selection.isArray() ? selection.toArray() : QJsonArray{selection};
            if (ids.isEmpty()) return "Each anchorSearch position needs a template";
            auto& forms = sequence.emplace_back();
            for (const auto& value : ids) {
                const auto id = value.toString();
                const auto found = std::find_if(declarations.begin(), declarations.end(), [&](const auto& declaration) {
                    return !id.isEmpty() && declaration.toObject().value("id") == id;
                });
                if (found == declarations.end())
                    return QString("anchorSearch: %1 must name an %2 template").arg(id, positions[position]);
                const auto form = bodyConstraintForm(config, found->toObject());
                const auto body = form.value("body").toObject();
                const auto shape = body.value("shape");
                const bool supported = position == 0 ? shape == "contour-route" : position == 1 ? toothDescription(body) :
                    shape == "stemmed-bowl" || shape == "tail-branch";
                if (!supported)
                    return QString("anchorSearch: %1 does not supply fixed anchors for this position").arg(id);
                forms.push_back(form);
            }
        }
    }
    sequences = std::move(compiled);
    return {};
}

std::vector<AnchoredBody> anchoredBodies(SearchContext& source, const QJsonObject& form) {
    const auto id = form.value("id").toString();
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto shape = body.value("shape");
    std::vector<AnchoredBody> result;
    source.res = {};
    source.nextIter = std::next(source.xSorted.begin());
    if (shape == "contour-route") {
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, true))
            result.push_back({.formId = id, .exit = source.res, .landmarks = {*source.xSorted.begin()}});
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
        InspectFormScope inspection(form, source.transformedPath);
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->tail) return result;
        const auto& tail = *evidence->tail;
        const auto settings = shapeSettings(form, "tail");
        for (const auto& branch : tail.branches) {
            Stretching entry{};
            inspectLandmark("Bowl shoulder", branch.corner);
            if (!cutTailBranches(source, {tail.tip, tail.curveCount, {branch}}, settings, entry)) continue;
            inspectCandidate("Entry at the bowl shoulder", entry);
            result.push_back({.formId = id, .entry = entry, .landmarks = {tail.tip, branch.corner}});
        }
    }
    return result;
}

} // namespace MedinaCutter
