#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

static QJsonObject readFormConfig(const QString& filename) {
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const auto config = QJsonDocument::fromJson(file.readAll(), &error).object();
    if (error.error != QJsonParseError::NoError || config.value("version").toInt() != 2 ||
        !config.value("initial").isArray() || !config.value("medial").isArray() || !config.value("final").isArray()) return {};
    return config;
}

QJsonObject withOverrides(QJsonObject defaults, const QJsonObject& overrides) {
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        defaults.insert(it.key(), it.value());
    }
    return defaults;
}

static QString invalidKashidaSettings(const QJsonValue& value, const QJsonObject& defaults,
                                      const QString& location) {
    if (value.isUndefined()) return {};
    if (!value.isObject()) return location + " must be an object";
    const auto settings = value.toObject();
    for (auto setting = settings.begin(); setting != settings.end(); ++setting) {
        const auto key = setting.key();
        const auto expected = defaults.value(key);
        if (expected.isUndefined()) return location + ": unknown setting " + key;
        if (setting.value().type() != expected.type()) return location + ": wrong type for " + key;
        if (!expected.isDouble()) continue;

        const double number = setting.value().toDouble();
        const bool sampleCount = key == "seedSteps" || key == "maxTrackSteps";
        const bool fraction = key == "steepAnchorRatio" || key == "neighborSearchRatio" || key == "sharedCutTolerance";
        if (!std::isfinite(number) || number < 0 ||
            (sampleCount && (number != std::floor(number) || number > 1000)) ||
            (fraction && number > 1) || (key == "samplesPerAnchorWidth" && number <= 0)) {
            return location + ": out-of-range value for " + key;
        }
    }
    const auto effective = withOverrides(defaults, settings);
    for (const auto& range : {std::pair{"minSeedWidthRatio", "maxSeedWidthRatio"},
                              std::pair{"minTrackWidthRatio", "maxTrackWidthRatio"}}) {
        if (effective.value(range.first).toDouble() > effective.value(range.second).toDouble())
            return location + ": minimum exceeds maximum for " + QString::fromLatin1(range.first);
    }
    return {};
}

QString invalidWalkSettings(const QJsonObject& route, const QString& location) {
    static const QStringList probes{"down", "up", "left", "right", "below", "normal", "corner", "walk-stroke-up", "down-stroke", "walk-stroke-normal"};
    if (route.contains("bottomProbe") && !probes.contains(route.value("bottomProbe").toString()))
        return location + ": unsupported contour-walk bottomProbe";
    if (route.contains("trimExitToNormal") && (!route.value("trimExitToNormal").isBool() ||
        route.value("bottomProbe") != "down"))
        return location + ": trimExitToNormal requires a down probe and a boolean value";
    if (route.contains("penWidth")) {
        const auto width = route.value("penWidth");
        const auto probe = route.value("bottomProbe").toString();
        if (!width.isDouble() || !std::isfinite(width.toDouble()) || width.toDouble() <= 0 ||
            route.contains("strokeSource") ||
            (probe != "walk-stroke-normal" && probe != "walk-stroke-up" && probe != "down-stroke"))
            return location + ": penWidth requires a stroke-walk probe and a positive width in page units";
    }
    for (const auto* field : {"visibleFrom", "recoveryVisibleFrom"}) {
        if (route.contains(field) && (route.value("bottom").toArray().isEmpty() || route.contains("bottomProbe") ||
            !QStringList{"top", "bottom"}.contains(route.value(field).toString())))
            return location + ": a visible chord needs two contour walks and a top or bottom anchor";
    }
    if (route.contains("topUseStepEnd") && !route.value("topUseStepEnd").isBool())
        return location + ": topUseStepEnd must be true or false";
    if (route.contains("topAtAnchor") && (!route.value("topAtAnchor").isBool() ||
        !route.value("top").toArray().isEmpty() || route.value("topUseStepEnd").toBool()))
        return location + ": topAtAnchor needs a boolean, without a top walk or step end";
    if (route.contains("probeBothSides") && (!route.value("probeBothSides").isBool() ||
        !route.value("topAtAnchor").toBool() || route.value("bottomProbe") != "normal"))
        return location + ": probeBothSides requires a normal directly at the anchor";
    if (route.contains("oppositeCorner")) {
        if (route.value("bottomProbe") != "corner" || !route.value("oppositeCorner").isObject())
            return location + ": oppositeCorner requires a corner probe and numeric offset bounds";
        const auto bounds = route.value("oppositeCorner").toObject();
        for (auto it = bounds.begin(); it != bounds.end(); ++it) {
            if (QStringList{"sharp", "xExtremum", "yExtremum"}.contains(it.key())) {
                if (!it.value().isBool()) return location + ": opposing landmark flags must be booleans";
                continue;
            }
            if (it.key() == "flanks") {
                if (!it.value().isObject()) return location + ": opposing corner flanks must contain numeric bounds";
                const auto flanks = it.value().toObject();
                for (auto flank = flanks.begin(); flank != flanks.end(); ++flank)
                    if (!QStringList{"minX", "maxX", "minY", "maxY"}.contains(flank.key()) ||
                        !flank.value().isDouble() || !std::isfinite(flank.value().toDouble()))
                        return location + ": opposing corner flanks need finite x/y bounds";
                for (const auto* suffix : {"X", "Y"})
                    if (flanks.contains(QString("min") + suffix) && flanks.contains(QString("max") + suffix) &&
                        flanks.value(QString("min") + suffix).toDouble() > flanks.value(QString("max") + suffix).toDouble())
                        return location + ": opposing corner flank minimum exceeds maximum";
                continue;
            }
            if (!QStringList{"minX", "maxX", "minY", "maxY", "minDistance", "maxDistance"}.contains(it.key()) ||
                !it.value().isDouble() || !std::isfinite(it.value().toDouble()) ||
                (it.key().endsWith("Distance") && it.value().toDouble() < 0))
                return location + ": oppositeCorner needs finite x/y/distance bounds";
        }
        for (const auto* suffix : {"X", "Y", "Distance"})
            if (bounds.contains(QString("min") + suffix) && bounds.contains(QString("max") + suffix) &&
                bounds.value(QString("min") + suffix).toDouble() > bounds.value(QString("max") + suffix).toDouble())
                return location + ": oppositeCorner minimum exceeds maximum";
    }
    for (const auto& options : {std::pair{"anchorSide", QStringList{"top", "bottom"}},
                                std::pair{"strokeSource", QStringList{"local", "paired"}}})
        if (route.contains(options.first) && !options.second.contains(route.value(options.first).toString()))
            return location + ": unsupported " + options.first;
    if (route.contains("walkNormalOffset") &&
        (!route.value("walkNormalOffset").isDouble() ||
         !std::isfinite(route.value("walkNormalOffset").toDouble()) || route.value("walkNormalOffset").toDouble() < 0))
        return location + ": walkNormalOffset must be a nonnegative distance in stroke widths";
    if (route.contains("forkSide") &&
        (!(route.value("forkSide") == "left" || route.value("forkSide") == "right") ||
         route.value("bottomProbe") != "down" ||
         route.value("walkNormalOffset").toDouble() <= 0))
        return location + ": forkSide requires left or right, a downward probe and a positive walkNormalOffset";
    for (const auto* side : {"top", "bottom"}) {
        for (const auto& value : route.value(side).toArray()) {
            const auto step = value.toObject();
            if (step.contains("immediate") && !step.value("immediate").isBool())
                return location + ": immediate must be a boolean on a contour step";
            for (const auto* endpoint : {"start", "end", "stopAt"}) {
                if (!step.contains(endpoint)) continue;
                if (!step.value(endpoint).isObject()) return location + ": step landmark must be an object";
                const auto features = step.value(endpoint).toObject();
                for (auto feature = features.begin(); feature != features.end(); ++feature)
                    if (!QStringList{"sharp", "xExtremum", "yExtremum", "xMinimum", "xMaximum"}.contains(feature.key()) ||
                        !feature.value().isBool())
                        return location + ": contour landmark features must be supported boolean flags";
            }
        }
    }
    return {};
}

QJsonObject namedLetterForm(const QJsonObject& config, const QString& id) {
    for (const auto* phase : {"initial", "medial", "final"})
        for (const auto& entry : config.value(phase).toArray())
            if (entry.toObject().value("id") == id) return entry.toObject();
    return {};
}

bool toothPairDescription(const QJsonObject& body) {
    return body.value("shape") == "tooth-pair";
}

bool bentToothDescription(const QJsonObject& body) {
    const auto shape = body.value("shape");
    return shape == "tooth-bend" || shape == "tooth-shoulder" || shape == "tooth-hook";
}

bool toothDescription(const QJsonObject& body) {
    return toothPairDescription(body) || bentToothDescription(body);
}

// Medial teeth use declared recognition and placement throughout assembly.
bool hasToothBody(const QJsonObject& form) {
    return toothDescription(form.value("body").toObject());
}

QJsonObject toothRecognitionSettings(const QJsonObject& form) {
    return hasToothBody(form) ? form.value("body").toObject() : QJsonObject{};
}

QJsonObject toothPlacementSettings(const QJsonObject& form) {
    return hasToothBody(form) ? form.value("cuts").toObject() : QJsonObject{};
}

// References share descriptions, not execution order. Resolve each field on
// demand so editing a parent in the preview immediately updates its users.
QJsonObject formDescription(const QJsonObject& config, const QJsonObject& form,
        const QString& field, QString* error, QSet<QString> parents) {
    const auto id = form.value("id").toString();
    const auto value = form.value(field);
    if (value.isUndefined()) return {};
    if (!value.isObject() || parents.contains(id)) {
        if (error) *error = id + ": " + field + " must be an object without cyclic inheritance";
        return {};
    }
    auto result = value.toObject();
    const auto inherited = result.take("inherit");
    if (inherited.isUndefined()) return result;
    const auto parent = namedLetterForm(config, inherited.toString());
    if (!inherited.isString() || parent.isEmpty() || !parent.contains(field)) {
        if (error) *error = id + ": " + field + ".inherit must reference an existing description";
        return {};
    }
    parents.insert(id);
    return withOverrides(formDescription(config, parent, field, error, parents), result);
}

QJsonObject resolvedBodyForm(const QJsonObject& config, QJsonObject form) {
    if (form.contains("body")) {
        const auto body = formDescription(config, form, "body");
        auto cuts = formDescription(config, form, "cuts");
        auto tail = cuts.value("tail").toObject();
        if (tail.value("strokeSource") == "global") {
            tail.insert("strokeWidth", config.value("strokeWidth"));
            cuts.insert("tail", tail);
        }
        form.insert("body", body);
        form.insert("cuts", cuts);
    }
    return form;
}

} // namespace MedinaCutter

QJsonObject resolvedLetterForm(const QJsonObject& config, const QString& id) {
    return resolvedBodyForm(config, namedLetterForm(config, id));
}

namespace MedinaCutter {

// Shared shape declarations keep recognition measurements separate from cut
// projection. The field readers also serve forms not migrated yet; they do not
// reconstruct a legacy method or route through the old method dispatcher.
bool lamDescription(const QJsonObject &body) {
    static const QStringList shapes{"contour-stem",    "descending-stem", "forked-stem",
                                    "turning-shaft",   "terminal-bowl",   "terminal-fork",
                                    "terminal-return", "attached-return", "detached-return"};
    return shapes.contains(body.value("shape").toString());
}

const QHash<QString, SharedShapeSchema> &sharedShapeSchemas() {
    static const QHash<QString, SharedShapeSchema> schemas{
        {"counter-neck", {"counter", {"free", "counter-neck", "counter-shoulder", "counter-bridge", "corner-junction"}}},
        {"counter-shoulder", {"counter", {"free", "counter-neck", "counter-shoulder", "counter-bridge"}}},
        {"counter-bridge", {"counter", {"free", "counter-neck", "counter-shoulder", "counter-bridge"}}},
        {"counter-loop", {"loop", {"free", "loop-entry", "loop-exit"}}},
        {"counter-rim", {"rim", {"free", "loop-entry", "loop-exit"}}},
        {"counter-outline", {"outline", {"free", "outline-entry", "outline-exit"}}},
        {"contour-route", {"route", {"free", "contour-route"}}},
        {"suspended-loop", {"head", {"free", "head-neck"}}},
        {"crested-loop", {"head", {"free", "head-neck"}}},
        {"open-head", {"head", {"free", "head-neck"}}},
        {"bowl-turn", {"probe", {"free", "turn-probe", "corner-junction"}}},
        {"open-bowl", {"bowl", {"free", "corner-junction"}}},
        {"horizontal-counters", {"pair", {"free", "pair-neck"}}},
        {"stacked-counters", {"pair", {"free", "pair-neck"}}},
        {"tail-branch", {"tail", {"free", "branch-normal"}}},
        {"rising-return", {"return", {"free", "return-seam"}}},
        {"folded-return", {"return", {"free", "return-seam"}}},
        {"paired-route", {"routes", {"route-left", "route-right"}}},
        {"bowl-neck", {"probe", {"free", "bowl-neck"}}},
        {"profile-neck", {"outline", {"free", "neck-section", "edge-projection", "lower-probe"}}},
        {"tooth-cluster", {"cluster", {"free", "tooth-entry", "tooth-exit"}}},
        {"enclosed-lips", {"head", {"free", "corner-normal"}}},
        {"returning-lips", {"mouth", {"corner-normal"}}},
        {"inclined-blade", {"blade", {"corner-normal"}}},
        {"parallel-return", {"bands", {"band-section"}}},
        {"stemmed-bowl", {"stem", {"free", "stem-turn"}}},
        {"upright-stem", {"stem", {"free", "stem-turn"}}},
        {"forked-bowl", {"fork", {"free", "branch-normal"}}},
        {"counter-attachment", {"attachment", {"free", "counter-bridge", "counter-shoulder"}}},
        {"layered-head", {"head", {"free", "head-probe"}}},
        {"tapered-arm", {"arm", {"free", "arm-root"}}},
        {"component-arm", {"arm", {"free", "source-separation"}}},
        {"separate-components", {"parts", {"component-form"}}},
    };
    return schemas;
}

bool sharedShapeDescription(const QJsonObject &body) {
    return sharedShapeSchemas().contains(body.value("shape").toString());
}

QJsonObject mergeShapeSettings(QJsonObject body, const QJsonObject &cuts) {
    for (auto it = cuts.begin(); it != cuts.end(); ++it)
        body.insert(it.key(),
                    body.value(it.key()).isObject() && it.value().isObject()
                        ? QJsonValue(mergeShapeSettings(body.value(it.key()).toObject(), it.value().toObject()))
                        : it.value());
    return body;
}

QJsonObject shapeSettings(const QJsonObject &form, const QString &key) {
    return mergeShapeSettings(form.value("body").toObject().value(key).toObject(),
                              form.value("cuts").toObject().value(key).toObject());
}

QJsonObject ligatureForm(const QJsonObject& config, const QJsonObject& selection) {
    auto form = resolvedBodyForm(config, namedLetterForm(config, selection.value("template").toString()));
    for (const auto* field : {"body", "cuts"}) {
        if (selection.contains(field))
            form.insert(field, mergeShapeSettings(form.value(field).toObject(), selection.value(field).toObject()));
    }
    return form;
}

bool hasSeenBody(const QJsonObject &form) {
    return form.value("body").toObject().value("shape") == "tooth-cluster" || form.contains("seenTeeth") ||
           form.value("method") == "seen-teeth";
}

QJsonObject seenSettings(const QJsonObject &form) {
    return form.value("body").toObject().value("shape") == "tooth-cluster" ? shapeSettings(form, "cluster")
           : form.contains("seenTeeth")                                    ? form.value("seenTeeth").toObject()
                                                                           : form;
}

QJsonObject loopSettings(const QJsonObject &form) {
    const auto shape = form.value("body").toObject().value("shape");
    if (shape == "counter-loop")
        return shapeSettings(form, "loop");
    if (shape == "counter-rim")
        return {{"rimAnchors", shapeSettings(form, "rim")}};
    return form.value("loopBoundaries").toObject();
}

QJsonObject loopHeadSettings(const QJsonObject &form) {
    return form.value("body").toObject().value("shape") == "crested-loop" ? shapeSettings(form, "head")
                                                                          : form.value("headEdges").toObject();
}

QJsonObject pairedRouteSettings(const QJsonObject &form) {
    return form.value("body").toObject().value("shape") == "paired-route" ? shapeSettings(form, "routes")
                                                                          : form.value("contourBody").toObject();
}

QJsonObject pairedCounterSettings(const QJsonObject &form) {
    return form.value("body").toObject().value("shape") == "stacked-counters"
               ? shapeSettings(form, "pair")
               : form.value("pairedCounterCorners").toObject();
}

QJsonObject openHeadSettings(const QJsonObject &form) {
    return form.value("body").toObject().value("shape") == "open-head" ? shapeSettings(form, "head")
                                                                       : form.value("openHead").toObject();
}

bool hasCounterOutline(const QJsonObject &form) {
    return form.value("body").toObject().value("shape") == "counter-outline" || form.contains("bodyOutline");
}

QJsonObject counterOutlineSettings(const QJsonObject &form) {
    if (form.value("body").toObject().value("shape") != "counter-outline")
        return form.value("bodyOutline").toObject();
    auto result = shapeSettings(form, "outline");
    result.insert("counters", form.value("body").toObject().value("counters"));
    return result;
}

// Common assembly constraints read the same resolved fields for both schemas.
// This view carries no method: declared shapes have their own recognizer and
// projection dispatcher below. Template JSON remains body/cuts only.
static QJsonObject sharedShapeProjectionForm(QJsonObject form) {
    const auto body = form.value("body").toObject();
    if (!sharedShapeDescription(body))
        return form;
    const auto shape = body.value("shape");
    if (shape == "separate-components") {
        auto policy = form.value("cuts").toObject().value("parts").toObject();
        const auto mode = policy.take("mode");
        if (mode == "pen-lift") {
            policy.insert("forms", body.value("parts").toObject().value("forms"));
            form.insert("penLift", policy);
        } else
            form.insert("overhangingEntry", policy);
    }
    if (shape == "component-arm")
        form.insert("detachedArm", shapeSettings(form, "arm"));
    if (shape == "tapered-arm") {
        auto arm = shapeSettings(form, "arm");
        arm.insert("maxNeck", arm.take("maxRootWidth"));
        form = withOverrides(form, arm);
    }
    if (shape == "upright-stem")
        form = withOverrides(form, shapeSettings(form, "stem"));
    if (shape == "layered-head")
        form = withOverrides(form, shapeSettings(form, "head"));
    if (shape == "counter-attachment") {
        const auto settings = shapeSettings(form, "attachment");
        if (form.value("cuts").toObject().value("boundaries").toObject().value("right") == "free")
            form = withOverrides(form, settings);
        else
            form.insert("counterLigature", settings);
    }
    if (shape == "forked-bowl") {
        form = withOverrides(form, shapeSettings(form, "fork"));
        form.insert("start", "left");
    }
    if (shape == "stemmed-bowl")
        form = withOverrides(form, shapeSettings(form, "stem"));
    if (shape == "inclined-blade")
        form.insert("blade", shapeSettings(form, "blade"));
    if (shape == "parallel-return")
        form.insert("parallelReturn", shapeSettings(form, "bands"));
    if (shape == "returning-lips")
        form.insert("mouth", shapeSettings(form, "mouth"));
    if (shape == "enclosed-lips") {
        auto head = shapeSettings(form, "head");
        head.remove("freeEntry");
        form.insert(body.value("head").toObject().value("freeEntry").toBool() ? "head" : "enclosedHead", head);
    }
    if (shape == "tooth-cluster") {
        form = withOverrides(form, seenSettings(form));
        form.insert("seenTeeth", seenSettings(form));
        if (form.value("cuts").toObject().value("boundaries").toObject().value("left") == "free")
            form.insert("side", "right");
    }
    if (shape == "contour-route")
        form = withOverrides(form, shapeSettings(form, "route"));
    if (shape == "profile-neck" && form.value("cuts").toObject().value("boundaries").toObject().value("left") == "free")
        form.insert("start", "left");
    if (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge") {
        form.insert("counter", shapeSettings(form, "counter"));
        for (const auto *field : {"shoulder", "bridge"})
            if (body.contains(field) || form.value("cuts").toObject().contains(field))
                form.insert(field, shapeSettings(form, field));
    }
    if (shape == "counter-loop" || shape == "counter-rim")
        form.insert("loopBoundaries", loopSettings(form));
    if (shape == "counter-outline")
        form.insert("bodyOutline", counterOutlineSettings(form));
    if (shape == "paired-route")
        form.insert("contourBody", pairedRouteSettings(form));
    if (shape == "stacked-counters")
        form.insert("pairedCounterCorners", shapeSettings(form, "pair"));
    if (shape == "tail-branch") {
        form.insert("tailEntry", shapeSettings(form, "tail"));
        form.insert("start", "left");
    }
    if (shape == "rising-return")
        form.insert("tail", shapeSettings(form, "return"));
    if (shape == "folded-return") {
        form.insert("fold", shapeSettings(form, "return"));
        form.insert("start", "left");
    }
    if (shape == "horizontal-counters")
        form = withOverrides(form, form.value("cuts").toObject().value("pair").toObject());
    if (shape == "open-head")
        form.insert("openHead", shapeSettings(form, "head"));
    if (shape == "suspended-loop")
        form.insert("head", shapeSettings(form, "head"));
    if (shape == "crested-loop")
        form.insert("headEdges", shapeSettings(form, "head"));
    if (shape == "bowl-turn") {
        form = withOverrides(form, form.value("cuts").toObject().value("probe").toObject());
        form.insert("start", body.value("origin"));
    }
    if (shape == "open-bowl")
        form.insert("start", "left");
    if (body.contains("features"))
        form.insert("bodyFeatures", body.value("features"));
    if (body.contains("counters"))
        form.insert("bodyCounters", body.value("counters"));
    return form;
}

static QString invalidBowlJunction(const QJsonObject& junction) {
    for (const auto* key : {"minCornerAngle", "maxTurnDistance", "maxWidth"}) {
        const auto value = junction.value(key);
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() <= 0)
            return "junction needs a positive corner angle, turn distance and width";
    }
    const auto sample = junction.value("roundedTurnSample");
    if (!sample.isUndefined() && (!sample.isDouble() || !std::isfinite(sample.toDouble()) ||
        sample.toDouble() <= 0 || sample.toDouble() > junction.value("maxTurnDistance").toDouble()))
        return "roundedTurnSample must be positive and at most maxTurnDistance";
    if (junction.size() != 3 + junction.contains("roundedTurnSample") ||
        junction.value("minCornerAngle").toDouble() >= 180)
        return "invalid bowl junction settings";
    return {};
}

static QString validateSharedShape(const QJsonObject &form, const QString &phase) {
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto shape = body.value("shape").toString(), id = form.value("id").toString();
    if (cuts.isEmpty())
        return id + ": declared shapes require separate cuts";
    const auto boundaries = cuts.value("boundaries").toObject();
    if (boundaries.size() != 2 || !boundaries.contains("left") || !boundaries.contains("right") ||
        (phase == "initial" && boundaries.value("right") != "free") ||
        (phase == "final" && boundaries.value("left") != "free"))
        return id + ": cuts.boundaries must describe both sides for this position";
    const auto schema = sharedShapeSchemas().value(shape);
    const auto &section = schema.section;
    QStringList bodyKeys{"shape", "counters", "features", section}, cutKeys{"boundaries", section};
    if (shape == "contour-route") {
        if (phase == "initial" && cuts.contains("minConnectorWidths")) {
            const auto length = cuts.value("minConnectorWidths");
            if (!length.isDouble() || !std::isfinite(length.toDouble()) || length.toDouble() <= 0)
                return id + ": minConnectorWidths must be positive";
            cutKeys << "minConnectorWidths";
        }
    }
    if (shape == "rising-return") {
        bodyKeys << "entryRecoveryRoute";
        cutKeys << "entryRecoveryRoute";
    }
    if (shape == "upright-stem")
        cutKeys << "recoveryShaft";
    if (shape == "tail-branch")
        bodyKeys << "maxShoulderBacktrackWidths";
    if (shape == "layered-head")
        bodyKeys << "neck";
    if (shape == "bowl-neck") {
        bodyKeys.removeAll("probe");
        bodyKeys << "neck";
    }
    if (phase == "final" && (shape == "contour-route" || shape == "bowl-neck" || shape == "open-bowl")) {
        cutKeys << "junction";
        if (cuts.contains("junction")) {
            const auto error = invalidBowlJunction(cuts.value("junction").toObject());
            if (!error.isEmpty()) return id + ": " + error;
        }
    }
    if (shape == "profile-neck") {
        bodyKeys = QStringList{"shape", "counters", "features", "neck", "profile"};
        cutKeys = QStringList{"boundaries", "probe", "attachment"};
    }
    if (shape == "bowl-turn")
        bodyKeys.removeAll("probe");
    if (shape == "bowl-turn" || (shape == "counter-neck" && boundaries.value("right") == "corner-junction")) {
        bodyKeys << "origin" << "junction";
        if (body.value("origin") != "left")
            return id + ": bowl-turn starts at the free left end";
        if (body.contains("junction") || boundaries.value("right") == "corner-junction") {
            const auto error = invalidBowlJunction(body.value("junction").toObject());
            if (!error.isEmpty()) return id + ": " + error;
        }
    }
    if (shape == "open-bowl") {
        const auto geometry = body.value("bowl").toObject(), policy = cuts.value("bowl").toObject();
        const auto angle = geometry.value("minCornerAngle").toDouble(), width = policy.value("maxWidth").toDouble();
        const auto sample = geometry.value("roundedTurnSample");
        if (phase != "final" || geometry.size() != 1 + !sample.isUndefined() || policy.size() != 1 ||
            !(angle > 0 && angle < 180) || !(width > 0) || !std::isfinite(width) ||
            (!sample.isUndefined() && !(sample.toDouble() > 0 && sample.toDouble() <= .5)))
            return id + ": open bowl needs a final position, a corner angle and a positive maximum cut width";
    }
    if (shape == "counter-loop") {
        const auto entry = body.value("loop").toObject().value("entryCorner").toObject();
        const auto fallback = entry.value("tryLowerCorner");
        if (!fallback.isUndefined() && (!fallback.isBool() || entry.value("placement") != "upper"))
            return id + ": entryCorner.tryLowerCorner must be a boolean on an upper entry corner";
    }
    if (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge") {
        bodyKeys << "shoulder";
        cutKeys << "shoulder" << "bridge";
    }
    const auto normalValley = cuts.value("counter").toObject().value("normalValley");
    if (!normalValley.isUndefined() && (shape != "counter-neck" || phase != "initial" || !normalValley.isBool()))
        return id + ": normalValley must be a boolean on an initial counter-neck cut";
    for (auto it = body.begin(); it != body.end(); ++it)
        if (!bodyKeys.contains(it.key()))
            return id + ": unknown body setting " + it.key();
    for (auto it = cuts.begin(); it != cuts.end(); ++it)
        if (!cutKeys.contains(it.key()))
            return id + ": unknown cut-policy setting " + it.key();
    for (const auto *old : {"method",
                            "counter",
                            "loopBoundaries",
                            "bodyOutline",
                            "top",
                            "bottom",
                            "bottomProbe",
                            "bodyFeatures",
                            "bodyCounters",
                            "seenTeeth",
                            "shoulder",
                            "bridge",
                            "head",
                            "headEdges",
                            "openHead",
                            "contourBody",
                            "pairedCounterCorners",
                            "counterOffset",
                            "baselineOffset",
                            "probeX",
                            "probeRadius",
                            "minCrossings"})
        if (form.contains(old))
            return id + ": declared geometry belongs in body / cuts, not " + old;
    const QStringList projectionKeys{
        "normalNeck", "normalValley", "normalCut", "trimExitToNormal", "offset", "baselineOffset", "sideOffset", "entryBelowCounter",
        "required",   "strokeOffsetX", "bottomProbe",      "oppositeCorner", "normalOffset", "normalOffsetTolerance",
        "penWidth",   "strokeSource",  "walkNormalOffset", "anchorSide",     "visibleFrom",  "maxProjectionInset"};
    const auto checkBody = [&](auto &&self, const QJsonObject &value, bool anchor = false) -> bool {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (projectionKeys.contains(it.key()) && !(anchor && it.key() == "offset"))
                return false;
            if (it.value().isObject() && !self(self, it.value().toObject(), it.key() == "anchor"))
                return false;
        }
        return true;
    };
    if (!checkBody(checkBody, body))
        return id + ": projection settings cannot change recognized body evidence";
    for (auto it = boundaries.begin(); it != boundaries.end(); ++it) {
        const auto mode = it.value().toString();
        const auto &modes = schema.boundaries;
        if (!modes.contains(mode) || (phase == "medial" && mode == "free"))
            return id + ": unsupported boundary policy for " + shape;
    }
    if ((shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge" ||
         shape == "contour-route") &&
        phase == "medial")
        return id + ": this boundary shape needs an initial or final position";
    if (shape == "counter-rim" && phase != "medial")
        return id + ": counter-rim requires two boundaries";
    if ((shape == "paired-route" && phase != "medial") ||
        ((shape == "tail-branch" || shape == "rising-return" || shape == "folded-return") && phase != "final") ||
        (shape == "horizontal-counters" && phase != "initial") || (shape == "stacked-counters" && phase != "medial") ||
        (shape == "suspended-loop" && phase != "initial") ||
        ((shape == "crested-loop" || shape == "open-head") && phase != "medial") ||
        ((shape == "bowl-turn" || shape == "bowl-neck") && phase != "final"))
        return id + ": shape does not support this letter position";
    if (shape == "stacked-counters") {
        const auto pair = cuts.value("pair").toObject();
        const auto ratio = pair.value("maxExitWidthRatio");
        if (!ratio.isUndefined() && (!ratio.isDouble() || !std::isfinite(ratio.toDouble()) ||
                                    ratio.toDouble() <= 0 || !pair.value("normalCut").toBool()))
            return id + ": maxExitWidthRatio needs a positive ratio and a normal cut";
    }
    if (shape == "contour-route") {
        const auto route = body.value("route").toObject(), policy = cuts.value("route").toObject();
        if (route.contains("shoulder") || policy.contains("range")) {
            const auto shoulder = route.value("shoulder").toObject();
            const auto range = policy.value("range").toObject();
            if (!route.value("shoulder").isObject() || range.isEmpty() ||
                policy.value("bottomProbe") != "walk-stroke-normal")
                return id + ": a contour cut range needs a shoulder and a normal probe";
            for (auto field = shoulder.begin(); field != shoulder.end(); ++field) {
                if (!QStringList{"minRise", "minRun", "maxRun", "flankSampleWidths"}.contains(field.key()) ||
                    !field.value().isDouble() || !std::isfinite(field.value().toDouble()) || field.value().toDouble() <= 0)
                    return id + ": contour shoulder measurements must be positive numbers";
            }
            if (shoulder.value("flankSampleWidths").toDouble(.25) > 1 ||
                shoulder.value("minRun").toDouble(2) > shoulder.value("maxRun").toDouble(12))
                return id + ": invalid contour shoulder flank or run limits";
            const auto minimum = range.value("minWidths"), preferred = range.value("preferredWidths");
            const auto maximum = range.value("maxWidths");
            if (range.size() != 2 + range.contains("maxWidths") ||
                !minimum.isDouble() || !std::isfinite(minimum.toDouble()) || minimum.toDouble() < 0 ||
                !preferred.isDouble() || !std::isfinite(preferred.toDouble()) || preferred.toDouble() < minimum.toDouble() ||
                (!maximum.isUndefined() && (!maximum.isDouble() || !std::isfinite(maximum.toDouble()) ||
                                            maximum.toDouble() < preferred.toDouble())))
                return id + ": contour cut range needs ordered minWidths, preferredWidths and optional maxWidths";
        }
    }
    if (shape == "folded-return") {
        const auto positive = [](const QJsonValue& value) {
            return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0;
        };
        const auto turn = body.value("return").toObject();
        for (const auto* field : {"minBowlRun", "minFoldRun", "minReturn"})
            if (!positive(turn.value(field))) return id + ": folded return distances must be positive";
        if (turn.contains("maxShoulderDistance") && !positive(turn.value("maxShoulderDistance")))
            return id + ": maxShoulderDistance must be positive";
        if (!positive(cuts.value("return").toObject().value("maxChord")))
            return id + ": return cut needs a positive maxChord";
    }
    if (body.contains("entryRecoveryRoute") || cuts.contains("entryRecoveryRoute")) {
        const auto route = body.value("entryRecoveryRoute").toObject();
        const auto policy = cuts.value("entryRecoveryRoute").toObject();
        if (route.value("top").toArray().isEmpty() || policy.isEmpty())
            return id + ": entryRecoveryRoute needs a top walk and a cut policy";
        const auto problem = invalidWalkSettings(withOverrides(route, policy), id + ".entryRecoveryRoute");
        if (!problem.isEmpty()) return problem;
    }
    if (shape == "tail-branch") {
        const auto tail = cuts.value("tail").toObject();
        const auto strokeSource = tail.value("strokeSource");
        const auto strokeWidth = tail.value("strokeWidth");
        if (!strokeSource.isUndefined() && strokeSource != "global")
            return id + ": cuts.tail.strokeSource must be global";
        if (strokeSource == "global" && strokeWidth.isUndefined())
            return id + ": global strokeSource requires a top-level strokeWidth";
        if (!strokeWidth.isUndefined() && (!strokeWidth.isDouble() ||
            !std::isfinite(strokeWidth.toDouble()) || strokeWidth.toDouble() <= 0))
            return id + ": cuts.tail.strokeWidth must be a positive width in page units";
    }
    const auto flankSample = body.value("tail").toObject().value("flankSampleWidths");
    if (!flankSample.isUndefined() && (!flankSample.isDouble() || !std::isfinite(flankSample.toDouble()) ||
        flankSample.toDouble() <= 0 || flankSample.toDouble() > 1))
        return id + ": tail.flankSampleWidths must be greater than zero and at most one";
    const auto shoulderBacktrack = body.value("maxShoulderBacktrackWidths");
    if (!shoulderBacktrack.isUndefined() && (!shoulderBacktrack.isDouble() ||
        !std::isfinite(shoulderBacktrack.toDouble()) || shoulderBacktrack.toDouble() <= 0 ||
        shoulderBacktrack.toDouble() > .5))
        return id + ": body.maxShoulderBacktrackWidths must be greater than zero and at most one half";
    if (shape == "separate-components") {
        const auto parts = body.value("parts").toObject(), policy = cuts.value("parts").toObject();
        if (phase != "medial" || parts.value("count") != 2)
            return id + ": separated bodies require two source components";
        for (auto it = parts.begin(); it != parts.end(); ++it)
            if (!QStringList{"count", "forms"}.contains(it.key()))
                return id + ": unknown component body feature";
        const auto mode = policy.value("mode");
        if (mode == "pen-lift") {
            if (policy.size() != 2 || !QStringList{"entry", "exit"}.contains(policy.value("side").toString()) ||
                parts.value("forms").toArray().isEmpty())
                return id + ": pen lift needs a side and component forms";
        } else if (mode == "overhanging-head") {
            if (policy.size() != 3 || !(policy.value("maxGap").toDouble() > 0) ||
                !(policy.value("maxEntryToHead").toDouble() > 0) || parts.contains("forms"))
                return id + ": invalid overhanging head policy";
        } else
            return id + ": unknown component assembly policy";
    }
    if (shape == "component-arm") {
        const auto arm = body.value("arm").toObject(), policy = cuts.value("arm").toObject();
        if (phase != "final" || arm.size() != 1 || !(arm.value("minRise").toDouble() > 0) || policy.size() != 1 ||
            !(policy.value("maxTipGap").toDouble() > 0))
            return id + ": invalid component arm or attachment gap";
    }
    if (shape == "tapered-arm") {
        const auto arm = body.value("arm").toObject(), policy = cuts.value("arm").toObject();
        if (phase != "final" || arm.size() != 2 || !(arm.value("minRise").toDouble() > 0) ||
            !(arm.value("maxRootWidth").toDouble() > 0) || policy.size() != 1 ||
            !policy.value("preferJunction").isBool())
            return id + ": invalid tapered arm body or root policy";
    }
    if (shape == "upright-stem") {
        const auto policy = cuts.value("stem").toObject();
        if (phase != "final" || !body.value("stem").toObject().isEmpty() ||
            !(policy.value("turnAngle").toDouble() > 0) || policy.value("turnAngle").toDouble() >= 90 ||
            !(policy.value("stemWidthLimit").toDouble() > 0) || !cuts.value("recoveryShaft").isBool())
            return id + ": invalid upright stem cut policy";
    }
    if (shape == "layered-head") {
        if (phase != "initial")
            return id + ": layered head requires an initial body";
        const auto head = body.value("head").toObject(), policy = cuts.value("head").toObject();
        for (const auto *key : {"minExtrema", "ascenderProbeX", "ascenderCrossings"})
            if (!(head.value(key).toDouble() > 0))
                return id + ": invalid layered head profile";
        if (!(policy.value("counterProbeX").toDouble() > 0) || policy.value("counterCrossings").toInt() < 2)
            return id + ": invalid layered head probe";
    }
    if (shape == "counter-attachment") {
        if (phase == "final")
            return id + ": counter attachment requires an initial or medial body";
        const auto attachment = body.value("attachment").toObject(), policy = cuts.value("attachment").toObject();
        for (auto it = attachment.begin(); it != attachment.end(); ++it)
            if (!QStringList{"outer", "forkRise", "minAscenderRise", "shoulder"}.contains(it.key()))
                return id + ": unknown attachment body feature";
        for (auto it = policy.begin(); it != policy.end(); ++it)
            if (!QStringList{"lowerBridge", "maxBridgeRatio", "maxBridgeBend", "letters"}.contains(it.key()))
                return id + ": unknown attachment cut policy";
        if (!attachment.contains("forkRise") && attachment.value("outer").toObject().isEmpty())
            return id + ": attachment needs an outer route or fork";
    }
    if (shape == "forked-bowl") {
        const auto dimensions = body.value("fork").toObject(), policy = cuts.value("fork").toObject();
        if (phase != "final" || dimensions.size() != 1 || !(dimensions.value("minRise").toDouble() > 0) ||
            policy.size() != 1 || !(policy.value("walkWidths").toDouble() > 0))
            return id + ": invalid bowl fork body or normal offset";
    }
    if (shape == "stemmed-bowl") {
        if (phase != "final")
            return id + ": stemmed bowl requires a final body";
        const auto stem = body.value("stem").toObject(), policy = cuts.value("stem").toObject();
        if (stem.size() != 1 || !(stem.value("minRise").toDouble() > 0) || policy.size() != 2 ||
            !(policy.value("maxNeckDistance").toDouble() > 0) || !(policy.value("turnAngle").toDouble() > 0) ||
            policy.value("turnAngle").toDouble() >= 90)
            return id + ": invalid stemmed bowl settings";
    }
    if (shape == "inclined-blade" || shape == "parallel-return") {
        if (phase != "medial")
            return id + ": a folded blade needs two medial boundaries";
        const auto dimensions = body.value(section).toObject(), policy = cuts.value(section).toObject();
        const QStringList keys = shape == "inclined-blade" ? QStringList{"minReach", "minRise"} : QStringList{"minRun"};
        for (auto it = dimensions.begin(); it != dimensions.end(); ++it)
            if (!keys.contains(it.key()) || !it.value().isDouble() || !(it.value().toDouble() > 0))
                return id + ": invalid blade measurement";
        for (auto it = policy.begin(); it != policy.end(); ++it)
            if (it.key() != "margin" || !it.value().isDouble() || !(it.value().toDouble() > 0))
                return id + ": invalid blade cut margin";
    }
    if (shape == "returning-lips") {
        if (phase != "medial")
            return id + ": returning-lips requires a medial body";
        const auto mouth = body.value("mouth").toObject(), policy = cuts.value("mouth").toObject();
        for (auto it = mouth.begin(); it != mouth.end(); ++it)
            if (!QStringList{"minLipRun", "maxLipRun", "maxHeight"}.contains(it.key()) || !it.value().isDouble() ||
                !(it.value().toDouble() > 0))
                return id + ": invalid returning-lip measurement";
        for (auto it = policy.begin(); it != policy.end(); ++it)
            if (!QStringList{"projectExit", "exitMargin", "maxThroat", "separateSharedEntry"}.contains(it.key()))
                return id + ": invalid returning-lip policy";
    }
    if (shape == "enclosed-lips") {
        const auto head = body.value("head").toObject();
        if (phase == "final" || head.value("freeEntry").toBool() != (phase == "initial"))
            return id + ": enclosed-lips position disagrees with its free entry";
        for (const auto *field : {"minAspect", "radius"})
            if (head.contains(field) && (!head.value(field).isDouble() || !(head.value(field).toDouble() > 0)))
                return id + ": invalid head measurement";
        const auto width = cuts.value("head").toObject().value("maxSectionWidth");
        if (!width.isUndefined() && (!width.isDouble() || !(width.toDouble() > 0)))
            return id + ": invalid head section width";
    }
    if (shape == "tooth-cluster") {
        const auto cluster = body.value("cluster").toObject(), policy = cuts.value("cluster").toObject();
        for (auto it = cluster.begin(); it != cluster.end(); ++it)
            if (!QStringList{"minToothRise", "minPitch", "maxPitch", "minLastPitch", "maxLastPitch", "retryMaxLastPitch",
                             "maxTipHeightSpreadRatio", "convexTips"}
                     .contains(it.key()))
                return id + ": unknown tooth-cluster body setting " + it.key();
        for (auto it = policy.begin(); it != policy.end(); ++it)
            if (!QStringList{"rightOffset", "maxExitBendDistance", "entryCornerOptional", "exitBendFirst", "exitOffset",
                             "exitDrop", "retryBendOffset", "terminalBowl", "roundedEntry"}
                     .contains(it.key()))
                return id + ": unknown tooth-cluster cut setting " + it.key();
    }
    if (shape == "profile-neck") {
        if (phase == "medial")
            return id + ": profile-neck requires an initial or final body";
        const auto neck = body.value("neck").toObject(),
                   strip = body.value("profile").toObject().value("strip").toObject();
        for (const auto *flag : {"bottomCheck", "topCheck", "sameDirectionCheck"})
            if (neck.contains(flag) && !neck.value(flag).isBool())
                return id + ": neck flank checks must be boolean";
        const auto clear = body.value("profile").toObject().value("clearBelow").toObject();
        if (!strip.isEmpty() && (!strip.value("radius").isDouble() || !(strip.value("radius").toDouble() > 0) ||
                                 strip.value("count").toInt() < 1))
            return id + ": strip profile needs a positive radius and count";
        if (!clear.isEmpty() &&
            (!clear.value("width").isDouble() || !(clear.value("width").toDouble() > 0) ||
             !clear.value("offsetY").isDouble() || !std::isfinite(clear.value("offsetY").toDouble())))
            return id + ": clear-below profile needs a positive width and finite offset";
        const auto landmarkCrossings = body.value("profile").toObject().value("minLandmarkCrossings");
        if (!landmarkCrossings.isUndefined() && (!landmarkCrossings.isDouble() || landmarkCrossings.toInt() < 2 ||
                                                 landmarkCrossings.toDouble() != landmarkCrossings.toInt()))
            return id + ": landmark crossings must be an integer of at least two";
        if (cuts.contains("attachment")) {
            const auto attachment = cuts.value("attachment").toObject();
            if (phase != "final" || attachment.value("previous").toString().isEmpty() ||
                !QStringList{"initial", "medial"}.contains(attachment.value("previousPosition").toString()))
                return id + ": invalid shared attachment selection";
        }
        const auto crossings = body.value("profile").toObject().value("centerCrossings");
        if (!crossings.isUndefined() &&
            (!crossings.isDouble() || crossings.toInt() < 2 || crossings.toDouble() != crossings.toInt()))
            return id + ": centerCrossings needs an integer of at least two";
        if (strip.isEmpty() && clear.isEmpty() && crossings.isUndefined() && landmarkCrossings.isUndefined())
            return id + ": profile-neck needs an intrinsic profile";
        if (cuts.contains("probe")) {
            const auto probe = cuts.value("probe").toObject();
            if (phase != "final" || boundaries.value("right") != "lower-probe" || probe.size() != 1 ||
                !probe.value("offsetX").isDouble() || !std::isfinite(probe.value("offsetX").toDouble()))
                return id + ": lower probe requires a finite offset";
        }
    }
    if (shape == "bowl-neck") {
        const auto neck = body.value("neck").toObject(), probe = cuts.value("probe").toObject();
        for (auto it = neck.begin(); it != neck.end(); ++it)
            if (!QStringList{"lookback", "previousOffset", "lookahead", "minX", "maxX", "minY", "maxY"}.contains(
                    it.key()) ||
                !it.value().isDouble() || !std::isfinite(it.value().toDouble()) || it.value().toDouble() < 0)
                return id + ": invalid bowl-neck body measurement";
        for (const auto *axis : {"X", "Y"})
            if (neck.value(QString("min") + axis).toDouble() > neck.value(QString("max") + axis).toDouble())
                return id + ": reversed bowl-neck limits";
        for (auto it = probe.begin(); it != probe.end(); ++it)
            if (!QStringList{"offsetX", "minCrossings"}.contains(it.key()) || !it.value().isDouble() ||
                !std::isfinite(it.value().toDouble()) || it.value().toDouble() < 0)
                return id + ": invalid bowl-neck cut policy";
    }
    if (shape == "open-head") {
        const auto head = body.value("head").toObject(), projection = cuts.value("head").toObject();
        for (auto it = head.begin(); it != head.end(); ++it)
            if (!QStringList{"minFlankRise", "maxFlankRise"}.contains(it.key()) || !it.value().isDouble() ||
                !std::isfinite(it.value().toDouble()) || it.value().toDouble() < 0)
                return id + ": open-head body accepts nonnegative flank rise measurements";
        if (head.value("maxFlankRise").toDouble(.8) < head.value("minFlankRise").toDouble(.15))
            return id + ": open-head flank rise limits are reversed";
        for (auto it = projection.begin(); it != projection.end(); ++it)
            if (!QStringList{"depth", "reach"}.contains(it.key()) || !it.value().isDouble() ||
                !std::isfinite(it.value().toDouble()) || it.value().toDouble() <= 0)
                return id + ": open-head projection accepts positive depth/reach in head heights";
    }
    return {};
}

// Reuse the established range/type checks during migration. This translation
// exists only in validation; execution never falls back to a legacy method.
static QJsonObject sharedShapeValidationForm(QJsonObject form, const QString &phase) {
    const auto shape = form.value("body").toObject().value("shape");
    form = sharedShapeProjectionForm(form);
    form.remove("body");
    form.remove("cuts");
    if (shape == "component-arm")
        form.insert("method", "detached-arm");
    if (shape == "tapered-arm")
        form.insert("method", "folded-arm");
    if (shape == "upright-stem")
        form.insert("method", "alef-stem");
    if (shape == "layered-head")
        form.insert("method", "hah");
    if (shape == "counter-attachment" && phase == "initial")
        form.insert("method", "counter-ligature");
    if (shape == "forked-bowl")
        form.insert("method", "bowl-fork");
    if (shape == "stemmed-bowl")
        form.insert("method", "stem-entry");
    if (shape == "enclosed-lips" && phase == "initial")
        form.insert("method", "enclosed-head");
    else if (shape == "tooth-cluster" && phase != "medial")
        form.insert("method", "seen-teeth");
    else if (shape == "contour-route")
        form.insert("method", "walk");
    else if (shape == "tail-branch")
        form.insert("method", "tail-entry");
    else if (shape == "rising-return")
        form.insert("method", "rising-tail");
    else if (shape == "folded-return")
        form.insert("method", "folded-bowl");
    else if (shape == "horizontal-counters")
        form.insert("method", "heh-initial-counters");
    else if (shape == "suspended-loop")
        form.insert("method", "initial-loop-head");
    else if (shape == "bowl-turn")
        form.insert("method", "qaf");
    else if (shape == "bowl-neck" || shape == "open-bowl")
        form.insert("method", "noon");
    else if (shape == "profile-neck")
        form.insert("method", phase == "initial" ? "kaf" : "waw");
    else if (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge")
        form.insert("method", phase == "initial" ? "initial-counter" : "final-counter");
    else if (shape == "counter-loop" && phase == "initial")
        form.insert("method", "initial-loop-body");
    return form;
}

// A terminal attachment is declared by the form. No template names or letter
// identities in the planner decide when a body changes its cut topology.
QJsonObject descendingStemDescription(const QJsonObject& config, const QJsonObject& form) {
    return form.contains("body") ? formDescription(config, form, "body").value("stem").toObject()
                                    : form.value("descendingStem").toObject();
}

QJsonObject medialLamShortExit(const QJsonObject& config) {
    if (config.value("_disableLamShortExit").toBool()) return {};
    for (const auto& value : config.value("medial").toArray()) {
        const auto form = value.toObject();
        if (formDescription(config, form, "body").value("shape") == "forked-stem")
            return formDescription(config, form, "cuts").value("shortExit").toObject();
    }
    return {};
}

QJsonObject bodyConstraintForm(const QJsonObject& config, QJsonObject form) {
    if (!form.contains("body")) return form;
    const auto resolved = resolvedBodyForm(config, form);
    const auto body = resolved.value("body").toObject();
    const auto cuts = resolved.value("cuts").toObject();
    if (sharedShapeDescription(body)) {
        form = sharedShapeProjectionForm(resolved);
    }
    if (toothDescription(body)) form.insert("tightJoin", cuts.value("tightJoin") == "shared-boundary");
    if (body.value("features").isObject()) form.insert("bodyFeatures", body.value("features"));
    if (body.value("counters").isDouble()) form.insert("bodyCounters", body.value("counters"));
    const auto stem = body.value("stem").toObject();
    if (stem.contains("headLip")) form.insert("_bodyPenWidth", stem.value("penWidth"));
    return form;
}

QJsonArray finalRetryCandidates(const QJsonArray& forms, QHash<QString, QString>& parents) {
    QJsonArray candidates;
    parents.clear();
    for (const auto& value : forms) {
        auto form = value.toObject();
        if (!form.contains("finalRetry")) {
            candidates.append(value);
            continue;
        }
        auto retry = form.take("finalRetry").toObject();
        const auto parent = form.value("id").toString();
        const auto key = parent + "#final-retry";
        // Keep separate search keys for the two sets of body evidence.
        retry.insert("id", key);
        retry.insert("_finalRetryFor", parent);
        retry.insert("current", form.value("current"));
        retry.insert(form.value("onIncomplete").toBool() ? "onIncomplete" : "onFinalRetry", true);
        if (form.value("onUnresolved").toBool()) retry.insert("onUnresolved", true);
        retry.insert("tryNext", true);
        candidates.append(retry);
        candidates.append(form);
        parents.insert(key, parent);
    }
    return candidates;
}

const PreparedPassRules& preparePassRules(const QJsonObject& config, bool recoverTeeth,
                                                OutlineSearchCache& cache) {
    for (const auto& rules : cache.rules)
        if (rules->config == config && rules->recoverTeeth == recoverTeeth) return *rules;
    auto prepared = std::make_unique<PreparedPassRules>();
    prepared->config = config;
    prepared->recoverTeeth = recoverTeeth;
    for (const auto* position : {"initial", "medial", "final"}) {
        for (const auto& value : config.value(position).toArray()) {
            auto rule = bodyConstraintForm(config, value.toObject());
            prepared->constraints.insert(rule.value("id").toString(), rule);
            if (QString(position) == "medial" || rule.contains("detachedArm")) continue;
            if (rule.value("retryWithVariants").toBool()) rule.insert("tryNext", true);
            if (!recoverTeeth && rule.value("onPartial").toBool()) continue;
            if (QString(position) == "initial") {
                rule.insert("first", true);
                rule.insert("bodyBoundary", "current");
            } else {
                rule.insert("next", rule.take("current"));
                rule.insert("last", true);
                rule.insert("bodyBoundary", "next");
                prepared->finals.append(rule);
                if (rule.value("body").toObject().value("shape") == "upright-stem")
                    prepared->alefTurns.append(rule);
            }
            prepared->boundaries.append(rule);
        }
    }
    const auto& result = *prepared;
    cache.rules.push_back(std::move(prepared));
    return result;
}

QJsonObject terminalArmRecipe(const QJsonObject& config, const QJsonObject& form) {
    if (form.contains("terminalArmRecovery")) return form.value("terminalArmRecovery").toObject();
    // Adapt saved recipes that used implicit alef selectors and arm profiles.
    const auto saved = form.value("foldedArmRecovery").toObject();
    if (saved.isEmpty()) return {};
    const auto body = formDescription(config, form, "body");
    return {{"prefixForm", saved.value("prefixForm")}, {"letters", body.value("alefLetters")},
            {"attached", QJsonObject{{"form", body.value("alefArm")}, {"variant", saved.value("attachedVariant")},
                                     {"maxTipGap", .75}, {"minRise", 3},
                                     {"prefixSection", QJsonObject{{"turnAngle", saved.value("entryTurnAngle")}}}}},
            {"detached", QJsonObject{{"variant", saved.value("detachedVariant")}}},
            {"cuts", QJsonObject{{"boundaries", QJsonObject{{"right", "preserve"}, {"left", "preserve"}}}}}};
}

// The cutter alternates between the full declarations and the smaller sets
// used by the search and recovery passes. Keep those programs ready across
// subwords. Compare declarations by value so template previews see edits.
const BodyCutProgram& bodyCutProgram(const QJsonObject& config) {
    constexpr std::array sections{"initial", "medial", "final"};
    using DeclarationLists = std::array<QJsonArray, sections.size()>;
    const DeclarationLists declarations{config.value(sections[0]).toArray(), config.value(sections[1]).toArray(),
                                        config.value(sections[2]).toArray()};
    static thread_local std::array<std::pair<DeclarationLists, BodyCutProgram>, 4> programs;
    static thread_local size_t nextProgram = 0;
    for (const auto& [compiled, program] : programs)
        if (compiled == declarations) return program;
    auto& [compiled, program] = programs[nextProgram];
    nextProgram = (nextProgram + 1) % programs.size();
    program = {};
    for (size_t position = 0; position < declarations.size(); ++position) {
        for (const auto& value : declarations[position]) {
            const auto form = value.toObject();
            if (!form.contains("body") || !form.contains("cuts")) continue;
            const auto id = form.value("id").toString();
            const auto body = formDescription(config, form, "body");
            QString nextLetters;
            if (position == 0) nextLetters = form.value("ligatureWith").toString();
            if (position == 1) nextLetters = form.value("sharedFoldWithFinal").toString();
            program.forms.insert(id, {body, formDescription(config, form, "cuts"), bodyDescriptionKey(body),
                                      nextLetters});
            for (QChar letter : form.value("current").toString())
                program.formsByPosition[position][letter].append(id);
            if (position != 1) continue;
            const auto recipe = terminalArmRecipe(config, form);
            if (recipe.isEmpty()) continue;
            const auto prefix = namedLetterForm(config, recipe.value("prefixForm").toString());
            if (prefix.isEmpty()) continue;
            const auto recoveredBody = formDescription(config, prefix, "body");
            auto followers = recipe.value("letters").toString();
            for (const auto* mode : {"attached", "detached"}) {
                const auto final = namedLetterForm(config, recipe.value(mode).toObject().value("form").toString());
                followers += final.value("current").toString();
            }
            program.recoveryBodies.insert(id, {recoveredBody,
                withOverrides(formDescription(config, form, "cuts"), recipe.value("cuts").toObject()),
                bodyDescriptionKey(recoveredBody), followers});
        }
    }
    compiled = declarations;
    return program;
}

QByteArray bodyDescriptionKey(const QJsonObject& body) {
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QJsonObject bodyCutPolicy(const QJsonObject& description, const QString& letters, int slot) {
    auto policy = description;
    const auto terminal = policy.take("beforeFinal").toObject();
    if (hasFollowingLetter(letters, slot, terminal.value("letters").toString(), true))
        policy = withOverrides(policy, terminal);
    return policy;
}

} // namespace MedinaCutter

QString PageAnalysisResult::prepareLetterForms(QJsonObject& config) {
    if (config.value("version").toInt() != 2 || !config.value("initial").isArray() ||
        !config.value("medial").isArray() || !config.value("final").isArray()) {
        return "Expected a version 2 config with initial, medial and final lists";
    }
    const auto strokeWidth = config.value("strokeWidth");
    if (!strokeWidth.isUndefined() && (!strokeWidth.isDouble() ||
        !std::isfinite(strokeWidth.toDouble()) || strokeWidth.toDouble() <= 0))
        return "strokeWidth must be a positive width in page units";
    for (const auto* obsolete : {"preferred", "fallback", "medialBodies"})
        if (config.contains(obsolete)) return QString("Obsolete template section: %1").arg(obsolete);
    if (config.contains("recoveryChecks")) {
        const auto checks = config.value("recoveryChecks").toObject();
        const auto bowl = checks.value("finalBowl").toObject();
        const auto valid = [](const QJsonValue& x) {
            return x.isDouble() && std::isfinite(x.toDouble()) && x.toDouble() > 0;
        };
        if (!valid(checks.value("connectorMargin")) || !valid(checks.value("connectorBend")) ||
            bowl.value("letters").toString().isEmpty() || !valid(bowl.value("minWidth")) ||
            !valid(bowl.value("minHeight")) || !valid(bowl.value("tipTolerance")))
            return "Invalid recovery ownership measurements";
        if (checks.contains("longConnectorWidths") && !valid(checks.value("longConnectorWidths")))
            return "longConnectorWidths needs a positive dimension in stroke widths";
        if (checks.contains("bodyExtents") && !checks.value("bodyExtents").isArray())
            return "bodyExtents must be an array";
        for (const auto& v : checks.value("bodyExtents").toArray()) {
            const auto bounds = v.toObject();
            const auto position = bounds.value("position").toString();
            if ((bounds.contains("protectHead") &&
                 (!bounds.value("protectHead").isBool() || !bounds.contains("lips") ||
                  (position != "initial" && position != "medial"))) ||
                (bounds.contains("minHeadRiseWidths") &&
                 (position != "final" || !valid(bounds.value("minHeadRiseWidths")))) ||
                (bounds.contains("onUnresolved") && !bounds.value("onUnresolved").isBool()) ||
                (bounds.contains("lips") && (!valid(bounds.value("lips").toObject().value("minRun")) ||
                                             !valid(bounds.value("lips").toObject().value("minGap")))) ||
                (bounds.contains("maxWidthToHeight") && !valid(bounds.value("maxWidthToHeight"))) ||
                (bounds.contains("template") && bounds.value("template").toString().isEmpty()) ||
                bounds.value("letters").toString().isEmpty() ||
                !QStringList{"initial", "medial", "final"}.contains(position) || !valid(bounds.value("minWidth")) ||
                !valid(bounds.value("minHeight")))
                return "Invalid finished letter-body dimensions";
        }
    }
    if (config.contains("retryUnresolvedForms") && !config.value("retryUnresolvedForms").isBool())
        return "retryUnresolvedForms must be true or false";
    if (config.contains("unresolvedShapeFrames")) {
        const auto frames = config.value("unresolvedShapeFrames").toArray();
        if (!config.value("unresolvedShapeFrames").isArray() || frames.size() > 18)
            return "unresolvedShapeFrames needs at most 18 [width, slant, optional angle] measurements";
        for (const auto& value : frames) {
            const auto pair = value.toArray();
            if ((pair.size() != 2 && pair.size() != 3) || !pair[0].isDouble() || !pair[1].isDouble() ||
                (pair.size() == 3 && (!pair[2].isDouble() || !(std::abs(pair[2].toDouble()) <= 18))) ||
                !(pair[0].toDouble() >= .55 && pair[0].toDouble() <= 1.7 && std::abs(pair[1].toDouble()) <= .45))
                return "Invalid shape frame width (0.55–1.7), shear (−0.45–0.45) or angle (−18°–18°)";
        }
    }
    for (const auto* field : {"unresolvedScaleCenters", "unresolvedTiltAngles"}) {
        if (!config.contains(field))
            continue;
        const auto values = config.value(field).toArray();
        if (!config.value(field).isArray() || values.size() > 6)
            return QString(field) + " needs an array of at most six measurements";
        for (const auto& item : values) {
            const double value = item.toDouble();
            if (!item.isDouble() || !std::isfinite(value) ||
                (QString(field) == "unresolvedScaleCenters" ? (value < .65 || value > 1.4) : std::abs(value) > 15))
                return QString(field) + " is outside the supported scale or tilt range";
        }
    }
    if (config.contains("unresolvedInitialShaft")) {
        const auto shaft = config.value("unresolvedInitialShaft").toObject();
        if (shaft.value("letters").toString().isEmpty()) return "The initial shaft needs a letter selector";
        for (const auto* field : {"minHeight", "maxWidthToHeight", "edgeTolerance"}) {
            const auto value = shaft.value(field);
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() <= 0)
                return QString("Invalid initial shaft dimension: ") + field;
        }
    }
    if (config.contains("reconsiderScale")) {
        const auto retry = config.value("reconsiderScale").toObject();
        if (!(retry.value("fraction").toDouble() > 0 && retry.value("fraction").toDouble() < .5 &&
              retry.value("boundaryTolerance").toDouble() > 0))
            return "reconsiderScale needs a fraction between zero and one half and a positive boundary tolerance";
    }
    if (config.contains("reconsiderFinalAttachments") &&
        (!config.value("reconsiderFinalAttachments").isString() || config.value("reconsiderFinalAttachments").toString().isEmpty()))
        return "reconsiderFinalAttachments needs a nonempty final letter selector";
    if (config.contains("reconsiderForms")) {
        const auto groups = config.value("reconsiderForms").toObject();
        if (groups.isEmpty())
            return "reconsiderForms needs positional letter selectors";
        for (auto it = groups.begin(); it != groups.end(); ++it)
            if (!QStringList{"initial", "medial", "final"}.contains(it.key()) || !it.value().isString() ||
                it.value().toString().isEmpty())
                return "reconsiderForms supports nonempty initial, medial and final selectors";
    }
    static const auto bundled = readFormConfig(":/templates/letter-forms.json");
    const auto defaults = bundled.value("kashidaDefaults").toObject();
    auto problem = invalidKashidaSettings(config.value("kashidaDefaults"), defaults, "kashidaDefaults");
    if (!problem.isEmpty()) return problem;
    const auto effective = withOverrides(defaults, config.value("kashidaDefaults").toObject());
    for (const auto* phase : {"initial", "medial", "final"}) {
        for (const auto& value : config.value(phase).toArray()) {
            const auto form = value.toObject();
            if (!form.contains("finalRetry")) continue;
            const auto retry = form.value("finalRetry").toObject();
            const auto id = form.value("id").toString();
            if (QString(phase) != "final" || form.value("onFinalRetry").toBool() ||
                !retry.value("body").isObject() || !retry.value("cuts").isObject())
                return id + ": finalRetry needs a final template and a body and cuts declaration";
            for (auto setting = retry.begin(); setting != retry.end(); ++setting)
                if (!QStringList{"body", "cuts", "terminalAttachment", "tightJoin", "returningStroke",
                                 "ownsLeftTip"}.contains(setting.key()))
                    return id + ": unknown finalRetry setting " + setting.key();
        }
    }
    QHash<QString, QString> retryParents;
    const auto finals = finalRetryCandidates(config.value("final").toArray(), retryParents);
    QSet<QString> ids;
    for (const auto* phase : {"initial", "medial", "final"}) {
        const auto forms = QString(phase) == "final" ? finals : config.value(phase).toArray();
        for (const auto& entry : forms) {
            if (!entry.isObject()) return QString(phase) + ": every template must be an object";
            auto rule = entry.toObject();
            if (!rule.contains("body") && !rule.contains("assembly"))
                return rule.value("id").toString() + ": use a body/cuts declaration or an assembly recipe";
            QString sharedError;
            const auto sharedBody = formDescription(config, rule, "body", &sharedError);
            if (!sharedError.isEmpty())
                return sharedError;
            if (sharedShapeDescription(sharedBody)) {
                const auto resolved = resolvedBodyForm(config, rule);
                sharedError = validateSharedShape(resolved, phase);
                if (!sharedError.isEmpty())
                    return sharedError;
                rule = sharedShapeValidationForm(resolved, phase);
            }
            const auto id = rule.value("id").toString().trimmed();
            if (id.isEmpty() || ids.contains(id)) return "Each template needs a unique nonempty id";
            ids.insert(id);
            if (!rule.value("current").isString() || rule.value("current").toString().isEmpty())
                return id + ": current must name the letters represented by this form";
            for (const auto* obsolete : {"next", "first", "last", "bodyBoundary", "deferAfterMedial"})
                if (rule.contains(obsolete)) return id + ": position is determined by its section; remove " + obsolete;
            for (const auto* key : {"tryNext", "retryOnPartial", "pairedOnly", "tightJoin", "sharedEntry", "sharedBoundary", "onPartial", "onIncomplete","onUnresolved","reclaimEntry","returningStroke","retryWithVariants","onHeadConflict"})
                if (rule.contains(key) && !rule.value(key).isBool()) return id + ": " + key + " must be true or false";
            if (rule.contains("sharedEntry") && QString(phase) != "medial")
                return id + ": sharedEntry belongs to a medial ligature form";
            if (rule.contains("sharedBoundary") && QString(phase) != "final")
                return id + ": sharedBoundary belongs to a final ligature form";
            if (rule.contains("sharedBoundaryWith") && (QString(phase) != "final" ||
                !rule.value("sharedBoundary").toBool() || !rule.value("sharedBoundaryWith").isString() ||
                rule.value("sharedBoundaryWith").toString().isEmpty()))
                return id + ": sharedBoundaryWith needs a final shared boundary and its ligature letters";
            const auto footAngle = rule.value("pairedCounterCorners").toObject().value("maxFootInwardAngle");
            if (!footAngle.isUndefined() && (!footAngle.isDouble() ||
                !std::isfinite(footAngle.toDouble()) || footAngle.toDouble() < 0 || footAngle.toDouble() >= 90))
                return id + ": maxFootInwardAngle must be an angle in degrees from 0 up to, but excluding, 90";
            const auto positive = [](const QJsonValue& value) {
                return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0;
            };
            if (rule.contains("terminalArmRecovery")) {
                const auto recipe = rule.value("terminalArmRecovery").toObject();
                const auto findFinal = [&](const QJsonValue& id) {
                    for (const auto& entry : config.value("final").toArray())
                        if (entry.toObject().value("id") == id) return entry.toObject();
                    return QJsonObject{};
                };
                const auto prefix = findFinal(recipe.value("prefixForm"));
                const auto prefixLetters = prefix.value("current").toString();
                const auto letters = rule.value("current").toString();
                if (QString(phase) != "medial" || rule.contains("foldedArmRecovery") || prefix.isEmpty() ||
                    !prefix.value("body").isObject() || !prefix.value("cuts").isObject() ||
                    std::any_of(letters.begin(), letters.end(), [&](QChar letter) { return !prefixLetters.contains(letter); }) ||
                    (!recipe.contains("attached") && !recipe.contains("detached")) ||
                    (recipe.contains("cuts") && !recipe.value("cuts").isObject()))
                    return id + ": terminalArmRecovery needs a final prefix profile for the same letters and an arm mode";
                for (const auto* key : {"attached", "detached"}) {
                    if (!recipe.contains(key)) continue;
                    const auto mode = recipe.value(key).toObject();
                    const auto arm = resolvedBodyForm(config, findFinal(mode.value("form")));
                    const auto shape = arm.value("body").toObject().value("shape");
                    const bool attached = QLatin1String(key) == "attached";
                    if (shape != (attached ? "tapered-arm" : "component-arm") ||
                        mode.value("variant").toString().trimmed().isEmpty() ||
                        (mode.contains("prefixSection") && !mode.value("prefixSection").isObject()) ||
                        (mode.contains("minRise") && (!attached || !positive(mode.value("minRise")))) ||
                        (mode.contains("maxTipGap") && !positive(mode.value("maxTipGap"))))
                        return id + ": invalid terminalArmRecovery." + key + " arm profile, variant or cut settings";
                }
            }
            const auto rimValue = loopSettings(rule).value("rimAnchors");
            if (!rimValue.isUndefined()) {
                const auto rim = rimValue.toObject();
                const auto point = [](const QJsonValue& value) {
                    const auto p = value.toArray();
                    return p.size() == 2 && p[0].isDouble() && p[1].isDouble() && std::isfinite(p[0].toDouble()) &&
                           std::isfinite(p[1].toDouble());
                };
                if (QString(phase) != "medial" || !rimValue.isObject() || !positive(rim.value("radius")) ||
                    !positive(rim.value("rimMargin")) || (QString(phase) != "initial" && !point(rim.value("entry"))) ||
                    (QString(phase) != "final" && !point(rim.value("exit"))) ||
                    (rim.contains("shaftRise") && !positive(rim.value("shaftRise"))))
                    return id + ": invalid counter-relative rim anchors, radius, margin or shaft rise";
            }
            if (hasCounterOutline(rule)) {
                const auto outline = rule.value("bodyOutline").toObject();
                const auto interval = [](const QJsonValue& value, bool positive = false) {
                    const auto bounds = value.toArray();
                    return bounds.size() == 2 && bounds[0].isDouble() && bounds[1].isDouble() &&
                           std::isfinite(bounds[0].toDouble()) && std::isfinite(bounds[1].toDouble()) &&
                           bounds[0].toDouble() <= bounds[1].toDouble() && (!positive || bounds[0].toDouble() > 0);
                };
                if (!rule.value("onUnresolved").toBool() || !interval(outline.value("aspect"), true) ||
                    (outline.value("counters") != 1 && outline.value("counters") != 2))
                    return id + ": bodyOutline needs one or two counters and a positive aspect range";
                if (rule.contains("requiresExitTemplate") && (QString(phase) != "medial" ||
                    rule.value("requiresExitTemplate").toString().isEmpty()))
                    return id + ": the counter outline needs a named final attachment";
                if (rule.contains("rimOffset") && (!positive(rule.value("rimOffset")) || !rule.contains("requiresExitTemplate")))
                    return id + ": a rim offset needs a positive counter-height distance and a final attachment";
                for (const auto* side : {"top", "bottom", "left", "right"})
                    if (!interval(outline.value("rim").toObject().value(side)))
                        return id + ": bodyOutline.rim needs four ordered ranges in counter heights";
                for (const auto* side : {"entry", "exit"}) {
                    const bool required =
                        QString(side) == "entry" ? QString(phase) != "initial" : QString(phase) != "final";
                    const auto attachment = outline.value(side).toObject();
                    if (outline.contains(side) != required ||
                        (required && (!interval(attachment.value("x")) || !interval(attachment.value("y")) ||
                                      !interval(attachment.value("width"), true))))
                        return id + ": invalid bodyOutline attachment for this position";
                }
                continue;
            }
            if (rule.contains("onFinalRetry") && (!rule.value("onFinalRetry").isBool() || rule.contains("onIncomplete")))
                return id + ": onFinalRetry must be a separate boolean retry phase";
            QString descriptionError;
            if (rule.contains("lamBody") || rule.contains("lamCuts"))
                return id + ": use body / cuts for declared forms";
            const auto declaredCuts = formDescription(config, rule, "cuts", &descriptionError);
            const auto declaredBody = formDescription(config, rule, "body", &descriptionError);
            if (!descriptionError.isEmpty()) return descriptionError;
            if (rule.contains("body") != rule.contains("cuts") ||
                (rule.contains("body") && (declaredBody.isEmpty() || declaredCuts.isEmpty())))
                return id + ": declared forms require both body and cuts";
            const auto method = rule.value("method").toString();
            if (method == "descending-stem" && (QString(phase) != "initial" ||
                !positive(rule.value("descendingStem").toObject().value("penWidth")) ||
                !positive(rule.value("descendingStem").toObject().value("minRise"))))
                return id + ": an initial descending stem needs a positive pen width and rise";
            const auto headLip = descendingStemDescription(config, rule).value("headLip");
            if (!headLip.isUndefined()) {
                const auto lip = headLip.toObject();
                if ((method != "descending-stem" && declaredBody.value("shape") != "descending-stem") || !rule.contains("ligatureWith") || !positive(lip.value("minRun")) ||
                    !positive(lip.value("maxRun")) || lip.value("maxRun").toDouble() <= lip.value("minRun").toDouble() ||
                    !positive(lip.value("maxDrop")))
                    return id + ": a stacked head needs a paired letter, ordered lip reach and positive drop";
            }
            if (rule.contains("terminalAttachment") &&
                (QString(phase) != "final" || !rule.value("terminalAttachment").isBool() ||
                 !rule.value("onFinalRetry").toBool()))
                return id + ": terminalAttachment belongs to a recovery-only final form";
            if (rule.contains("stemFoot") && (method != "bowl-fork" || !rule.value("stemFoot").isBool() ||
                                              (rule.contains("penWidth") && !positive(rule.value("penWidth")))))
                return id + ": stemFoot requires a bowl fork and a positive pen width";
            if (rule.contains("bridge") &&
                (method != "final-counter" || !positive(rule.value("bridge").toObject().value("maxWidth"))))
                return id + ": a final counter bridge needs a positive maximum width in counter heights";
            if (rule.contains("terminalBowl") &&
                (method != "seen-teeth" || rule.value("side") != "right" || !rule.value("terminalBowl").isBool()))
                return id + ": terminalBowl requires a final three-tooth form";
            if (rule.contains("roundedEntry") &&
                (!rule.value("terminalBowl").toBool() || !rule.value("roundedEntry").isBool()))
                return id + ": roundedEntry requires a terminal bowl";
            const auto neckWalk = rule.value("head").toObject().value("neckWalk");
            if (!neckWalk.isUndefined()) {
                if (method != "initial-loop-head" || !neckWalk.isObject() || neckWalk.toObject().value("top").toArray().isEmpty())
                    return id + ": head.neckWalk requires an initial loop head and a top route";
                problem = invalidWalkSettings(neckWalk.toObject(), id + ".head.neckWalk");
                if (!problem.isEmpty()) return problem;
            }
            for (const auto* owner : {"counter", "loopBoundaries"}) {
              for (const auto* side : {"entryCorner", "exitCorner"}) {
                const auto cornerValue = rule.value(owner).toObject().value(side);
                if (!cornerValue.isUndefined() && !cornerValue.isObject())
                    return id + ": " + side + " must be an object";
                const auto corner = cornerValue.toObject();
                const auto placement = corner.value("placement");
                if (!placement.isUndefined() && (!placement.isString() ||
                    !QStringList{"upper", "top-right", "right-corner-pair"}.contains(placement.toString())))
                    return id + ": " + side + ".placement must be upper, top-right or right-corner-pair";
                if (QString(side) == "exitCorner" && (!placement.isUndefined() && placement != "upper"))
                    return id + ": exitCorner.placement supports upper or the default lower neck";
                const auto offset = corner.value("strokeOffsetX");
                if (!offset.isUndefined() && (placement != "right-corner-pair" || !offset.isDouble() ||
                    !std::isfinite(offset.toDouble())))
                    return id + ": " + side + ".strokeOffsetX needs a finite distance and right-corner-pair";
                const auto distance = corner.value("maxDistance");
                if (!distance.isUndefined() && (!distance.isDouble() ||
                    !std::isfinite(distance.toDouble()) || distance.toDouble() <= 0))
                    return id + ": " + side + ".maxDistance must be positive, in counter heights";
                for (const auto* key : {"minStepAngle", "minRoundedAngle"}) {
                    const auto angle = corner.value(key);
                    if (!angle.isUndefined() && (!placement.isUndefined() || !angle.isDouble() ||
                        !(angle.toDouble() > 0 && angle.toDouble() < 180)))
                        return id + ": " + side + "." + key + " needs a lower corner and an angle between 0 and 180";
                }
                const auto sampleValley = corner.value("sampleValley");
                if (!sampleValley.isUndefined() && (!placement.isUndefined() || !sampleValley.isBool()))
                    return id + ": " + side + ".sampleValley needs a boolean and a lower corner";
                const auto counterY = corner.value("maxCounterY");
                if (!counterY.isUndefined() && (placement != "upper" || !counterY.isDouble() ||
                    !std::isfinite(counterY.toDouble()) || counterY.toDouble() < 0 || counterY.toDouble() > .5))
                    return id + ": " + side + ".maxCounterY needs upper placement and a value from 0 to 0.5";
                for (const auto* key : {"required", "normalCut"})
                    if (corner.contains(key) && !corner.value(key).isBool())
                        return id + ": " + side + "." + key + " must be true or false";
              }
            }
            if (rule.contains("ownsLeftTip") && (QString(phase) != "final" || (rule.value("start") != "left" &&
                declaredBody.value("shape") != "terminal-return" && declaredBody.value("shape") != "terminal-fork") ||
                !rule.value("ownsLeftTip").isBool()))
                return id + ": ownsLeftTip needs a final form starting from its own left tip";
            if (rule.contains("retainBodyFeatures") && (QString(phase) != "final" ||
                !rule.value("retainBodyFeatures").isBool()))
                return id + ": retainBodyFeatures must be a boolean on a final form";
            if (rule.contains("retainEntryCrest") && (QString(phase) != "final" ||
                !rule.value("retainEntryCrest").isBool() || !rule.value("ownsLeftTip").toBool()))
                return id + ": retainEntryCrest requires a final form that owns its left tip";
            if (rule.contains("bodyFeatures")) {
                if (QString(phase) == "medial" || !rule.value("bodyFeatures").isObject())
                    return id + ": bodyFeatures requires an initial or final body";
                const auto features = rule.value("bodyFeatures").toObject();
                for (auto feature = features.begin(); feature != features.end(); ++feature)
                    if (!QStringList{"minHeight", "minWidthToHeight", "maxWidthToHeight", "maxUpperPeaks"}.contains(feature.key()) ||
                        !feature.value().isDouble() || !std::isfinite(feature.value().toDouble()) ||
                        feature.value().toDouble() <= 0 ||
                        (feature.key() == "maxUpperPeaks" && feature.value().toDouble() != std::floor(feature.value().toDouble())))
                        return id + ": bodyFeatures needs positive dimensions and an integer peak limit";
                if (features.value("minWidthToHeight").toDouble() >
                    features.value("maxWidthToHeight").toDouble(INFINITY))
                    return id + ": minimum body aspect exceeds maximum";
            }
            if (rule.contains("tooth")) return id + ": medial teeth must use body / cuts";
            if (rule.contains("onPartial") && (QString(phase) != "initial" || method != "walk"))
                return id + ": onPartial requires an initial contour walk";
            if (rule.contains("recoveryStage")) {
                const auto stage = rule.value("recoveryStage");
                if (QString(phase) != "medial" || !toothDescription(declaredBody) || !stage.isDouble() ||
                    !std::isfinite(stage.toDouble()) || stage.toDouble() != std::floor(stage.toDouble()) ||
                    stage.toInt() < 1 || stage.toInt() > 3)
                    return id + ": recoveryStage must be 1, 2 or 3 on a declared medial tooth";
            }
            if (toothDescription(declaredBody)) {
                if (QString(phase) != "medial") return id + ": tooth shapes are medial bodies";
                for (const auto* key : {"method", "tightJoin"})
                    if (rule.contains(key)) return id + ": declared tooth geometry and policies belong in body / cuts";
                const bool pair = toothPairDescription(declaredBody);
                const QStringList dimensions = pair ? QStringList{"minPairDistance", "maxPairDistance", "minFlankRise",
                    "flankDrop", "maxTipOverhang", "minWidth", "maxWidth"} :
                    QStringList{"minPairDistance", "maxPairDistance", "minBendTurn"};
                for (auto key = declaredBody.begin(); key != declaredBody.end(); ++key) {
                    if (dimensions.contains(key.key())) {
                        if (!key.value().isDouble() || !std::isfinite(key.value().toDouble()) || key.value().toDouble() < 0)
                            return id + ": tooth body measurements must be finite and nonnegative";
                    } else if (key.key() != "shape" && key.key() != "baselineEntry" &&
                               !(pair && key.key() == "allowFlatBend"))
                        return id + ": unknown tooth body setting " + key.key();
                }
                if (!positive(declaredBody.value("maxPairDistance")) ||
                    declaredBody.value("minPairDistance").toDouble() >= declaredBody.value("maxPairDistance").toDouble() ||
                    (pair && (!positive(declaredBody.value("maxWidth")) ||
                     declaredBody.value("minWidth").toDouble() >= declaredBody.value("maxWidth").toDouble())))
                    return id + ": tooth body measurements need ordered pair-distance and width limits";
                if (!pair && (!declaredBody.contains("minBendTurn") || declaredBody.value("minBendTurn").toDouble() > 1))
                    return id + ": body.minBendTurn must be between 0 and 1";
                if (declaredBody.contains("allowFlatBend") && !declaredBody.value("allowFlatBend").isBool())
                    return id + ": body.allowFlatBend must be boolean";
                const auto entry = declaredBody.value("baselineEntry").toObject();
                if (declaredBody.contains("baselineEntry") && declaredBody.value("baselineEntry") != QJsonValue(false)) {
                    if (!positive(entry.value("minRise"))) return id + ": body.baselineEntry needs a positive minRise";
                    for (auto key = entry.begin(); key != entry.end(); ++key)
                        if (key.key() != "minRise" && (!pair || key.key() != "required" || !key.value().isBool()))
                            return id + ": body.baselineEntry supports minRise, plus required on a tooth-pair";
                }
                if (declaredCuts.value("offsetUnit") != "local-stroke" ||
                    !QStringList{"shared-boundary", "separate"}.contains(declaredCuts.value("tightJoin").toString()))
                    return id + ": tooth cuts need local-stroke offsets and a tightJoin policy";
                const auto boundaries = declaredCuts.value("boundaries").toObject();
                if (boundaries.size() != 2) return id + ": tooth cuts need exactly left and right boundaries";
                for (const auto* side : {"left", "right"})
                    if (boundaries.value(side) != (pair ? "tooth-normal" : "tooth-flank"))
                        return id + ": unsupported tooth boundary policy";
                if (pair) {
                    for (const auto* key : {"leftOffset", "rightOffset", "normalOffsetTolerance"}) {
                        const auto value = declaredCuts.value(key);
                        if ((QString(key) != "normalOffsetTolerance" || !value.isUndefined()) &&
                            (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0))
                            return id + ": tooth offsets must be nonnegative local stroke widths";
                    }
                } else if (!positive(declaredCuts.value("flankOffset")) || !positive(declaredCuts.value("maxSectionWidth")) ||
                    !QStringList{"contour-normal", "axis-projection", "contour-search"}.contains(declaredCuts.value("flankMode").toString()))
                    return id + ": tooth-flank cuts need a mode, positive flankOffset and maxSectionWidth";
                const auto turn = declaredCuts.value("baselineEntry").toObject();
                if (declaredCuts.contains("baselineEntry") && declaredCuts.value("baselineEntry") != QJsonValue(false)) {
                    if (entry.isEmpty() || !positive(turn.value("turnAngle")) || turn.value("turnAngle").toDouble() >= 90 ||
                        (turn.contains("minTurnRun") && (!pair || !positive(turn.value("minTurnRun")))))
                        return id + ": cuts.baselineEntry needs recognized entry geometry and a turn angle between 0 and 90";
                    for (auto key = turn.begin(); key != turn.end(); ++key)
                        if (key.key() != "turnAngle" && key.key() != "minTurnRun")
                            return id + ": cuts.baselineEntry supports turnAngle and minTurnRun";
                }
                QStringList policyKeys{"boundaries", "offsetUnit", "tightJoin", "baselineEntry"};
                policyKeys += pair ? QStringList{"leftOffset", "rightOffset", "normalOffsetTolerance"} :
                                     QStringList{"flankMode", "flankOffset", "maxSectionWidth"};
                for (auto key = declaredCuts.begin(); key != declaredCuts.end(); ++key)
                    if (!policyKeys.contains(key.key())) return id + ": unknown tooth cuts setting " + key.key();
            } else if (rule.contains("body") || rule.contains("cuts")) {
                if (!positive(declaredBody.value("minRise")) || !positive(declaredBody.value("maxWidthToHeight")) ||
                    !positive(declaredBody.value("minFlankSpan")) || declaredCuts.isEmpty())
                    return id + ": body needs positive flank/rise/aspect measurements and separate cuts";
                for (const auto* key : {"method", "top", "bottomProbe", "legacyLamRoutes", "shaft", "tailEntry", "descendingStem",
                                        "terminalArm", "detachedExit", "prefixTail", "bodyFeatures", "bodyCounters"})
                    if (rule.contains(key)) return id + ": lām geometry belongs in body / cuts, not " + key;
                const auto shape = declaredBody.value("shape").toString();
                const QStringList shapes = QString(phase) == "initial" ? QStringList{"contour-stem", "descending-stem"} :
                    QString(phase) == "medial" ? QStringList{"forked-stem", "turning-shaft", "detached-return", "attached-return"} :
                    QStringList{"terminal-bowl", "terminal-fork", "terminal-return"};
                if (!shapes.contains(shape)) return id + ": unknown body.shape for this position";
                const auto counters = declaredBody.value("counters");
                if (!counters.isUndefined() && counters != QJsonValue(false) &&
                    (!counters.isDouble() || !std::isfinite(counters.toDouble()) || counters.toDouble() < 0 ||
                     counters.toDouble() != std::floor(counters.toDouble())))
                    return id + ": body.counters needs a nonnegative integer or false";
                const auto checkRoute = [&](const QJsonValue& body, const QJsonValue& cuts, const QString& name) {
                    if (!body.isObject() || !cuts.isObject() || body.toObject().value("top").toArray().isEmpty())
                        return id + ": " + name + " needs body landmarks and a cut policy";
                    return invalidWalkSettings(withOverrides(body.toObject(), cuts.toObject()), id + "." + name);
                };
                if (shape == "contour-stem") {
                    problem = checkRoute(declaredBody.value("route"), declaredCuts.value("route"), "route");
                    if (!problem.isEmpty()) return problem;
                }
                if (shape == "forked-stem") {
                    const auto routes = declaredBody.value("exitRoutes").toArray(), cuts = declaredCuts.value("exitRoutes").toArray();
                    if (routes.isEmpty() || routes.size() != cuts.size()) return id + ": body/cut exitRoutes must have equal nonzero size";
                    for (int i = 0; i < routes.size(); ++i) {
                        problem = checkRoute(routes[i], cuts[i], "exitRoutes");
                        if (!problem.isEmpty()) return problem;
                    }
                    problem = checkRoute(declaredBody.value("entryRoute"), declaredCuts.value("entryRoute"), "entryRoute");
                    if (!problem.isEmpty()) return problem;
                    const auto placement = declaredCuts.value("placement").toObject();
                    if (!positive(placement.value("entryOverlap")) || !positive(placement.value("turnAngle")) ||
                        placement.value("turnAngle").toDouble() >= 90 || !placement.value("levelForkEntry").isBool())
                        return id + ": invalid cuts.placement";
                }
                const auto stem = declaredBody.value("stem").toObject();
                if ((shape == "descending-stem" || shape == "terminal-fork") &&
                    (!positive(stem.value("minRise")) || !positive(stem.value("penWidth"))))
                    return id + ": body.stem needs positive penWidth and minRise";
                if (shape == "terminal-bowl" && (!positive(stem.value("minRise")) || !positive(stem.value("maxNeckDistance")) ||
                    !positive(declaredCuts.value("section").toObject().value("turnAngle"))))
                    return id + ": invalid terminal bowl body / section";
                if (shape == "turning-shaft" && (!positive(declaredBody.value("shaft").toObject().value("minRise")) ||
                    !positive(declaredBody.value("shaft").toObject().value("maxWidthToHeight"))))
                    return id + ": invalid body.shaft";
                if (shape == "attached-return" && (declaredBody.value("terminalArm").toString().isEmpty() ||
                    !positive(declaredCuts.value("entryTurnAngle")) || declaredCuts.value("entryTurnAngle").toDouble() >= 90))
                    return id + ": invalid returning body / entry angle";
                if (shape == "detached-return" && declaredBody.value("detachedExit").toString().isEmpty())
                    return id + ": detached return needs exit letters";
                if (shape == "terminal-return" || shape == "detached-return") {
                    const auto tail = declaredBody.value("tail").toObject();
                    for (const auto* key : {"minRise", "minRun", "maxRun"})
                        if (!positive(tail.value(key))) return id + ": invalid body.tail";
                }
                if ((shape == "terminal-return" || shape == "detached-return" || shape == "terminal-fork") &&
                    !positive(declaredCuts.value("section").toObject().value("walkWidths")))
                    return id + ": section.walkWidths must be positive";
                const auto shortExit = declaredCuts.value("shortExit");
                if (!shortExit.isUndefined() && shortExit != QJsonValue(false)) {
                    if (!shortExit.isObject()) return "cuts.shortExit must be an object or false";
                    const auto settings = shortExit.toObject();
                    for (const auto* key : {"minHorizontalRun", "maxEntryGap", "minLengthRatio", "anchorTolerance"}) {
                        const auto value = settings.value(key);
                        const double minimum = QString(key) == "minLengthRatio" ? 1 : 0;
                        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() <= minimum)
                            return QString("cuts.shortExit.%1 must be greater than %2").arg(key).arg(minimum);
                    }
                    if (!settings.value("retryOnUnresolved").isBool())
                        return "cuts.shortExit.retryOnUnresolved must be true or false";
                    const auto hollow = settings.value("cutsAroundHollow");
                    if (!hollow.isUndefined() && !hollow.isBool())
                        return "cuts.shortExit.cutsAroundHollow must be true or false";
                    const auto tilt = settings.value("entryMaxTilt");
                    if (!tilt.isUndefined() && (!tilt.isDouble() || !std::isfinite(tilt.toDouble()) || tilt.toDouble() < 0 || tilt.toDouble() >= 90))
                        return "cuts.shortExit.entryMaxTilt must be an angle from 0 up to 90 degrees";
                }
                const auto arm = declaredBody.value("alefArm");
                if (!arm.isUndefined() &&
                    (!arm.isString() ||
                     formDescription(config, namedLetterForm(config, arm.toString()), "body").value("shape") !=
                         "tapered-arm" ||
                     declaredBody.value("alefLetters").toString().isEmpty()))
                    return id + ": body.alefArm needs a folded-arm body template and alefLetters";
                if (rule.contains("foldedArmRecovery")) {
                    const auto recovery = rule.value("foldedArmRecovery").toObject();
                    const auto prefix = namedLetterForm(config, recovery.value("prefixForm").toString());
                    const auto detached = recovery.value("detachedVariant").toString();
                    const auto attached = recovery.value("attachedVariant").toString();
                    const auto angle = recovery.value("entryTurnAngle");
                    if (QString(phase) != "medial" || shape != "forked-stem" || arm.isUndefined() ||
                        recovery.size() != 4 || detached.trimmed().isEmpty() || attached.trimmed().isEmpty() ||
                        detached == attached || !positive(angle) || angle.toDouble() >= 90 ||
                        formDescription(config, prefix, "body").value("shape") != "terminal-return")
                        return id + ": foldedArmRecovery needs a medial fork, a terminal return, two variant labels and an entry angle";
                    const auto prefixLetters = prefix.value("current").toString();
                    const auto letters = rule.value("current").toString();
                    if (std::any_of(letters.begin(), letters.end(), [&](QChar letter) {
                            return !prefixLetters.contains(letter);
                        }))
                        return id + ": foldedArmRecovery prefixForm must support the same letters";
                }
                const auto forkOffset = declaredCuts.value("forkOffset");
                if (!forkOffset.isUndefined() && !positive(forkOffset))
                    return id + ": cuts.forkOffset must be positive in page units";
                if (!QStringList{"tip-normal", "preserve"}.contains(declaredCuts.value("tightJoin").toString()) ||
                    declaredCuts.value("counterJoin") != "preserve-entry")
                    return id + ": cuts needs a tightJoin policy and counterJoin: preserve-entry";
                const auto terminal = declaredCuts.value("beforeFinal");
                if (!terminal.isUndefined() && terminal != QJsonValue(false) &&
                    (!terminal.isObject() || terminal.toObject().value("letters").toString().isEmpty()))
                    return id + ": cuts.beforeFinal needs a terminal letter selector, or false";
                for (const auto& settings : {declaredCuts, terminal.toObject()}) {
                    if (settings.isEmpty()) continue;
                    const auto boundaries = settings.value("boundaries").toObject();
                    if (!QStringList{"free", "preserve", "fork-normal"}.contains(boundaries.value("right").toString()) ||
                        !QStringList{"free", "preserve", "fork-normal", "shared-return"}.contains(boundaries.value("left").toString()))
                        return id + ": cuts.boundaries must declare the right and left cut policies";
                    if (boundaries.value("left") == "fork-normal" && boundaries.value("right") != "fork-normal")
                        return id + ": an outgoing fork-normal policy currently needs the incoming fork-normal policy";
                    if (boundaries.value("left") == "shared-return" && !settings.contains("letters"))
                        return id + ": a shared-return policy belongs in beforeFinal with its letter selector";
                    if (settings.contains("preserveEntryAfter") && !settings.value("preserveEntryAfter").isString())
                        return id + ": cuts.preserveEntryAfter must be a letter selector";
                    if (settings.contains("junction") && (settings.value("junction") != "inward-corners" ||
                        arm.isUndefined() || boundaries.value("left") != "shared-return"))
                        return id + ": inward-corners needs a shared-return boundary and an alefArm body";
                    for (auto key = settings.begin(); key != settings.end(); ++key)
                        if (!QStringList{"boundaries", "forkOffset", "tightJoin", "counterJoin", "beforeFinal",
                                         "letters", "junction", "preserveEntryAfter", "route", "section", "exitRoutes",
                                         "entryRoute", "placement", "entryTurnAngle", "shortExit"}.contains(key.key()))
                            return id + ": unknown cuts setting " + key.key();
                }
            }
            if (rule.contains("foldedArmRecovery") && !lamDescription(declaredBody))
                return id + ": foldedArmRecovery requires a medial lām body";
            if (rule.contains("shaftTurn") && (method != "alef-stem" || !rule.value("shaftTurn").isBool()))
                return id + ": shaftTurn requires an alef stem and must be true or false";
            if (rule.contains("tailEntry")) {
                const auto tail = rule.value("tailEntry").toObject();
                if (QString(phase) != "final" || (method != "walk" && method != "tail-entry") ||
                    (tail.contains("minRun") && !positive(tail.value("minRun"))) ||
                    (tail.contains("maxRun") &&
                     (!positive(tail.value("maxRun")) ||
                      tail.value("maxRun").toDouble() < tail.value("minRun").toDouble(2))) ||
                    (tail.contains("turnAngle") &&
                     (!positive(tail.value("turnAngle")) || tail.value("turnAngle").toDouble() >= 90)) ||
                    !positive(tail.value("walkWidths")) || !positive(tail.value("minRise")))
                    return id + ": tailEntry needs positive walkWidths and minRise and ordered run limits";
            }
            const auto bodyCounters = rule.value("bodyCounters");
            if (!bodyCounters.isUndefined() && (!bodyCounters.isDouble() ||
                !std::isfinite(bodyCounters.toDouble()) || bodyCounters.toDouble() < 0 ||
                bodyCounters.toDouble() != std::floor(bodyCounters.toDouble())))
                return id + ": bodyCounters must be a nonnegative integer";
            if (rule.contains("ligatureWith") && ((method != "counter-ligature" && headLip.isUndefined()) || QString(phase) != "initial" ||
                !rule.value("ligatureWith").isString() || rule.value("ligatureWith").toString().isEmpty()))
                return id + ": ligatureWith requires an initial ligature form and its paired letters";
            const auto counterLigature = rule.value("counterLigature").toObject();
            if (rule.contains("counterLigature") &&
                (QString(phase) != "medial" || counterLigature.value("letters").toString().isEmpty() ||
                 !positive(counterLigature.value("forkRise")) ||
                 !positive(counterLigature.value("minAscenderRise")) ||
                 !positive(counterLigature.value("maxBridgeRatio"))))
                return id + ": a medial counter ligature needs paired letters, fork rise, ascender rise and bridge "
                            "ratio";
            const auto maxBend = rule.value("maxBridgeBend").isUndefined() ? counterLigature.value("maxBridgeBend")
                                                                           : rule.value("maxBridgeBend");
            if (!maxBend.isUndefined() && (!positive(maxBend) || maxBend.toDouble() > .25))
                return id + ": an ink bridge bend must be within a quarter of its length";
            if (rule.contains("forkRise") && (method != "counter-ligature" || !positive(rule.value("forkRise"))))
                return id + ": forkRise needs a positive counter-height ratio";
            for (const auto* name : {"mouth", "blade", "shaft", "enclosedHead"})
                if (rule.contains(name)) {
                    const auto feature = rule.value(name).toObject();
                    if (QString(phase) != "medial" || feature.isEmpty())
                        return id + ": expected a medial shape description";
                    for (auto it = feature.begin(); it != feature.end(); ++it) {
                        const bool flag = it.key() == "projectExit" ||
                            (QLatin1String(name) == "mouth" && it.key() == "separateSharedEntry");
                        if (flag ? !it.value().isBool() : !positive(it.value()))
                            return id + ": expected a boolean shape flag or a positive measurement";
                    }
                    if (QLatin1String(name) == "mouth" && feature.value("separateSharedEntry").toBool() &&
                        (!positive(feature.value("maxThroat")) || !positive(feature.value("exitMargin"))))
                        return id + ": a separate mouth entry needs positive throat and exit dimensions";
                }
            if (rule.contains("terminalArm") &&
                (QString(phase) != "medial" || rule.value("terminalArm").toString().isEmpty() ||
                 !positive(rule.value("entryTurnAngle")) || rule.value("entryTurnAngle").toDouble() >= 90))
                return id +
                       ": a returning stem needs a final-arm template and an entry turn between 0 and 90 degrees";
            if (rule.contains("overhangingEntry") &&
                (QString(phase) != "medial" ||
                 !positive(rule.value("overhangingEntry").toObject().value("maxGap")) ||
                 !positive(rule.value("overhangingEntry").toObject().value("maxEntryToHead"))))
                return id + ": the detached entry needs a gap limit and entry-to-head width ratio";
            if (method == "folded-arm" && (!positive(rule.value("minRise")) || !positive(rule.value("maxNeck"))))
                return id + ": a folded arm needs a positive rise and neck width";
            if (method == "bowl-fork" && (!positive(rule.value("minRise")) || !positive(rule.value("walkWidths"))))
                return id + ": a bowl fork needs a positive rise and contour distance";
            if (method == "detached-arm" && (!positive(rule.value("detachedArm").toObject().value("minRise")) ||
                                             !positive(rule.value("detachedArm").toObject().value("maxTipGap"))))
                return id + ": a detached arm needs a positive rise and maximum tip gap";
            if (rule.contains("detachedExit") &&
                (QString(phase) != "medial" || rule.value("detachedExit").toString().isEmpty() ||
                 rule.value("prefixTail").toObject().value("method") != "tail-entry"))
                return id + ": expected a returning stem and detached exit letters";
            if (method == "tail-entry" && !rule.contains("tailEntry"))
                return id + ": tail-entry needs its tail measurements";
            if (method == "counter-ligature") {
                if (!rule.contains("ligatureWith") ||
                    (!rule.contains("forkRise") && rule.value("outer").toObject().value("top").toArray().isEmpty()))
                    return id + ": counter-ligature needs paired letters and a fork or an outer contour walk";
                problem = invalidWalkSettings(rule.value("outer").toObject(), id + ".outer");
                if (!problem.isEmpty()) return problem;
                for (const auto* key : {"maxBridgeRatio", "minAscenderRise"})
                    if (!rule.value(key).isDouble() || !std::isfinite(rule.value(key).toDouble()) ||
                        rule.value(key).toDouble() <= 0) return id + ": " + key + " must be a positive counter-height ratio";
                if (rule.contains("lowerBridge") && !QStringList{"right-tip", "bottom-extrema", "stem-side"}.contains(
                                                        rule.value("lowerBridge").toString()))
                    return id + ": lowerBridge must be right-tip, bottom-extrema or stem-side";
                if (rule.value("lowerBridge") == "stem-side" && !rule.contains("forkRise"))
                    return id + ": a stem-side bridge requires a fork rise";
            }
            if (rule.contains("lamPosition") && (method != "lam-alef" ||
                !QStringList{"initial", "medial"}.contains(rule.value("lamPosition").toString())))
                return id + ": lamPosition requires lam-alef and must be initial or medial";
            if (rule.contains("sameDirectionCheck") &&
                (method != "lam-alef" || !rule.value("sameDirectionCheck").isBool()))
                return id + ": sameDirectionCheck requires lam-alef and must be true or false";
            const auto teeth = method == "seen-teeth" ? rule : rule.value("seenTeeth").toObject();
            if (rule.contains("sharedFoldWithFinal")) {
                const bool initialBody = QString(phase) == "initial" && method == "seen-teeth";
                const bool medialBody = QString(phase) == "medial" &&
                    (!teeth.isEmpty() || !rule.value("contourBody").toObject().isEmpty());
                if ((!initialBody && !medialBody) || !rule.value("sharedFoldWithFinal").isString() ||
                    rule.value("sharedFoldWithFinal").toString().isEmpty())
                    return id + ": sharedFoldWithFinal needs a supported initial or medial body and final letters";
            }
            if (rule.contains("sharedFoldForm")) {
                const auto name = rule.value("sharedFoldForm").toString();
                const auto letters = rule.value("sharedFoldWithFinal").toString();
                const auto finals = config.value("final").toArray();
                const auto partner = std::find_if(finals.begin(), finals.end(), [&](const auto& value) {
                    return value.toObject().value("id") == name;
                });
                if (QString(phase) != "initial" || name.isEmpty() || letters.isEmpty() || partner == finals.end())
                    return id + ": sharedFoldForm needs a shared fold and a declared final template";
                const auto selected = partner->toObject().value("current").toString();
                if (!std::all_of(letters.begin(), letters.end(), [&](QChar ch) { return selected.contains(ch); }))
                    return id + ": sharedFoldForm must select the declared final letters";
            }
            const auto lastPitch = teeth.value("maxLastPitch");
            if (!lastPitch.isUndefined() && (!lastPitch.isDouble() || !std::isfinite(lastPitch.toDouble()) ||
                lastPitch.toDouble() < teeth.value("minPitch").toDouble(1)))
                return id + ": maxLastPitch must be at least minPitch";
            const auto minLastPitch = teeth.value("minLastPitch");
            if (!minLastPitch.isUndefined() && (!minLastPitch.isDouble() || !std::isfinite(minLastPitch.toDouble()) ||
                minLastPitch.toDouble() < teeth.value("minPitch").toDouble(1) ||
                minLastPitch.toDouble() > lastPitch.toDouble(teeth.value("maxPitch").toDouble(5))))
                return id + ": minLastPitch must be between minPitch and maxLastPitch";
            const auto widerPitch = teeth.value("retryMaxLastPitch");
            if (!widerPitch.isUndefined() && (!widerPitch.isDouble() || !std::isfinite(widerPitch.toDouble()) ||
                widerPitch.toDouble() <= lastPitch.toDouble(teeth.value("maxPitch").toDouble(5))))
                return id + ": retryMaxLastPitch must exceed the normal last pitch limit";
            const auto tipSpread = teeth.value("maxTipHeightSpreadRatio");
            if (!tipSpread.isUndefined() && (!tipSpread.isDouble() ||
                !std::isfinite(tipSpread.toDouble()) || tipSpread.toDouble() <= 0))
                return id + ": maxTipHeightSpreadRatio must be a positive ratio of the mean tooth pitch";
            const auto entryOffset = teeth.value("rightOffset");
            if (!entryOffset.isUndefined() && (!entryOffset.isDouble() ||
                !std::isfinite(entryOffset.toDouble()) || entryOffset.toDouble() < 0))
                return id + ": rightOffset must be a nonnegative distance in local stroke widths";
            const auto exitDistance = teeth.value("maxExitBendDistance");
            if (!exitDistance.isUndefined() && (!exitDistance.isDouble() ||
                !std::isfinite(exitDistance.toDouble()) || exitDistance.toDouble() <= 0))
                return id + ": maxExitBendDistance must be a positive ratio of the mean tooth pitch";
            if (teeth.contains("exitBendFirst") && !teeth.value("exitBendFirst").isBool())
                return id + ": exitBendFirst must be true or false";
            const auto retryBend = teeth.value("retryBendOffset");
            if (!retryBend.isUndefined() && (QString(phase) == "final" || !retryBend.isDouble() ||
                !std::isfinite(retryBend.toDouble()) || retryBend.toDouble() < 0))
                return id + ": retryBendOffset needs an outgoing edge and a nonnegative stroke-width offset";
            if (teeth.contains("entryCornerOptional") &&
                (!teeth.value("entryCornerOptional").isBool() || !teeth.value("rightOffset").isDouble()))
                return id + ": entryCornerOptional needs a boolean and rightOffset";
            if (teeth.contains("convexTips") && !teeth.value("convexTips").isBool())
                return id + ": convexTips must be true or false";
            const auto exitOffset = teeth.value("exitOffset");
            if (!exitOffset.isUndefined() && (!exitOffset.isDouble() || !std::isfinite(exitOffset.toDouble()) || exitOffset.toDouble() < 0))
                return id + ": exitOffset must be nonnegative in local stroke widths";
            const auto head = rule.value("headEdges").toObject();
            if (head.contains("maxExitDistance"))
                return id + ": headEdges uses its own counter and edges; remove maxExitDistance";
            const auto neckDistance = head.value("maxNeckDistance");
            if (!neckDistance.isUndefined() && (!neckDistance.isDouble() ||
                !std::isfinite(neckDistance.toDouble()) || neckDistance.toDouble() <= 0))
                return id + ": maxNeckDistance must be a positive distance in counter heights";
            if (QString(phase) == "medial") {
                if (rule.contains("penLift")) {
                    const auto lift = rule.value("penLift").toObject();
                    if (!QStringList{"entry", "exit"}.contains(lift.value("side").toString()) ||
                        lift.value("forms").toArray().isEmpty() || !rule.value("onUnresolved").toBool())
                        return id + ": a pen lift needs an exposed side and its allowed forms";
                    continue;
                }
                if (rule.contains("assembly")) {
                    const auto assembly = rule.value("assembly").toObject();
                    const auto heads = assembly.value("bodyForms").toArray();
                    const auto entry = assembly.value("entry").toObject();
                    const auto source = entry.value("source").toString();
                    if (assembly.size() != 3 || assembly.value("operation") != "exposed-head" || heads.isEmpty() ||
                        !rule.value("onUnresolved").toBool() || rule.contains("method") || rule.contains("body") ||
                        !QStringList{"matched-boundary", "form"}.contains(source) ||
                        (source == "matched-boundary" && entry.size() != 1) ||
                        (source == "form" && (entry.size() != 2 || entry.value("form").toString().isEmpty())))
                        return id + ": exposed-head assembly needs body variants and a shared entry policy";
                    for (const auto &value : heads) {
                        const auto child = namedLetterForm(config, value.toString());
                        if (!value.isString() || child.isEmpty() || !child.contains("body"))
                            return id + ": assembly bodyForms must name declared body templates";
                    }
                    if (source == "form" && namedLetterForm(config, entry.value("form").toString()).isEmpty())
                        return id + ": unknown entry form";
                    continue;
                }
                const bool feature =
                    rule.contains("terminalArm") || rule.contains("overhangingEntry") ||
                    rule.contains("parallelReturn") || rule.contains("enclosedHead") || rule.contains("shaft") ||
                    rule.contains("detachedExit") || rule.contains("counterLigature") || rule.contains("mouth") ||
                    rule.contains("blade") || !loopSettings(rule).isEmpty() ||
                    !rule.value("contourBody").toObject().isEmpty() ||
                    !rule.value("pairedCounterCorners").toObject().isEmpty() ||
                    !rule.value("openHead").toObject().isEmpty() || !rule.value("seenTeeth").toObject().isEmpty() ||
                    !declaredBody.isEmpty() || !rule.value("headEdges").toObject().isEmpty() ||
                    (!rule.value("right").toObject().value("walk").toArray().isEmpty() &&
                     !rule.value("left").toObject().value("walk").toArray().isEmpty());
                if (!feature || rule.contains("method")) return id + ": a medial form must define both body boundaries";
            } else {
                static const QStringList initialMethods{"enclosed-head","walk","descending-stem", "initial-counter", "initial-loop-head", "initial-loop-body", "heh-initial-counters", "seen-teeth", "kaf", "kaf-zigzag", "hah", "counter-ligature"};
                static const QStringList finalMethods{"rising-tail",  "folded-bowl", "folded-arm",    "bowl-fork",
                                                      "detached-arm", "tail-entry",  "walk",          "qaf",
                                                      "noon",         "alef-stem",   "stem-entry",    "lam-alef",
                                                      "waw",          "sad-tah",     "final-counter", "seen-teeth"};
                if (declaredBody.isEmpty() && !(QString(phase) == "initial" ? initialMethods : finalMethods).contains(method))
                    return id + ": an explicit body method for this position is required; generic join rules are unsupported";
            }
            if (rule.contains("bottomProbe") && method != "walk") return id + ": bottomProbe requires method walk";
            if (method == "rising-tail") {
                const auto spec = rule.value("tail").toObject();
                for (auto key : {"minRun", "minRise", "maxChord"})
                    if (!positive(spec.value(key))) return id + ": rising tail distances must be positive";
            }
            if (method == "walk") {
                problem = invalidWalkSettings(rule, id);
                if (!problem.isEmpty()) return problem;
            }
            const auto entryWalk = loopSettings(rule).value("entryWalk");
            if (!entryWalk.isUndefined()) {
                if (!entryWalk.isObject() || entryWalk.toObject().value("top").toArray().isEmpty())
                    return id + ": loopBoundaries.entryWalk needs a top walk";
                problem = invalidWalkSettings(entryWalk.toObject(), id + ".loopBoundaries.entryWalk");
                if (!problem.isEmpty()) return problem;
            }
            if (rule.contains("upperAttachment")) {
                const auto attachment = rule.value("upperAttachment").toObject();
                const auto right = rule.value("contourBody").toObject().value("right").toObject();
                if (QString(phase) != "medial" || !rule.value("sharedEntry").toBool() ||
                    right.value("bottomProbe") != "left" || right.value("anchorSide") != "bottom" ||
                    attachment.value("letters").toString().isEmpty())
                    return id + ": upperAttachment needs a shared leftward probe and upper letters";
                for (const auto key : {"headFlank", "risingFlank"}) {
                    const auto bounds = attachment.value(key).toObject();
                    if (bounds.isEmpty()) return id + ": upperAttachment needs both flank bounds";
                    for (auto it = bounds.begin(); it != bounds.end(); ++it) {
                        if (it.key() == "accumulate" && it.value().isBool()) continue;
                        if (!QStringList{"minX","maxX","minY","maxY"}.contains(it.key()) ||
                            !it.value().isDouble() || !std::isfinite(it.value().toDouble()))
                            return id + ": upperAttachment needs finite x/y bounds";
                    }
                    if (bounds.value("minX").toDouble(-INFINITY) > bounds.value("maxX").toDouble(INFINITY) ||
                        bounds.value("minY").toDouble(-INFINITY) > bounds.value("maxY").toDouble(INFINITY))
                        return id + ": upperAttachment flank minimum exceeds maximum";
                }
            }
            if (rule.contains("contourBody")) {
                const auto orient = rule.value("contourBody").toObject().value("orientToBody");
                if (!orient.isUndefined() && !orient.isBool()) return id + ": contourBody.orientToBody must be true or false";
                const auto convex = rule.value("contourBody").toObject().value("anchor").toObject().value("convex");
                if (!convex.isUndefined() && !convex.isBool()) return id + ": contour anchor convex must be true or false";
                for (const auto& flank : rule.value("contourBody").toObject().value("anchor").toObject().value("flanks").toArray()) {
                    const auto retained = flank.toObject().value("insideBody");
                    if (!retained.isUndefined() && !retained.isBool())
                        return id + ": contour anchor flank insideBody must be true or false";
                }
                for (const auto* side : {"right", "left"}) {
                    const auto route = rule.value("contourBody").toObject().value(side);
                    if (!route.isObject() || (route.toObject().value("top").toArray().isEmpty() &&
                        !route.toObject().value("topAtAnchor").toBool()))
                        return id + ": contourBody." + side + " needs a top walk or topAtAnchor";
                    problem = invalidWalkSettings(route.toObject(), id + ".contourBody." + side);
                    if (!problem.isEmpty()) return problem;
                }
                const auto retain = rule.value("contourBody").toObject().value("retain");
                if (!retain.isUndefined() && !retain.isArray())
                    return id + ": contourBody.retain must list intrinsic feature descriptions";
                for (const auto& value : retain.toArray()) {
                    if (!value.isObject() || value.toObject().isEmpty())
                        return id + ": a retained feature must have a description";
                    const auto feature = value.toObject();
                    for (const auto* flag : {"sharp", "xExtremum", "yExtremum"})
                        if (feature.contains(flag) && !feature.value(flag).isBool())
                            return id + ": retained landmark flags must be booleans";
                    for (const auto* field : {"offset", "flanks"}) {
                        if (!feature.value(field).isObject()) return id + ": retained landmarks need offset and flank bounds";
                        const auto bounds = feature.value(field).toObject();
                        for (auto item = bounds.begin(); item != bounds.end(); ++item)
                            if (!QStringList{"minX", "maxX", "minY", "maxY"}.contains(item.key()) ||
                                !item.value().isDouble() || !std::isfinite(item.value().toDouble()))
                                return id + ": retained landmark bounds must be finite x/y distances";
                        if (bounds.value("minX").toDouble(-INFINITY) > bounds.value("maxX").toDouble(INFINITY) ||
                            bounds.value("minY").toDouble(-INFINITY) > bounds.value("maxY").toDouble(INFINITY))
                            return id + ": retained landmark minimum exceeds maximum";
                    }
                }
            }
            const auto counterPair = loopSettings(rule).value("counterPair");
            const auto loopHeight = loopSettings(rule).value("maxBodyHeight");
            if (!loopHeight.isUndefined() && (!loopHeight.isDouble() || !std::isfinite(loopHeight.toDouble()) ||
                loopHeight.toDouble() <= 0)) return id + ": loopBoundaries.maxBodyHeight must be positive, in counter heights";
            if (!counterPair.isUndefined() && !QStringList{"horizontal", "vertical"}.contains(counterPair.toString())) {
                return id + ": loopBoundaries.counterPair must be horizontal or vertical";
            }
            problem = invalidKashidaSettings(rule.value("kashida"), effective, id + ".kashida");
            if (!problem.isEmpty()) return problem;
        }
    }
    const auto ligatures = config.value("ligatures");
    if (!ligatures.isUndefined() && !ligatures.isArray()) return "ligatures must be an array";
    for (const auto& value : ligatures.toArray()) {
        const auto rule = value.toObject();
        const auto id = rule.value("id").toString();
        if (id.isEmpty() || ids.contains(id)) return "Each ligature needs a unique nonempty id";
        ids.insert(id);
        const auto letters = rule.value("letters").toArray();
        if (letters.size() != 2 || letters.at(0).toString().isEmpty() || letters.at(1).toString().isEmpty())
            return id + ": letters must select both bases in the ligature";
        for (const auto& [position, index] : {std::pair{"initial", 0}, std::pair{"final", 1}}) {
            const auto selection = rule.value(position).toObject();
            const auto name = selection.value("template").toString();
            const auto selected = letters.at(index).toString();
            const auto forms = config.value(position).toArray();
            const auto valid = std::any_of(forms.begin(), forms.end(), [&](const auto& item) {
                const auto form = item.toObject();
                const auto current = form.value("current").toString();
                return form.value("id") == name && form.contains("body") && form.contains("cuts") &&
                    std::all_of(selected.begin(), selected.end(), [&](QChar ch) { return current.contains(ch); });
            });
            if (!valid) return id + ": " + position + " must name a body/cuts template for the selected letters";
            for (auto setting = selection.begin(); setting != selection.end(); ++setting)
                if (setting.key() != "template" &&
                    ((setting.key() != "body" && setting.key() != "cuts") || !setting.value().isObject()))
                    return id + ": template overrides must be body or cuts objects";
            const auto body = selection.value("body").toObject();
            const auto cuts = selection.value("cuts").toObject();
            if (body.contains("shape") || body.contains("inherit") || cuts.contains("inherit"))
                return id + ": a ligature override cannot change the template's shape or parent";
            const auto form = ligatureForm(config, selection);
            problem = validateSharedShape(form, position);
            if (!problem.isEmpty()) return id + ": " + problem;
        }
    }
    config.insert("kashidaDefaults", effective);
    return {};
}

namespace MedinaCutter {

static QJsonObject& activeLetterForms() {
    static auto config = [] {
        const auto bundled = readFormConfig(":/templates/letter-forms.json");
        const auto overridePath = qEnvironmentVariable("MEDINA_LETTER_FORMS");
        if (!overridePath.isEmpty()) {
            auto custom = readFormConfig(overridePath);
            const auto problem = PageAnalysisResult::prepareLetterForms(custom);
            if (problem.isEmpty()) return custom;
            qWarning().noquote() << "Invalid letter form configuration" << overridePath
                                 << "-" << problem << "- using bundled forms";
        }
        return bundled;
    }();
    return config;
}

} // namespace MedinaCutter

const QJsonObject& PageAnalysisResult::letterForms() {
    return activeLetterForms();
}

void PageAnalysisResult::activateLetterForms(const QJsonObject& config) {
    activeLetterForms() = config;
}
