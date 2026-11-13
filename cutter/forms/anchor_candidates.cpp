#include "../detail.h"

namespace MedinaCutter {

QString compileAnchorSequences(const QJsonObject& config, AnchorSequences& sequences) {
    sequences.clear();
    const auto declared = config.value("anchorSearch");
    if (declared.isUndefined()) return {};
    if (!declared.isArray()) return "anchorSearch must be an array of template sequences";

    AnchorSequences compiled;
    const std::array positions{"initial", "medial", "final"};
    const std::array shapes{"contour-route", "tooth-pair", "stemmed-bowl"};
    for (const auto& entry : declared.toArray()) {
        const auto ids = entry.toArray();
        if (ids.size() < 2) return "Each anchorSearch sequence needs an initial and a final template";
        auto& sequence = compiled.emplace_back();
        for (int slot = 0; slot < ids.size(); ++slot) {
            const int position = slot == 0 ? 0 : slot + 1 == ids.size() ? 2 : 1;
            const auto declarations = config.value(positions[position]).toArray();
            const auto id = ids.at(slot).toString();
            const auto found = std::find_if(declarations.begin(), declarations.end(), [&](const auto& value) {
                return !id.isEmpty() && value.toObject().value("id") == id;
            });
            if (found == declarations.end())
                return QString("anchorSearch: %1 must name an %2 template").arg(id, positions[position]);
            const auto form = resolvedBodyForm(config, found->toObject());
            if (form.value("body").toObject().value("shape") != shapes[position])
                return QString("anchorSearch: %1 does not supply fixed anchors for this position").arg(id);
            sequence.push_back(bodyConstraintForm(config, form));
        }
    }
    sequences = std::move(compiled);
    return {};
}

std::vector<AnchoredBody> anchoredBodies(SearchContext& source, const QJsonObject& config, const QJsonObject& form) {
    const auto id = form.value("id").toString();
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto shape = body.value("shape");
    std::vector<AnchoredBody> result;
    source.res = {};
    source.nextIter = std::next(source.xSorted.begin());
    if (shape == "contour-route") {
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, true))
            result.push_back({.formId = id, .exit = source.res, .landmarks = {*source.xSorted.begin()}});
    } else if (shape == "tooth-pair") {
        for (const auto& tooth : declaredToothBoundaries(source, form)) {
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
        const auto policy = cuts.value("stem").toObject();
        for (const auto& stem : stems) {
            if (!cutTerminalStemBodies(source, {stem}, shapeSettings(form, "stem"), policy)) continue;
            result.push_back({.formId = id, .entry = source.res, .landmarks = {stem.crest, stem.bowlEdge},
                .samples = {stem.sample, stem.bowlBottom - QPointF(0, .01 * stem.stroke)}});
        }
    }
    return result;
}

} // namespace MedinaCutter
