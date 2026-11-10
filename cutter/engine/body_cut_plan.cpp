#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

InkOutline inkOutline(const SearchContext &context) {
    return {context.transformedPath, context.xSorted, context.ySorted, context.timeSorted,
            context.subPaths, context.bbox, context.orientation};
}

// Placement retries change joins and cursors, never the source landmarks.
// Cache the complete inventory, including the corner-preserving deduplication.
bool preparePassOutline(SearchContext& context, OutlineSearchCache& cache) {
    if (!cache.outline || cache.outline->ink != context.transformedPath) {
        auto prepared = std::make_shared<PreparedPassOutline>();
        prepared->ink = context.transformedPath;
        QVector<PathExtrema> raw;
        ShapeItem::getExtrema(prepared->ink, prepared->contours, raw);
        for (const auto& point : raw) {
            auto existing = std::find_if(prepared->extrema.begin(), prepared->extrema.end(), [&](const auto& other) {
                const double dx = std::abs(other.point.x() - point.point.x());
                const double dy = std::abs(other.point.y() - point.point.y());
                return other.pathIndex == point.pathIndex && dx < .01 && dy < .01 &&
                    (other.isSharp == point.isSharp || (dx < 1e-5 && dy < 1e-5));
            });
            if (existing == prepared->extrema.end()) prepared->extrema.append(point);
            else if (point.isSharp && !existing->isSharp) *existing = point;
        }
        for (const auto& point : prepared->extrema) if (point.pathIndex == 0) {
            prepared->time.insert(point);
            prepared->x.insert(point);
            prepared->y.insert(point);
        }
        if (prepared->time.size() < 3) return false;
        context.subPaths = prepared->contours;
        context.timeSorted = prepared->time;
        context.xSorted = prepared->x;
        context.ySorted = prepared->y;
        context.setOrientation();
        prepared->orientation = context.orientation;
        cache.outline = std::move(prepared);
    } else {
        context.subPaths = cache.outline->contours;
        context.timeSorted = cache.outline->time;
        context.xSorted = cache.outline->x;
        context.ySorted = cache.outline->y;
        context.orientation = cache.outline->orientation;
    }
    context.subword.extrema = cache.outline->extrema;
    return true;
}

// QJsonObject copies share their data until edited. Reuse the resolved
// declarations across a page; a template preview invalidates this bounded
// cache by value, without keeping any source geometry or segmentation result.
const BodyCutProgram& bodyCutProgram(const QJsonObject& config) {
    static thread_local QJsonObject compiledConfig;
    static thread_local BodyCutProgram program;
    if (compiledConfig == config) return program;
    program = {};
    for (const auto* phase : {"initial", "medial", "final"})
        for (const auto& value : config.value(phase).toArray()) {
            const auto form = value.toObject();
            if (!form.contains("body") || !form.contains("cuts")) continue;
            const auto current = form.value("current").toString();
            program.forms.insert(form.value("id").toString(), {current,
                formDescription(config, form, "body"), formDescription(config, form, "cuts")});
            for (QChar letter : current)
                if (!program.letters.contains(letter)) program.letters += letter;
        }
    compiledConfig = config;
    return program;
}

QByteArray bodyDescriptionKey(const QJsonObject& body) {
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

const LamBodyRecognition* recognizedLamBody(const SubwordBodyRecognition& recognition,
                                                   const QJsonObject& description) {
    const auto found = recognition.lamProfiles.constFind(bodyDescriptionKey(description));
    return found == recognition.lamProfiles.cend() ? nullptr : &found.value();
}

SubwordBodyRecognition recognizeSubwordBodies(const Word& word,
        const WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    SubwordBodyRecognition result;
    if (sub.paths.empty() || sub.paths.front() < 0 || sub.paths.front() >= int(word.paths.size()) ||
        sub.secondaryBodyPath >= int(word.paths.size())) return result;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    const auto& program = bodyCutProgram(config);
    if (std::none_of(letters.begin(), letters.end(), [&](QChar letter) { return program.letters.contains(letter); }))
        return result;
    QMap<QByteArray, QJsonObject> descriptions;
    for (const auto& form : program.forms) {
        // Tooth declarations have their own intrinsic recognizer. They must
        // not be interpreted as stems or trigger the lām fork search.
        if (!lamDescription(form.body))
            continue;
        if (std::none_of(letters.begin(), letters.end(), [&](QChar letter) { return form.current.contains(letter); }))
            continue;
        descriptions.insert(bodyDescriptionKey(form.body), form.body);
    }
    if (descriptions.isEmpty()) return result;
    const auto shape = word.bodyShape(sub);
    result.source = shape.transform.map(shape.path).translated(shape.pos);
    WordResultInfo::SubWordInfo geometryOnly{};
    SearchContext context{.transformedPath = result.source, .subword = geometryOnly};
    if (!readTerminalOutline(context)) return result;
    result.orientation = context.orientation;
    result.contours = context.subPaths;
    for (const auto& point : context.timeSorted) result.landmarks.append(point);
    QMap<std::pair<double, bool>, LamBodyEvidence> forkFeatures;
    QMap<QString, std::optional<std::pair<PathExtrema, PathExtrema>>> armFeatures;
    for (auto it = descriptions.begin(); it != descriptions.end(); ++it) {
        const auto& body = it.value();
        LamBodyRecognition features;
        const auto span = body.value("minFlankSpan").toDouble();
        const bool verifyShaft = body.value("shape") == "forked-stem" || body.value("shape") == "turning-shaft";
        const auto key = std::pair{span, verifyShaft};
        auto forks = forkFeatures.find(key);
        if (forks == forkFeatures.end()) forks = forkFeatures.insert(key, recognizeLamBodies(context, span, verifyShaft));
        features.lamBodies = forks.value();
        if (verifyShaft) {
            const auto curves = baseContourCurves(context.transformedPath);
            for (const auto& crest : features.lamBodies.crests)
                features.shaftForks.push_back(lamShaftForks(context, crest, features.lamBodies.forks, curves));
        }
        if (letters.size() >= 2 && body.value("alefLetters").toString().contains(letters.back())) {
            const auto armId = body.value("alefArm").toString();
            auto root = armFeatures.find(armId);
            if (root == armFeatures.end()) {
                const auto arm = namedLetterForm(config, armId);
                Stretching attachment{};
                bool corners = false;
                std::optional<std::pair<PathExtrema, PathExtrema>> seam;
                if (!arm.isEmpty() && declaredArmRoot(context, arm, attachment, &corners) && corners)
                    seam = std::pair{attachment.topBaseLine, attachment.bottomBaseLine};
                root = armFeatures.insert(armId, seam);
            }
            features.alefAttachment = root.value();
        }
        result.lamProfiles.insert(it.key(), std::move(features));
    }
    result.valid = true;
    return result;
}

SubwordCutPlan makeSubwordCutPlan(WordResultInfo::SubWordInfo proposals, const QJsonObject& config) {
    SubwordCutPlan plan{std::move(proposals), {}};
    const auto& sub = plan.partition;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (sub.letterDiagnostics.size() != letters.size()) return plan;
    const auto& program = bodyCutProgram(config);
    const std::vector<Stretching> joins(sub.joins.begin(), sub.joins.end());
    for (int slot = 0; slot < letters.size(); ++slot) {
        const auto id = sub.letterDiagnostics[slot].toObject().value("template").toString();
        const auto form = program.forms.constFind(id);
        if (form == program.forms.cend()) continue;
        const auto& body = form->body;
        const auto policy = bodyCutPolicy(form->cuts, letters, slot);
        const auto boundaries = policy.value("boundaries").toObject();
        const auto add = [&](BodyAttachmentPolicy attachment) {
            plan.bodies.push_back({slot, attachment, id, body, policy});
        };
        const bool incoming = slot > 0 && slot <= int(joins.size());
        const bool shared = boundaries.value("left") == "shared-return";
        if (shared) {
            const bool separate = sub.letterDiagnostics.last().toObject().value("boundaryKind") == "separate-contours";
            add(separate ? BodyAttachmentPolicy::SeparateAlefReturn : BodyAttachmentPolicy::SharedAlefReturn);
        }
        if (!incoming || boundaries.value("right") != "fork-normal") continue;
        if (boundaries.value("left") == "fork-normal" && slot < int(joins.size())) {
            add(joins[slot - 1].counterEntry.pathIndex > 0 ?
                BodyAttachmentPolicy::PreserveCounterEntry : BodyAttachmentPolicy::ForkFlanks);
        } else if ((shared || boundaries.value("left") == "free") &&
            !policy.value("preserveEntryAfter").toString().contains(letters[slot - 1]) &&
            joins[slot - 1].counterEntry.pathIndex <= 0) {
            add(BodyAttachmentPolicy::ForkEntry);
        }
    }
    return plan;
}

} // namespace MedinaCutter
