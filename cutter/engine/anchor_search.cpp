#include "../detail.h"

namespace MedinaCutter {
namespace {

using Subword = WordResultInfo::SubWordInfo;
using BodySequence = std::span<const AnchoredBody* const>;
using FormChoices = std::vector<const std::vector<QJsonObject>*>;

size_t letterPosition(size_t slot, size_t count) {
    return slot == 0 ? 0 : slot + 1 == count ? 2 : 1;
}

QLineF sectionLine(const Stretching& section) {
    return {section.topBaseLine.point, section.bottomBaseLine.point};
}

bool sameSection(const Stretching& a, const Stretching& b) {
    return a.topBaseLine.time == b.topBaseLine.time && a.bottomBaseLine.time == b.bottomBaseLine.time &&
           sectionLine(a) == sectionLine(b);
}

bool orderedSections(const Stretching& right, const Stretching& left, Orientation orientation) {
    const auto receiving = getJoinTime(left, orientation);
    const bool atFork = sharesLowerForkTip(right, left);
    return cutPrecedes(right, left, orientation) &&
           receiving.isStrictRight(right.topBaseLine.time) &&
           (atFork || (receiving.isStrictRight(right.bottomBaseLine.time) &&
                       sectionLine(right).intersects(sectionLine(left), nullptr) != QLineF::BoundedIntersection));
}

bool sharesIncomingSection(const Stretching& sending, const AnchoredBody& receiving, Orientation orientation) {
    if (!receiving.sharesIncoming || !receiving.exit || cutPrecedes(sending, *receiving.entry, orientation) ||
        !orderedSections(sending, *receiving.exit, orientation)) return false;
    const auto side = getJoinTime(sending, orientation);
    return std::all_of(receiving.landmarks.begin(), receiving.landmarks.end(),
                       [&](const auto& landmark) { return side.isStrictLeft(landmark.time); });
}

bool sharesTightEntry(const AnchoredBody& sending, const AnchoredBody& receiving, double tolerance) {
    if (receiving.tightEntryAfter != sending.formId) return false;
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    // A triangular remnant at a stacked join has one nearly coincident
    // endpoint. The other end of the receiving seam stays on the loop rim.
    return std::min(QLineF(exit.topBaseLine.point, entry.topBaseLine.point).length(),
                    QLineF(exit.bottomBaseLine.point, entry.bottomBaseLine.point).length()) <= tolerance;
}

bool leavesSendingStroke(const SearchContext& source, const Stretching& exit, double penWidth) {
    const auto side = getJoinTime(exit, source.orientation);
    const int count = baseContourCurves(source.transformedPath).size();
    const auto chord = sectionLine(exit);
    const auto axis = (chord.p2() - chord.p1()) / chord.length();
    const auto flankDirection = [&](const PathExtrema& end, bool sending) {
        const bool forward = sending == (side.startAtLeft == (end.time == side.minTime));
        auto limit = end;
        limit.time += forward ? count : -count;
        PathExtrema sample{};
        QPointF tangent;
        if (!advanceOnContour(source.transformedPath, end, limit, forward, .25 * penWidth, sample, tangent))
            return QPointF{};
        const auto delta = sample.point - end.point;
        const double length = std::hypot(delta.x(), delta.y());
        return length > 0 ? delta / length : QPointF{};
    };
    const auto top = flankDirection(exit.topBaseLine, true);
    const auto bottom = flankDirection(exit.bottomBaseLine, true);
    constexpr double minFlankAlignment = .6;
    constexpr double minStrokeAlignment = .866;
    const double alongTop = QPointF::dotProduct(top, axis);
    const double alongBottom = QPointF::dotProduct(bottom, axis);
    const auto stroke = top + bottom;
    const bool alongBody = alongTop * alongBottom > 0 &&
        std::min(std::abs(alongTop), std::abs(alongBottom)) >= minFlankAlignment &&
        std::abs(alongTop + alongBottom) >= minStrokeAlignment * std::hypot(stroke.x(), stroke.y());
    if (!alongBody) return true;

    // The body flanks can run along the cap at a natural turn. Keep the
    // cut when the two flanks leave the cap together as a single stroke.
    constexpr double minOutgoingAlignment = .819152; // cos(35 degrees)
    return QPointF::dotProduct(flankDirection(exit.topBaseLine, false),
                               flankDirection(exit.bottomBaseLine, false)) >= minOutgoingAlignment;
}

bool retainsLandmarks(const AnchoredBody& body, const Stretching* entry, const Stretching* exit,
                      Orientation orientation) {
    const auto first = entry ? std::optional{getJoinTime(*entry, orientation)} : std::nullopt;
    const auto last = exit ? std::optional{getJoinTime(*exit, orientation)} : std::nullopt;
    return std::all_of(body.landmarks.begin(), body.landmarks.end(), [&](const auto& point) {
        return point.pathIndex == 0 && (!first || first->isStrictLeft(point.time)) &&
               (!last || last->isStrictRight(point.time));
    });
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
    return retainsLandmarks(body, body.entry ? &*body.entry : nullptr, body.exit ? &*body.exit : nullptr,
                            source.orientation);
}

std::optional<Subword> assemble(const Word& word, const Subword& sub, const Apath& shape,
        const SearchContext& source, const QString& letters, BodySequence bodies, bool sharesFinalFold,
        double tightTolerance, double penWidth) {
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
    std::vector<std::vector<QBezier>> outer;

    for (size_t slot = 1; slot < bodies.size(); ++slot) {
        const auto& sending = *bodies[slot - 1];
        const auto& receiving = *bodies[slot];
        auto exit = *sending.exit;
        const auto& entry = *receiving.entry;
        const bool sharesRim = receiving.sharesRimEntry &&
            (!orderedSections(exit, entry, source.orientation) ||
             QLineF(sectionLine(exit).center(), sectionLine(entry).center()).length() <= tightTolerance);
        if (sharesRim || (receiving.sharesJunctionEntry && !orderedSections(exit, entry, source.orientation)) ||
            (slot + 1 == bodies.size() &&
                         (sharesFinalFold || sharesTightEntry(sending, receiving, tightTolerance)))) {
            if (!retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &entry, source.orientation))
                return {};
            exit = entry;
        }
        const bool shared = sameSection(exit, entry) || sharesIncomingSection(exit, receiving, source.orientation);
        if (sending.footExitCandidate && (shared || !receiving.keepsEntryJunction)) return {};
        if (shared && ((receiving.keepsEntryJunction && !sameSection(exit, entry)) ||
                       (sending.keepsExitHollow && !sameSection(exit, *sending.exit)))) return {};
        if (!shared && (sending.keepsExitHollow || receiving.keepsEntryJunction) &&
            !leavesEntryClearance(exit, entry, source.orientation, tightTolerance)) return {};
        if (!shared && receiving.keepsEntryJunction && !leavesSendingStroke(source, exit, penWidth)) return {};
        if (!shared && bodies[slot]->retainsCounterRim) {
            // The saved rim geometry uses the sending cap's source curve
            // parameter. Rebind it without moving either endpoint.
            if (outer.empty()) outer.push_back(baseContourCurves(source.subPaths.front()));
            if (!sourceLandmark(exit.topBaseLine, outer) || !sourceLandmark(exit.bottomBaseLine, outer)) return {};
        }
        sections.push_back(exit);
        if (!shared) {
            sections.push_back(entry);
            trial.kashidaCuts[slot - 1] = {sectionLine(exit), sectionLine(entry)};
        }
        auto join = exit;
        join.topRight = exit.topBaseLine;
        join.bottomRight = exit.bottomBaseLine;
        join.topLeft = shared ? exit.topBaseLine : entry.topBaseLine;
        join.bottomLeft = shared ? exit.bottomBaseLine : entry.bottomBaseLine;
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
        if (!body.counters.isEmpty()) {
            const auto contours = toSubpaths(mask);
            if (std::any_of(body.counters.begin(), body.counters.end(), [&](size_t counter) {
                    return !contours.contains(source.subPaths[counter]);
                })) return {};
        }
        trial.baseGlyphs.push_back(std::move(regions[region++]));
        bodyForms.append(body.formId);
        trial.letterDiagnostics.append(QJsonObject{{"letterIndex", int(slot + 1)}, {"character", letters.mid(int(slot), 1)},
            {"position", slot == 0 ? "initial" : slot + 1 == bodies.size() ? "final" : "medial"},
            {"status", "matched"}, {"template", body.formId}, {"candidates", QJsonArray{body.formId}}});

        if (slot + 1 == bodies.size() || trial.kashidaCuts[slot].first.isNull()) continue;
        const auto& [right, left] = trial.kashidaCuts[slot];
        auto& connector = regions[region++].path;
        if (connector.boundingRect().isEmpty() || toSubpaths(connector).size() != 1 ||
            !followsOneStroke(inPage(connector), right, left, ConnectorEndpointMatch::NearestVertex)) return {};
        trial.kashidaPaths[slot] = std::move(connector);
    }
    if (!coversOriginalInk(word, trial)) return {};
    trial.correct = true;
    trial.templateTrace.append("Fixed anchors · " + bodyForms.join(" → "));
    return trial;
}

bool applies(const QJsonObject& form, const QString& letters, int slot) {
    if (!form.value("current").toString().contains(letters[slot]) ||
        !form.value("kashida").toObject().isEmpty() || form.contains("sharedFoldWithFinal")) return false;
    const auto maxLetters = form.value("fixedAnchorsMaxLetters");
    if (maxLetters.isDouble() && letters.size() > maxLetters.toInt()) return false;
    const auto following = slot == 0 ? form.value("ligatureWith").toString() : medialFinalLetters(form);
    return following.isEmpty() || hasFollowingLetter(letters, slot, following, slot > 0);
}

std::optional<Subword> searchSequence(const Word& word, const Subword& sub, const Apath& shape,
        SearchContext& source, const QString& letters, const FormChoices& forms, double penWidth,
        const QJsonObject& config, const AnchorPartitionCheck& accept) {
    std::map<QString, std::vector<AnchoredBody>> candidates;
    // Keep each candidate at its original contour sections across backtracking.
    const auto bodiesFor = [&](const QJsonObject& form, size_t slot) -> const std::vector<AnchoredBody>& {
        auto [saved, inserted] = candidates.try_emplace(form.value("id").toString());
        if (inserted) {
            saved->second = anchoredBodies(source, form, letterPosition(slot, forms.size()), penWidth);
            std::erase_if(saved->second, [&](const auto& body) { return !fitsBody(body, source, slot, forms.size()); });
        }
        return saved->second;
    };

    std::vector<const AnchoredBody*> selected;
    selected.reserve(forms.size());
    const auto connector = config.value("kashidaDefaults").toObject();
    const double tightTolerance = penWidth * connector.value("sharedCutTolerance").toDouble(.25);
    std::map<QString, std::optional<Stretching>> folds;
    const auto sharesFinalFold = [&]() {
        const auto& sending = *selected[selected.size() - 2];
        const auto& receiving = *selected.back();
        const auto& policy = sending.sharedFold;
        if (!policy.value("letters").toString().contains(letters.back())) return false;
        const auto descent = sectionLine(*receiving.entry).center() - sectionLine(*sending.exit).center();
        if (descent.y() < penWidth || std::abs(descent.x()) > policy.value("maxSlope").toDouble() * descent.y())
            return false;
        const auto id = policy.value("template").toString();
        auto [saved, inserted] = folds.try_emplace(id);
        if (inserted) {
            const auto receiver = namedLetterForm(config, id);
            source.res = {};
            source.nextIter = std::next(source.xSorted.begin());
            if (!receiver.isEmpty() && declaredSharedBoundary(source, resolvedBodyForm(config, receiver)))
                saved->second = source.res;
        }
        return saved->second && sameSection(*saved->second, *receiving.entry);
    };
    const auto search = [&](auto&& self, size_t slot) -> std::optional<Subword> {
        if (slot == forms.size()) {
            auto partition = assemble(word, sub, shape, source, letters, selected, sharesFinalFold(), tightTolerance,
                                      penWidth);
            if (partition && accept && !accept(*partition)) return {};
            return partition;
        }
        for (const auto& form : *forms[slot]) {
            if (!applies(form, letters, int(slot))) continue;
            const bool terminal = slot + 1 == forms.size();
            for (const auto& body : bodiesFor(form, slot)) {
                if (slot) {
                    const auto& sending = *selected.back();
                    const auto& exit = *sending.exit;
                    if (body.stemFootCandidate && !sending.sharesIncoming) continue;
                    if (!sameSection(exit, *body.entry) &&
                        !orderedSections(exit, *body.entry, source.orientation) &&
                        !sharesIncomingSection(exit, body, source.orientation) && !body.sharesRimEntry &&
                        !body.sharesJunctionEntry) {
                        // An overlapping fork can still fit the terminal
                        // bowl through the existing attachment policy.
                        if (sending.preservesTerminalBody && terminal &&
                            form.value("body").toObject().value("shape") == "stemmed-bowl")
                            return {};
                        continue;
                    }
                }
                selected.push_back(&body);
                if (auto complete = self(self, slot + 1)) return complete;
                selected.pop_back();
            }
        }
        return {};
    };
    return search(search, 0);
}

} // namespace

std::optional<WordResultInfo::SubWordInfo> segmentWithAnchors(const Word& word, const Subword& sub,
        const QJsonObject& config, const AnchorForms& forms, const AnchorPartitionCheck& accept) {
    if (forms.front().isEmpty() || forms.back().isEmpty() || sub.paths.empty() || sub.paths.front() < 0 ||
        sub.paths.front() >= int(word.paths.size()) || sub.secondaryBodyPath >= 0 ||
        sub.text.contains(QChar(0x0640))) return {};
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 2 || !forms.front().contains(letters.front()) || !forms.back().contains(letters.back())) return {};
    FormChoices choices;
    choices.reserve(letters.size());
    for (int slot = 0; slot < letters.size(); ++slot) {
        const auto& position = forms[letterPosition(slot, letters.size())];
        const auto found = position.constFind(letters[slot]);
        if (found == position.cend() ||
            std::none_of(found->begin(), found->end(), [&](const auto& form) { return applies(form, letters, slot); }))
            return {};
        choices.push_back(&found.value());
    }
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
    auto result = searchSequence(word, sub, shape, source, letters, choices, config.value("strokeWidth").toDouble(1),
                                 config, accept);
    if (result && cutGeometryMemo && cutGeometryMemo->profiling) cutGeometryMemo->anchorSearch = "matched";
    return result;
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
