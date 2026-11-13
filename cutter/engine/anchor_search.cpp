#include "../detail.h"

namespace MedinaCutter {
namespace {

using Subword = WordResultInfo::SubWordInfo;
using BodySequence = std::span<const AnchoredBody* const>;

QLineF sectionLine(const Stretching& section) {
    return {section.topBaseLine.point, section.bottomBaseLine.point};
}

bool sameSection(const Stretching& a, const Stretching& b) {
    return a.topBaseLine.time == b.topBaseLine.time && a.bottomBaseLine.time == b.bottomBaseLine.time &&
           sectionLine(a) == sectionLine(b);
}

bool orderedSections(const Stretching& right, const Stretching& left, Orientation orientation) {
    const auto receiving = getJoinTime(left, orientation);
    return cutPrecedes(right, left, orientation) &&
           receiving.isStrictRight(right.topBaseLine.time) && receiving.isStrictRight(right.bottomBaseLine.time) &&
           sectionLine(right).intersects(sectionLine(left), nullptr) != QLineF::BoundedIntersection;
}

bool fitsBody(const AnchoredBody& body, const SearchContext& source, size_t slot, size_t count) {
    if (body.entry.has_value() != (slot > 0) || body.exit.has_value() != (slot + 1 < count)) return false;
    const auto validSection = [&](const Stretching& section) {
        const auto& top = section.topBaseLine;
        const auto& bottom = section.bottomBaseLine;
        return section.counterEntry.pathIndex <= 0 && top.pathIndex == 0 && bottom.pathIndex == 0 &&
            top.time != bottom.time && sectionLine(section).length() > 1e-6 &&
            interiorChord(source.transformedPath, top, bottom, section.topEndpointTolerance);
    };
    if ((body.entry && !validSection(*body.entry)) || (body.exit && !validSection(*body.exit)) ||
        (body.entry && body.exit && !orderedSections(*body.entry, *body.exit, source.orientation))) return false;
    const auto entry = body.entry ? std::optional{getJoinTime(*body.entry, source.orientation)} : std::nullopt;
    const auto exit = body.exit ? std::optional{getJoinTime(*body.exit, source.orientation)} : std::nullopt;
    return std::all_of(body.landmarks.begin(), body.landmarks.end(), [&](const auto& point) {
        return point.pathIndex == 0 && (!entry || entry->isStrictLeft(point.time)) &&
               (!exit || exit->isStrictRight(point.time));
    });
}

std::optional<Subword> assemble(const Word& word, const Subword& sub, const Apath& shape,
        const SearchContext& source, const QString& letters, BodySequence bodies) {
    Subword trial{};
    trial.text = sub.text;
    trial.paths = sub.paths;
    trial.secondaryBodyPath = sub.secondaryBodyPath;
    trial.bodyCounter = sub.bodyCounter;
    trial.extrema = source.subword.extrema;
    trial.basePathOrientation = source.orientation;
    trial.kashidaPaths.resize(bodies.size() - 1);
    trial.kashidaCuts.resize(bodies.size() - 1);
    std::vector<Stretching> sections;
    sections.reserve(2 * (bodies.size() - 1));

    for (size_t slot = 1; slot < bodies.size(); ++slot) {
        const auto& exit = *bodies[slot - 1]->exit;
        const auto& entry = *bodies[slot]->entry;
        const bool shared = sameSection(exit, entry);
        sections.push_back(exit);
        if (!shared) {
            sections.push_back(entry);
            trial.kashidaCuts[slot - 1] = {sectionLine(exit), sectionLine(entry)};
        }
        auto join = exit;
        join.topRight = exit.topBaseLine;
        join.bottomRight = exit.bottomBaseLine;
        join.topLeft = entry.topBaseLine;
        join.bottomLeft = entry.bottomBaseLine;
        join.length = 0;
        join.readingOrder = int(slot - 1);
        join.fixedRightBoundary = true;
        join.fixedLeftBoundary = !shared;
        join.kashidaOverrides.insert("enabled", !shared);
        trial.joins.insert(join);
    }

    // Partition only after choosing a compatible sequence. Each section
    // comes directly from the chosen body, including the two ends of a connector.
    auto regions = cutRegions(shape.path, sub.text, sections, source.orientation, true);
    if (regions.size() != sections.size() + 1) return {};
    const auto inPage = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    size_t region = 0;
    QStringList bodyForms;
    for (size_t slot = 0; slot < bodies.size(); ++slot) {
        const auto& body = *bodies[slot];
        const auto mask = inPage(regions[region].path);
        if (mask.boundingRect().isEmpty() || std::any_of(body.samples.begin(), body.samples.end(),
                [&](const auto& point) { return !mask.contains(point); })) return {};
        trial.baseGlyphs.push_back(std::move(regions[region++]));
        bodyForms.append(body.formId);
        trial.letterDiagnostics.append(QJsonObject{{"letterIndex", int(slot + 1)}, {"character", letters.mid(int(slot), 1)},
            {"position", slot == 0 ? "initial" : slot + 1 == bodies.size() ? "final" : "medial"},
            {"status", "matched"}, {"template", body.formId}, {"candidates", QJsonArray{body.formId}}});

        if (slot + 1 == bodies.size() || sameSection(*body.exit, *bodies[slot + 1]->entry)) continue;
        const auto& [right, left] = trial.kashidaCuts[slot];
        auto& connector = regions[region++].path;
        if (connector.boundingRect().isEmpty() || toSubpaths(connector).size() != 1 ||
            !followsOneStroke(inPage(connector), right, left)) return {};
        trial.kashidaPaths[slot] = std::move(connector);
    }
    if (!coversOriginalInk(word, trial)) return {};
    trial.correct = true;
    trial.templateTrace.append("Fixed anchors · " + bodyForms.join(" → "));
    return trial;
}

std::optional<Subword> searchSequence(const Word& word, const Subword& sub, const Apath& shape,
        SearchContext& source, const QString& letters, const QJsonObject& config, const std::vector<QJsonObject>& forms) {
    std::vector<std::vector<AnchoredBody>> choices;
    choices.reserve(forms.size());
    for (size_t slot = 0; slot < forms.size(); ++slot) {
        auto candidates = anchoredBodies(source, config, forms[slot]);
        std::erase_if(candidates, [&](const auto& body) { return !fitsBody(body, source, slot, forms.size()); });
        if (candidates.empty()) return {};
        choices.push_back(std::move(candidates));
    }

    std::vector<const AnchoredBody*> selected;
    selected.reserve(choices.size());
    const auto search = [&](auto&& self, size_t slot) -> std::optional<Subword> {
        if (slot == choices.size()) return assemble(word, sub, shape, source, letters, selected);
        for (const auto& body : choices[slot]) {
            if (slot) {
                const auto& exit = *selected.back()->exit;
                if (!sameSection(exit, *body.entry) && !orderedSections(exit, *body.entry, source.orientation))
                    continue;
            }
            selected.push_back(&body);
            if (auto complete = self(self, slot + 1)) return complete;
            selected.pop_back();
        }
        return {};
    };
    return search(search, 0);
}

bool applies(const std::vector<QJsonObject>& forms, const QString& letters) {
    if (forms.size() != size_t(letters.size())) return false;
    for (size_t slot = 0; slot < forms.size(); ++slot) {
        const auto& form = forms[slot];
        if (!form.value("current").toString().contains(letters[int(slot)]) ||
            !form.value("kashida").toObject().isEmpty() || form.contains("sharedFoldWithFinal")) return false;
        const auto following = slot == 0 ? form.value("ligatureWith").toString() : medialFinalLetters(form);
        if (!following.isEmpty() && !hasFollowingLetter(letters, int(slot), following, slot > 0)) return false;
    }
    return true;
}

} // namespace

std::optional<WordResultInfo::SubWordInfo> segmentWithAnchors(const Word& word, const Subword& sub,
        const QJsonObject& config, const AnchorSequences& sequences) {
    if (sequences.empty() || sub.paths.empty() || sub.paths.front() < 0 ||
        sub.paths.front() >= int(word.paths.size()) || sub.secondaryBodyPath >= 0 ||
        sub.text.contains(QChar(0x0640))) return {};
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    const auto first = std::find_if(sequences.begin(), sequences.end(), [&](const auto& forms) {
        return applies(forms, letters);
    });
    if (first == sequences.end()) return {};
    const auto settings = config.value("kashidaDefaults").toObject();
    if (!settings.value("enabled").toBool(true) || settings.value("trackingEnabled").toBool() ||
        settings.value("minSpan").toDouble() > 0) return {};
    if (cutGeometryMemo && cutGeometryMemo->profiling) cutGeometryMemo->anchorSearch = "fallback";

    const auto shape = word.bodyShape(sub);
    auto ink = shape.transform.map(shape.path).translated(shape.pos);
    ink.setFillRule(Qt::WindingFill);
    Subword geometryOnly{};
    OutlineSearchCache cache;
    SearchContext source{.transformedPath = ink, .bbox = ink.boundingRect(),
                         .subword = geometryOnly, .outlineCache = &cache};
    if (!preparePassOutline(source, cache)) return {};
    for (auto sequence = first; sequence != sequences.end(); ++sequence) {
        if (sequence != first && !applies(*sequence, letters)) continue;
        if (auto complete = searchSequence(word, sub, shape, source, letters, config, *sequence)) {
            if (cutGeometryMemo && cutGeometryMemo->profiling) cutGeometryMemo->anchorSearch = "matched";
            return complete;
        }
    }
    return {};
}

bool sameAnchoredPartition(const Subword& a, const Subword& b) {
    const auto count = a.baseGlyphs.size();
    if (!a.correct || !b.correct || a.paths != b.paths || a.secondaryBodyPath != b.secondaryBodyPath ||
        count != b.baseGlyphs.size() || a.letterDiagnostics.size() != int(count) ||
        b.letterDiagnostics.size() != int(count) || a.kashidaPaths.size() != a.joins.size() || a.kashidaPaths != b.kashidaPaths ||
        a.kashidaCuts != b.kashidaCuts || a.joins.size() != b.joins.size()) return false;
    for (size_t slot = 0; slot < count; ++slot) {
        const auto& first = a.baseGlyphs[slot].path;
        const auto& second = b.baseGlyphs[slot].path;
        if (first != second || first.fillRule() != second.fillRule()) return false;
        const auto one = a.letterDiagnostics.at(int(slot)).toObject(), two = b.letterDiagnostics.at(int(slot)).toObject();
        if (one.value("template") != two.value("template") || one.value("variant") != two.value("variant")) return false;
    }
    auto second = b.joins.begin();
    size_t join = 0;
    for (const auto& first : a.joins) {
        if (a.kashidaPaths[join++].isEmpty() && first.cutSegments() != second->cutSegments()) return false;
        ++second;
    }
    return true;
}

} // namespace MedinaCutter
