#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

LigatureMatch segmentDeclaredLigature(const Word& word, WordResultInfo::SubWordInfo& sub,
                                      const QJsonObject& config) {
    const auto rules = config.value("ligatures").toArray();
    if (rules.isEmpty() || sub.text.contains(QChar(0x0640))) return LigatureMatch::NotApplicable;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() != 2) return LigatureMatch::NotApplicable;
    const auto selected = std::find_if(rules.begin(), rules.end(), [&](const auto& value) {
        const auto selectors = value.toObject().value("letters").toArray();
        return selectors.size() == 2 && selectors.at(0).toString().contains(letters.front()) &&
               selectors.at(1).toString().contains(letters.back());
    });
    if (selected == rules.end()) return LigatureMatch::NotApplicable;

    const auto rule = selected->toObject();
    const auto id = rule.value("id").toString();
    const auto initial = bodyConstraintForm(config, ligatureForm(config, rule.value("initial").toObject()));
    const auto final = bodyConstraintForm(config, ligatureForm(config, rule.value("final").toObject()));
    const auto initialId = initial.value("id").toString();
    const auto finalId = final.value("id").toString();
    auto trial = sub;
    const auto clearCuts = [&] {
        trial.baseGlyphs.clear();
        trial.kashidaPaths.clear();
        trial.kashidaCuts.clear();
        trial.joins.clear();
    };
    clearCuts();
    trial.extrema.clear();
    trial.templateTrace.clear();
    const auto slot = [&](int index, const QString& form, bool matched) {
        return QJsonObject{{"letterIndex", index + 1}, {"character", letters.mid(index, 1)},
            {"position", index == 0 ? "initial" : "final"}, {"status", matched ? "matched" : "no-match"},
            {"template", matched ? form : QString{}}, {"candidates", QJsonArray{form}}};
    };
    trial.letterDiagnostics = {slot(0, initialId, false), slot(1, finalId, false)};
    trial.correct = false;
    const auto fail = [&](const QString& reason) {
        clearCuts();
        trial.templateTrace.append(id + " · " + reason);
        sub = std::move(trial);
        return LigatureMatch::Failed;
    };
    if (sub.paths.empty() || sub.paths.front() < 0 || sub.paths.front() >= int(word.paths.size()))
        return fail("no source outline");
    const auto shape = word.bodyShape(sub);
    OutlineSearchCache cache;
    SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                          .subword = trial, .baseText = letters, .outlineCache = &cache};
    context.transformedPath.setFillRule(Qt::WindingFill);
    context.bbox = context.transformedPath.boundingRect();
    if (!preparePassOutline(context, cache)) return fail("no contour landmarks");
    trial.basePathOrientation = context.orientation;
    context.nextIter = std::next(context.xSorted.begin());

    // The receiving body supplies the one seam. The initial form must retain
    // its defining landmarks at that seam without supplying a second cut.
    if (!proposeTemplateBoundary(context, config, final, true, false, nullptr))
        return fail(finalId + " · no shared boundary");
    if (!matchesBoundaryBody(context, final, context.res, false))
        return fail(finalId + " · shared cut loses body features");
    auto seam = context.res;
    const auto body = initial.value("body").toObject();
    if (body.value("shape") == "suspended-loop") {
        InspectFormScope inspection(initial, context.transformedPath);
        const auto evidence = recognizedSharedShape(context, body);
        inspectSharedShape(*evidence);
        const auto parts = cutRegions(context.transformedPath, {}, {seam}, context.orientation, true);
        if (!evidence->head || parts.size() != 2 ||
            !parts.front().path.boundingRect().contains(evidence->head->counter) ||
            parts.back().path.boundingRect().intersects(evidence->head->counter))
            return fail(initialId + " · shared cut loses the loop");
    } else {
        if (!proposeTemplateBoundary(context, config, initial, false, false, nullptr, &seam))
            return fail(initialId + " · body not recognized");
        if (cutPrecedes(seam, context.res, context.orientation))
            return fail(initialId + " · shared cut precedes the initial body's exit");
        if (rule.contains("minConnectorWidths")) {
            const auto& exit = context.res;
            if (!cutPrecedes(exit, seam, context.orientation) || !matchesBoundaryBody(context, initial, exit, true))
                return fail(initialId + " · initial exit does not fit before the shared cut");
            const auto midpointX = [](const Stretching& cut) {
                return (cut.topBaseLine.point.x() + cut.bottomBaseLine.point.x()) / 2;
            };
            const double minSpan = rule.value("minConnectorWidths").toDouble() * config.value("strokeWidth").toDouble(1);
            if (midpointX(exit) - midpointX(seam) >= minSpan)
                return LigatureMatch::SeparateConnector;
        }
    }
    if (!matchesBoundaryBody(context, initial, seam, true))
        return fail(initialId + " · shared cut loses body features");
    seam.topLeft = seam.topRight = seam.topBaseLine;
    seam.bottomLeft = seam.bottomRight = seam.bottomBaseLine;
    seam.fixedLeftBoundary = false;
    seam.length = 0;
    seam.kashidaOverrides.insert("enabled", false);
    trial.joins.insert(seam);
    if (!assembleBodyPartition(word, trial, config) || trial.baseGlyphs.size() != 2 ||
        std::any_of(trial.baseGlyphs.begin(), trial.baseGlyphs.end(), [](const auto& glyph) { return glyph.path.isEmpty(); }) ||
        (initial.contains("bodyCounters") &&
         int(toSubpaths(trial.baseGlyphs.front().path).size()) != initial.value("bodyCounters").toInt() + 1) ||
        (final.contains("bodyCounters") &&
         int(toSubpaths(trial.baseGlyphs.back().path).size()) != final.value("bodyCounters").toInt() + 1) ||
        !coversOriginalInk(word, trial))
        return fail("shared cut failed ink ownership");
    sub = std::move(trial);
    sub.letterDiagnostics = {slot(0, initialId, true), slot(1, finalId, true)};
    recordBoundaryTemplate(sub, 0, false, finalId);
    sub.templateTrace.append(id + " · " + initialId + " + " + finalId + " · shared cut");
    sub.correct = true;
    return LigatureMatch::SharedCut;
}

bool connectorContainsLeadingStroke(const QVector<PathExtrema>& extrema,
        const Stretching& right, const Stretching& left, Orientation orientation) {
    const PathExtrema* origin = nullptr;
    for (const auto& p : extrema) if (p.pathIndex == 0 &&
        (!origin || p.point.x() > origin->point.x())) origin = &p;
    return origin && getJoinTime(right, orientation).isStrictLeft(origin->time) &&
        getJoinTime(left, orientation).isStrictRight(origin->time);
}

// Partition a local proposal. Policy trials never mutate a published subword.
bool assembleBodyPartition(const Word& word, WordResultInfo::SubWordInfo& subword,
        const QJsonObject& config, ConnectorAssembly connectors) {
    if (connectors == ConnectorAssembly::ShareTightEntry) {
        const auto& diagnostics = subword.letterDiagnostics;
        if (subword.joins.empty() || !subword.joins.begin()->fixedLeftBoundary ||
            diagnostics.size() != subword.joins.size() + 1 ||
            std::any_of(diagnostics.cbegin(), diagnostics.cend(), [](const auto& slot) {
                return slot.toObject().value("status") != "matched";
            })) return false;
        const auto form = namedLetterForm(config, diagnostics.first().toObject().value("template").toString());
        if (!form.value("tightJoin").toBool() || !form.contains("ligatureWith")) return false;

        auto seam = *subword.joins.begin();
        auto entry = seam;
        entry.topBaseLine = seam.topLeft;
        entry.bottomBaseLine = seam.bottomLeft;
        if (!connectorContainsLeadingStroke(subword.extrema, seam, entry, subword.basePathOrientation))
            return false;

        seam.length = 0;
        seam.fixedLeftBoundary = false;
        seam.kashidaOverrides.insert("enabled", false);
        subword.joins.erase(subword.joins.begin());
        subword.joins.insert(seam);
    }
    const auto shape = word.bodyShape(subword);
    auto ink = shape.transform.map(shape.path);
    ink.translate(shape.pos);
    ink.setFillRule(Qt::WindingFill);

    std::vector<Stretching> cuts(subword.joins.begin(), subword.joins.end());
    subword.kashidaPaths.assign(cuts.size(), {});
    subword.kashidaCuts.assign(cuts.size(), {});
    if (cuts.empty()) {
        subword.baseGlyphs = {{subword.text, shape.path}};
        return true;
    }
    auto regions = cutRegions(shape.path, subword.text, cuts, subword.basePathOrientation);
    if (regions.size() != cuts.size() + 1) {
        subword.baseGlyphs = std::move(regions);
        return false;
    }

    // Replace a single cut with the two contour chords at the ends of its join.
    // Keep the single cut if either endpoint is ambiguous or the cuts conflict.
    const auto defaults = config.value("kashidaDefaults").toObject();
    std::vector<size_t> joinRegions;
    size_t cutIndex = 0;
    for (const auto& join : subword.joins) {
        const size_t candidate = cutIndex++;
        joinRegions.push_back(0);
        const auto settings = withOverrides(defaults, join.kashidaOverrides);
        if (!settings.value("enabled").toBool(true)) continue;

        const double neighborRatio = settings.value("neighborSearchRatio").toDouble();
        auto right = join;
        auto left = join;
        const double x = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).center().x();
        const auto cutX = [](const Stretching& cut) {
            return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).center().x();
        };
        const double minX = candidate + 1 < cuts.size() ? x + neighborRatio * (cutX(cuts[candidate + 1]) - x) : ink.boundingRect().left();
        const double maxX = candidate > 0 ? x + neighborRatio * (cutX(cuts[candidate - 1]) - x) : ink.boundingRect().right();
        if (join.fixedLeftBoundary) {
            left.topBaseLine = join.topLeft;
            left.bottomBaseLine = join.bottomLeft;
        } else if (!settings.value("trackingEnabled").toBool(false) ||
                   !findJoinEnds(ink, join, minX, maxX, settings, left, right)) {
            if (join.length > 0 &&
                interiorChord(ink, join.topRight, join.bottomRight) &&
                interiorChord(ink, join.topLeft, join.bottomLeft)) {
                if (!join.fixedRightBoundary) {
                    right.topBaseLine = join.topRight;
                    right.bottomBaseLine = join.bottomRight;
                }
                left.topBaseLine = join.topLeft;
                left.bottomBaseLine = join.bottomLeft;
            } else continue;
        }

        const double minSpan = settings.value("minSpan").toDouble();
        if (minSpan > 0 && cutX(right) - cutX(left) < minSpan) continue;

        // Attachment order follows the contour, not X on the page. Folded
        // bodies can have an incoming endpoint to the left of their exit.
        const auto before = [&](const Stretching& a, const Stretching& b) {
            return cutPrecedes(a, b, subword.basePathOrientation);
        };
        if (!before(right, left) ||
            (candidate > 0 && !before(cuts[candidate - 1], right)) ||
            (candidate + 1 < cuts.size() && !before(left, cuts[candidate + 1]))) continue;

        auto trial = cuts;
        trial[candidate] = right;
        trial.insert(trial.begin() + candidate + 1, left);
        const auto chord = [](const Stretching& cut) {
            return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
        };
        const auto crosses = [&](const Stretching& a, const Stretching& b) {
            return !sharesLowerForkTip(a, b) && !sharesLowerForkTip(b, a) &&
                chord(a).intersects(chord(b), nullptr) == QLineF::BoundedIntersection;
        };
        bool crossed = false;
        for (size_t other = 0; other < trial.size(); ++other) {
            if (other != candidate && crosses(right, trial[other]))
                crossed = true;
            if (other != candidate + 1 && crosses(left, trial[other]))
                crossed = true;
        }
        if (crossed) continue;
        auto pieces = cutRegions(shape.path, subword.text, trial, subword.basePathOrientation, true);
        if (pieces.size() != trial.size() + 1 ||
            std::any_of(pieces.begin(), pieces.end(), [](const GlyphInfo& piece) {
                return piece.path.isEmpty() || piece.path.boundingRect().isEmpty();
            })) continue;
        if (candidate == 0) {
            // The first connecting span cannot wrap around the leading stroke.
            // Check the actual proposed span, after shared ligature boundaries
            // have been reconciled, rather than rejecting tentative walks
            // that a later intrinsic ligature template legitimately replaces.
            if (connectorContainsLeadingStroke(subword.extrema, right, left, subword.basePathOrientation)) {
                subword.templateTrace.append("kashida · leading stroke before initial body rejected");
                subword.baseGlyphs = std::move(regions);
                return false;
            }
        }
        cuts = std::move(trial);
        regions = std::move(pieces);
        subword.kashidaCuts[joinRegions.size() - 1] = {
            QLineF(right.topBaseLine.point, right.bottomBaseLine.point),
            QLineF(left.topBaseLine.point, left.bottomBaseLine.point)
        };
        joinRegions.back() = candidate + 1;
        ++cutIndex;
    }

    for (size_t index = joinRegions.size(); index-- > 0;) {
        const auto region = joinRegions[index];
        if (!region) continue;
        subword.kashidaPaths[index] = std::move(regions[region].path);
        regions.erase(regions.begin() + region);
    }
    subword.baseGlyphs = std::move(regions);
    if (connectors == ConnectorAssembly::ShareTightEntry)
        subword.templateTrace.append("Initial ligature shares its seam");
    return true;
}

bool replaceLocalJoin(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config,
        const QPainterPath& ink, int slot, Stretching sending, const std::optional<Stretching>& receiving,
        const QString& trace, const std::function<bool(const WordResultInfo::SubWordInfo&)>& retainsBodies,
        const std::optional<Stretching>& previousJoin) {
    if (!interiorChord(ink, sending.topBaseLine, sending.bottomBaseLine) ||
        (receiving && !interiorChord(ink, receiving->topBaseLine, receiving->bottomBaseLine))) return false;
    auto trial = sub;
    sending.length = 0;
    sending.fixedRightBoundary = true;
    sending.fixedLeftBoundary = receiving.has_value();
    sending.topRight = sending.topBaseLine;
    sending.bottomRight = sending.bottomBaseLine;
    sending.topLeft = receiving ? receiving->topBaseLine : sending.topBaseLine;
    sending.bottomLeft = receiving ? receiving->bottomBaseLine : sending.bottomBaseLine;
    sending.kashidaOverrides.insert("enabled", receiving.has_value());
    trial.joins.clear();
    int i = 0;
    for (auto cut : sub.joins) {
        if (i == slot) cut = sending;
        else if (previousJoin && i == slot - 1) cut = *previousJoin;
        cut.readingOrder = i++;
        trial.joins.insert(cut);
    }
    if (!assembleBodyPartition(word, trial, config) || trial.baseGlyphs.size() != sub.baseGlyphs.size() ||
        trial.kashidaPaths.size() != sub.kashidaPaths.size() ||
        trial.kashidaPaths[slot].isEmpty() == receiving.has_value() || !retainsBodies(trial)) return false;
    for (size_t j = 0; j < sub.baseGlyphs.size(); ++j)
        if (j != size_t(slot) && j != size_t(slot + 1) && !(previousJoin && j + 1 == size_t(slot)) &&
            trial.baseGlyphs[j].path != sub.baseGlyphs[j].path) return false;
    for (size_t j = 0; j < sub.kashidaPaths.size(); ++j) {
        if (previousJoin && j + 1 == size_t(slot)) {
            if (trial.kashidaPaths[j].isEmpty() != sub.kashidaPaths[j].isEmpty()) return false;
            if (!trial.kashidaPaths[j].isEmpty() && (trial.kashidaCuts[j].first != sub.kashidaCuts[j].first ||
                trial.baseGlyphs[j].path != sub.baseGlyphs[j].path)) return false;
        } else if (j != size_t(slot) && trial.kashidaPaths[j] != sub.kashidaPaths[j]) return false;
    }
    if (!coversOriginalInk(word, trial)) return false;
    trial.templateTrace.append(trace);
    sub = std::move(trial);
    return true;
}

static bool matchAssembledBody(SearchContext& source, const Apath& shape, const QJsonObject& config,
        WordResultInfo::SubWordInfo& trial, int slot, const QString& exitTemplate = {}) {
    const auto diagnostic = trial.letterDiagnostics[slot].toObject();
    const auto letter = diagnostic.value("character").toString();
    if (letter.isEmpty()) return false;
    const auto cap = [&](size_t gap, bool receiving) {
        if (gap < trial.kashidaPaths.size() && !trial.kashidaPaths[gap].isEmpty())
            return receiving ? trial.kashidaCuts[gap].second : trial.kashidaCuts[gap].first;
        const auto segments = std::next(trial.joins.begin(), gap)->cutSegments();
        return segments.size() == 1 ? segments.front() : QLineF{};
    };
    const auto entry = slot > 0 ? cap(slot - 1, true) : QLineF{};
    const auto exit = size_t(slot) < trial.joins.size() ? cap(slot, false) : QLineF{};
    const auto mask = shape.transform.map(trial.baseGlyphs[slot].path).translated(shape.pos);
    for (const auto& value : config.value(diagnostic.value("position").toString()).toArray()) {
        const auto form = value.toObject();
        if (!hasCounterOutline(form) || !form.value("current").toString().contains(letter) ||
            (form.contains("requiresExitTemplate") && form.value("requiresExitTemplate").toString() != exitTemplate) ||
            !matchesCounterOutline(source, mask, form, entry, exit)) continue;
        const auto id = form.value("id").toString();
        recordAssembledForm(trial, slot, id);
        auto matched = trial.letterDiagnostics[slot].toObject();
        matched.insert("matchMethod", "counter-outline");
        trial.letterDiagnostics[slot] = matched;
        trial.templateTrace.append(id + " · counter and body attachments matched");
        return true;
    }
    return false;
}

static bool projectTerminalShoulder(SearchContext& context, const QJsonObject& form,
        const QJsonObject& policy, Stretching current, double bodyFloor, std::optional<double> sharedShift = {});

static bool assembleSharedTerminalEntry(SearchContext& source, const Apath& shape, const QJsonObject& config,
        const QJsonObject& receiver, const QString& letters, WordResultInfo::SubWordInfo& trial) {
    const int last = letters.size() - 1;
    const auto matched = [&](int slot) {
        return trial.letterDiagnostics[slot].toObject().value("status") == "matched";
    };
    for (int slot = 0; slot < last - 1; ++slot)
        if (!matched(slot)) return false;
    if (!declaredSharedBoundary(source, receiver)) return false;
    auto seam = source.res;
    const auto id = receiver.value("id").toString();

    // A declared counter rim can supply the preceding seam if its eye was
    // enclosed by the earlier body. Verify the proposed body on the source.
    std::optional<Stretching> rim;
    QString rimTemplate;
    if (last >= 2 && !matched(last - 1)) {
        const auto captured = toSubpaths(shape.transform.map(trial.baseGlyphs[last - 2].path).translated(shape.pos));
        for (const auto& value : config.value("medial").toArray()) {
            const auto form = value.toObject();
            if (!form.contains("rimOffset") || form.value("requiresExitTemplate").toString() != id ||
                !form.value("current").toString().contains(letters[last - 1])) continue;
            for (int i = 1; i < captured.size(); ++i) {
                const auto eye = captured[i].boundingRect();
                Stretching edge{};
                if (!medialChordAt(source, eye.right() + form.value("rimOffset").toDouble() * eye.height(),
                                   eye.center().y(), 1.3 * eye.height(), edge)) continue;
                orientSection(source, edge, seam.topBaseLine.time, true);
                const auto pieces = cutRegions(source.transformedPath, {}, {edge, seam}, source.orientation, true);
                if (pieces.size() != 3 || !matchesCounterOutline(source, pieces[1].path, form,
                        edge.cutSegments().front(), seam.cutSegments().front())) continue;
                if (rim) return false;
                rim = edge;
                rimTemplate = form.value("id").toString();
            }
        }
    }

    // Recut only the bodies affected by the new seams. Start at the receiving
    // cap of the preserved prefix so that its last connector stays intact.
    const int first = last - (rim ? 2 : 1);
    std::vector<Stretching> sections;
    if (first > 0) {
        const auto gap = size_t(first - 1);
        auto cap = *std::next(trial.joins.begin(), gap);
        if (gap < trial.kashidaPaths.size() && !trial.kashidaPaths[gap].isEmpty()) {
            const auto edge = trial.kashidaCuts[gap].second;
            cap = {};
            cap.topBaseLine.point = edge.p1();
            cap.bottomBaseLine.point = edge.p2();
            const std::vector<std::vector<QBezier>> outer{baseContourCurves(source.subPaths.front())};
            if (!sourceLandmark(cap.topBaseLine, outer) || !sourceLandmark(cap.bottomBaseLine, outer)) return false;
            orientSection(source, cap, seam.topBaseLine.time, true);
        }
        sections.push_back(cap);
    }
    if (rim) sections.push_back(*rim);
    std::optional<Stretching> exit;
    if (!rim && matched(last - 1) && size_t(last) <= trial.joins.size()) {
        const auto& sending = *std::next(trial.joins.begin(), last - 1);
        const QLineF from(sending.topBaseLine.point, sending.bottomBaseLine.point);
        const auto canKeepExit = [&](const Stretching& receiving) {
            const QLineF to(receiving.topBaseLine.point, receiving.bottomBaseLine.point);
            const auto minSpan = config.value("kashidaDefaults").toObject().value("minSpan").toDouble();
            if (!cutPrecedes(sending, receiving, source.orientation) ||
                from.center().x() - to.center().x() <= std::max(minSpan, .5 * std::min(from.length(), to.length())))
                return false;
            const auto pieces = cutRegions(source.transformedPath, {}, {sending, receiving}, source.orientation, true);
            return pieces.size() == 3 && toSubpaths(pieces[1].path).size() == 1 &&
                followsOneStroke(pieces[1].path, from, to);
        };
        if (sending.counterEntry.pathIndex <= 0 && sending.fixedRightBoundary) {
            if (canKeepExit(seam)) {
                exit = sending;
            } else if (const auto shoulder = formDescription(config, receiver, "cuts").value("shoulderEntry").toObject();
                       !shoulder.isEmpty()) {
                // A slanted terminal seam can put a slice of the bowl into
                // the connector. Test the declared lower shoulder before
                // merging the preceding body's exit with the terminal cut.
                const auto pieces = cutRegions(source.transformedPath, {}, {seam}, source.orientation, true);
                if (pieces.size() == 2 && projectTerminalShoulder(source, receiver, shoulder, seam,
                        pieces.back().path.boundingRect().bottom()) && canKeepExit(source.res) &&
                    matchesBoundaryBody(source, receiver, source.res, false)) {
                    exit = sending;
                    seam = source.res;
                }
            }
        }
    }
    if (exit) sections.push_back(*exit);
    sections.push_back(seam);
    const auto pieces = cutRegions(shape.path, {}, sections, source.orientation, true);
    if (pieces.size() != sections.size() + 1) return false;

    trial.baseGlyphs.resize(last + 1);
    const auto bodyBegin = pieces.size() - (last - first + 1) - (exit ? 1 : 0);
    for (int slot = first; slot < last; ++slot)
        trial.baseGlyphs[slot].path = pieces[bodyBegin + slot - first].path;
    trial.baseGlyphs.back().path = pieces.back().path;
    trial.baseGlyphs.back().text = letters.right(1);
    trial.kashidaPaths.resize(last);
    trial.kashidaCuts.resize(last);
    std::vector<Stretching> joins(trial.joins.begin(), std::next(trial.joins.begin(), first));
    if (exit) {
        auto connector = *exit;
        connector.topRight = connector.topBaseLine;
        connector.bottomRight = connector.bottomBaseLine;
        connector.topLeft = seam.topBaseLine;
        connector.bottomLeft = seam.bottomBaseLine;
        connector.fixedLeftBoundary = true;
        connector.length = 0;
        connector.kashidaOverrides.insert("enabled", true);
        joins.push_back(connector);
    } else {
        joins.insert(joins.end(), sections.end() - (last - first), sections.end());
    }
    trial.joins.clear();
    for (int gap = 0; gap < last; ++gap) {
        joins[gap].readingOrder = gap;
        trial.joins.insert(joins[gap]);
        if (gap >= first) {
            trial.kashidaPaths[gap] = {};
            trial.kashidaCuts[gap] = {};
        }
    }
    if (!matched(last - 1) && !matchAssembledBody(source, shape, config, trial, last - 1, id)) return false;
    if (rim) {
        recordBoundaryTemplate(trial, last - 2, false, rimTemplate);
        recordBoundaryTemplate(trial, last - 1, true, rimTemplate);
    }
    if (exit) {
        trial.kashidaPaths.back() = pieces[pieces.size() - 2].path;
        trial.kashidaCuts.back() = {
            QLineF(exit->topBaseLine.point, exit->bottomBaseLine.point),
            QLineF(seam.topBaseLine.point, seam.bottomBaseLine.point)};
    } else {
        recordBoundaryTemplate(trial, last - 1, false, id);
    }
    recordAssembledForm(trial, last, id);
    recordBoundaryTemplate(trial, last, true, id);
    trial.templateTrace.append(id + (exit ? " · separate connector at its declared cut" : " · shared entry at its declared cut"));
    return true;
}

bool completeBodyPartition(const Word& word, WordResultInfo::SubWordInfo& subword,
        const QJsonObject& config, const QJsonObject& checks) {
    const auto letters = PageAnalysisResult::bodyLetters(subword.text);
    const int count = letters.size();
    if (count < 2 || subword.paths.empty() || subword.letterDiagnostics.size() != count ||
        subword.baseGlyphs.size() != subword.joins.size() + 1 ||
        (subword.baseGlyphs.size() != size_t(count) && subword.baseGlyphs.size() != size_t(count - 1)) ||
        subword.kashidaPaths.size() > subword.kashidaCuts.size()) return false;

    QJsonObject receiver;
    for (const auto& value : config.value("final").toArray()) {
        const auto form = value.toObject();
        if (!form.value("sharedBoundary").toBool() || !form.value("current").toString().contains(letters.back()) ||
            (form.contains("sharedBoundaryWith") &&
             !form.value("sharedBoundaryWith").toString().contains(letters[count - 2]))) continue;
        if (formDescription(config, form, "cuts").value("shareEntryWhenUnresolved").toBool()) {
            receiver = resolvedBodyForm(config, form);
            break;
        }
    }
    const auto unresolved = [](const QJsonValue& diagnostic) {
        return diagnostic.toObject().value("status") != "matched";
    };
    const bool checkExisting = subword.baseGlyphs.size() == size_t(count) &&
        std::any_of(subword.letterDiagnostics.cbegin(), subword.letterDiagnostics.cend(), unresolved);
    if (!checkExisting && receiver.isEmpty()) return false;
    const auto shape = word.bodyShape(subword);
    WordResultInfo::SubWordInfo geometryOnly;
    OutlineSearchCache cache;
    SearchContext source{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                         .subword = geometryOnly, .outlineCache = &cache};
    if (!readTerminalOutline(source)) return false;
    const auto accept = [&](WordResultInfo::SubWordInfo& trial) {
        if (!plausibleRecoveredBodies(word, trial, checks) || !coversOriginalInk(word, trial)) return false;
        trial.correct = true;
        subword = std::move(trial);
        return true;
    };
    auto trial = subword;
    if (checkExisting) {
        bool complete = true;
        for (int slot = 0; slot < count; ++slot) {
            if (unresolved(trial.letterDiagnostics[slot]) && !matchAssembledBody(source, shape, config, trial, slot)) {
                complete = false;
                break;
            }
        }
        if (complete && accept(trial)) return true;
        trial = subword;
    }
    return !receiver.isEmpty() && assembleSharedTerminalEntry(source, shape, config, receiver, letters, trial) && accept(trial);
}

// An oblique cut at the lower shoulder can leave the upper stroke outside the
// body. Project from the selected form's own shoulder on the uncut outline.
static bool projectTerminalShoulder(SearchContext& context, const QJsonObject& form,
        const QJsonObject& policy, Stretching current, double bodyFloor, std::optional<double> sharedShift) {
    const auto& tip = *context.xSorted.rbegin();
    context.nextIter = std::next(context.xSorted.begin());
    if (!declaredSharedBoundary(context, form)) return false;
    orientSection(context, context.res, tip.time, true);
    const auto shoulder = context.res.bottomBaseLine;
    if (shoulder.pathIndex != 0 || !shoulder.isSharp ||
        !projectUpwardCut(context, shoulder, context.res) || context.res.topBaseLine.pathIndex != 0)
        return false;
    orientSection(context, context.res, tip.time, true);
    orientSection(context, current, tip.time, true);
    const auto& cut = context.res;
    const QLineF section(cut.topBaseLine.point, cut.bottomBaseLine.point);
    const double depth = bodyFloor - section.p1().y();
    const double reach = sharedShift.value_or(policy.value("maxShiftWidths").toDouble()) * section.length();
    const auto side = getJoinTime(cut, context.orientation);
    // A shared seam can already have the correct lower endpoint and still
    // clip the upper shoulder. Keep that lower point when replacing the seam.
    const bool keepsShoulder = sharedShift ? QLineF(section.p2(), current.bottomBaseLine.point).length() <= 1e-5 :
        side.isLeft(current.topBaseLine.time) && side.isLeft(current.bottomBaseLine.time);
    return section.dy() > 0 && section.length() <= policy.value("maxWidthToDepth").toDouble() * depth &&
        side.isStrictLeft(tip.time) && keepsShoulder &&
        cut.topBaseLine.point.y() < current.topBaseLine.point.y() - 1e-5 &&
        QLineF(section.p1(), current.topBaseLine.point).length() <= reach &&
        QLineF(section.p2(), current.bottomBaseLine.point).length() <= reach;
}

// Assemble the terminal form's own cut while preserving the earlier joins.
void applyTerminalJunction(const Word& word, WordResultInfo::SubWordInfo& sub,
        const QJsonObject& config, TerminalEntryPolicy policy) {
    const auto last = sub.joins.size();
    if (!last || sub.baseGlyphs.size() != last + 1 || sub.kashidaPaths.size() != last ||
        sub.kashidaCuts.size() != last || sub.letterDiagnostics.size() != int(last + 1)) return;
    auto diagnostic = sub.letterDiagnostics.last().toObject();
    const auto id = diagnostic.value("template").toString();
    const auto& forms = bodyCutProgram(config).forms;
    const auto selected = forms.constFind(id);
    if (selected == forms.cend()) return;
    const bool atShoulder = policy == TerminalEntryPolicy::LowerShoulder;
    const auto settings = selected->cuts.value(atShoulder ? "shoulderEntry" : "junction").toObject();
    if (settings.isEmpty()) return;

    auto join = *sub.joins.rbegin();
    if (join.counterEntry.pathIndex > 0) return;
    const auto shape = word.bodyShape(sub);
    const auto inPage = [&](const QPainterPath& path) {
        return shape.transform.map(path).translated(shape.pos);
    };
    SearchContext context{.transformedPath = inPage(shape.path), .subword = sub};
    if (!readTerminalOutline(context)) return;
    const auto form = bodyConstraintForm(config, namedLetterForm(config, id));
    InspectFormScope inspection(form, context.transformedPath);
    context.res = join;
    const bool connector = !sub.kashidaPaths.back().isEmpty();
    // Two contour corners can define the actual shared seam of a ligature.
    // Projecting past it would take the upper stroke of the preceding body.
    const bool sharedCorners = atShoulder && !connector && form.value("sharedBoundary").toBool() &&
        join.topBaseLine.isSharp && join.bottomBaseLine.isSharp;
    std::optional<double> sharedShift;
    if (sharedCorners) {
        const auto rule = settings.value("sharedCorners").toObject();
        const auto previous = sub.letterDiagnostics[int(last) - 1].toObject().value("character").toString();
        if (previous.isEmpty() || !rule.value("after").toString().contains(previous)) return;
        sharedShift = rule.value("maxShiftWidths").toDouble();
    }
    if (connector) {
        const auto& [sending, receiving] = sub.kashidaCuts.back();
        const std::vector<std::vector<QBezier>> outer{baseContourCurves(context.transformedPath)};
        const auto useEndpoint = [&](PathExtrema& point, QPointF actual) {
            if (point.point == actual && point.pathIndex == 0) return true;
            point.point = actual;
            return sourceLandmark(point, outer);
        };
        context.res.topBaseLine = join.topLeft;
        context.res.bottomBaseLine = join.bottomLeft;
        if (!useEndpoint(join.topBaseLine, sending.p1()) || !useEndpoint(join.bottomBaseLine, sending.p2()) ||
            !useEndpoint(context.res.topBaseLine, receiving.p1()) ||
            !useEndpoint(context.res.bottomBaseLine, receiving.p2())) return;
    }
    if (atShoulder) {
        const double floor = inPage(sub.baseGlyphs.back().path).boundingRect().bottom();
        if (!projectTerminalShoulder(context, form, settings, context.res, floor, sharedShift)) return;
    } else if (!refineContourJunction(context, settings)) return;
    if (!matchesBoundaryBody(context, form, context.res, false)) return;
    const bool separate = connector && cutPrecedes(join, context.res, context.orientation);
    if (connector && !separate) {
        if (!atShoulder) return;
        // Overlapping caps can share the shoulder only at the same attachment.
        const QLineF sending(join.topBaseLine.point, join.bottomBaseLine.point);
        const QLineF receiving(context.res.topBaseLine.point, context.res.bottomBaseLine.point);
        if (QLineF(sending.center(), receiving.center()).length() > receiving.length()) return;
    }
    if (separate) {
        join.topLeft = context.res.topBaseLine;
        join.bottomLeft = context.res.bottomBaseLine;
    } else {
        join.topBaseLine = join.topLeft = context.res.topBaseLine;
        join.bottomBaseLine = join.bottomLeft = context.res.bottomBaseLine;
    }
    join.topRight = join.topBaseLine;
    join.bottomRight = join.bottomBaseLine;
    join.fixedRightBoundary = true;
    join.fixedLeftBoundary = separate;
    join.length = 0;
    join.kashidaOverrides.insert("enabled", separate);
    auto trial = sub;
    trial.joins.erase(std::prev(trial.joins.end()));
    trial.joins.insert(join);
    if (!assembleBodyPartition(word, trial, config) || trial.baseGlyphs.size() != sub.baseGlyphs.size() ||
        trial.kashidaPaths.size() != last || trial.kashidaPaths.back().isEmpty() == separate) return;
    const auto unchanged = [&](const QPainterPath& original, QPainterPath& rebuilt) {
        return atShoulder ? preserveUnchangedOutline(original, rebuilt) : original == rebuilt;
    };
    for (size_t i = 0; i < last - size_t(!separate); ++i)
        if (!unchanged(sub.baseGlyphs[i].path, trial.baseGlyphs[i].path)) return;
    for (size_t i = 0; i + 1 < last; ++i)
        if (!unchanged(sub.kashidaPaths[i], trial.kashidaPaths[i]) || trial.kashidaCuts[i] != sub.kashidaCuts[i]) return;
    if (separate && trial.kashidaCuts.back().first != sub.kashidaCuts.back().first) return;
    if (atShoulder && !separate) {
        const auto before = inPage(sub.baseGlyphs[last - 1].path);
        const auto after = inPage(trial.baseGlyphs[last - 1].path);
        if (toSubpaths(before).size() != toSubpaths(after).size() ||
            std::abs(before.boundingRect().top() - after.boundingRect().top()) > 1e-5 ||
            std::abs(before.boundingRect().right() - after.boundingRect().right()) > 1e-5) return;
        const auto previous = sub.letterDiagnostics[last - 1].toObject();
        const auto previousForm = forms.constFind(previous.value("template").toString());
        if (previousForm != forms.cend() && previousForm->body.value("shape") == "tooth-cluster" &&
            !fitToothCluster(context, previousForm->body, previousForm->cuts, after, false,
                config.value("strokeWidth").toDouble(1), {.exit = context.res, .sameContour = true}, false).error.isEmpty()) return;
    }
    if (!coversOriginalInk(word, trial)) return;
    diagnostic.insert("junctionPolicy", atShoulder ? "lower-shoulder" : "inward-corners");
    diagnostic.insert("entryTemplate", id);
    trial.letterDiagnostics[trial.letterDiagnostics.size() - 1] = diagnostic;
    if (!separate) recordBoundaryTemplate(trial, int(last) - 1, false, id);
    trial.templateTrace.append(id + (atShoulder ? " · terminal cut from lower shoulder" : " · terminal cut at contour junction"));
    inspectCandidate(atShoulder ? "Terminal lower shoulder" : "Terminal contour junction", context.res);
    sub = std::move(trial);
}

} // namespace MedinaCutter

bool PageAnalysisResult::cutSubword(int lineIndex, int wordIndex, int subWordIndex, const QJsonObject& config) {
    const auto& word = page.lines[lineIndex].words[wordIndex];
    return assembleBodyPartition(word, page.lines[lineIndex].words[wordIndex].wordResultInfo.subWords[subWordIndex], config);
}

QString PageAnalysisResult::bodyLetters(const QString& text) {
    QString letters;
    for (const auto ch : text) {
        // U+0640 supplies joining/spacing (often under a combining hamza),
        // not an extra letter body. The marks remain in the source components.
        if (!ch.isMark() && ch != QChar(0x0640)) letters += ch;
    }
    return letters;
}

namespace MedinaCutter {

// The recovery search can find several ordered chords on a folded shape.
// Check their actual masks, including counters and connecting ink, before
// accepting the replacement for a partial result.
bool coversOriginalInk(const Word& word, const WordResultInfo::SubWordInfo& sub) {
    if (sub.paths.empty())
        return false;
    const auto shape = word.bodyShape(sub);
    auto world = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path);
        p.translate(shape.pos);
        return p;
    };
    auto ink = world(shape.path);
    ink.setFillRule(Qt::WindingFill);
    const auto box = ink.boundingRect();
    constexpr double scale = 12;
    const QSize size(int(std::ceil(box.width() * scale)) + 4, int(std::ceil(box.height() * scale)) + 4);
    if (size.isEmpty())
        return false;
    const QTransform transform(scale, 0, 0, scale, 2 - box.left() * scale, 2 - box.top() * scale);
    const auto inverse = transform.inverted();
    auto render = [&](const QPainterPath& path) {
        QImage image(size, QImage::Format_Grayscale8);
        image.fill(0);
        QPainter painter(&image);
        painter.setTransform(transform);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPath(path);
        painter.end();
        return image;
    };
    const auto source = render(ink);
    std::vector<QPainterPath> shapes;
    for (const auto& body : sub.baseGlyphs)
        shapes.push_back(world(body.path));
    for (const auto& join : sub.kashidaPaths)
        if (!join.isEmpty())
            shapes.push_back(world(join));
    std::vector<QImage> images;
    for (const auto& path : shapes)
        images.push_back(render(path));
    QPainterPathStroker stroke;
    stroke.setWidth(.002);
    stroke.setCurveThreshold(1e-5);
    const auto boundary = stroke.createStroke(ink);
    std::vector<const uchar*> scan(images.size());
    for (int y = 0; y < size.height(); ++y) {
        for (size_t i = 0; i < images.size(); ++i)
            scan[i] = images[i].constScanLine(y);
        const auto line = source.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            int owners = 0;
            for (auto row : scan)
                owners += row[x] != 0;
            if (owners == int(line[x] != 0))
                continue;
            const auto probe = inverse.map(QPointF(x + .5, y + .5));
            int actual = 0;
            for (const auto& path : shapes)
                actual += path.contains(probe);
            if (actual != int(ink.contains(probe)) && !(actual <= 1 && boundary.contains(probe)))
                return false;
        }
    }
    return true;
}

// Compare the two sides by arc length. A short bent connector can lie well
// outside its straight endpoint chord while both sides still follow the same
// thin stroke. A tooth or head produces a long unmatched excursion instead.
bool followsOneStroke(const QPainterPath& ribbon, const QLineF& right, const QLineF& left,
                      ConnectorEndpointMatch match) {
    auto curves = baseContourCurves(ribbon);
    if (curves.size() < 3)
        return false;
    const auto first = curves.front().pt1(), last = curves.back().pt4();
    if (QLineF(last, first).length() > 1e-6) {
        const QLineF closing(last, first);
        curves.push_back(QBezier::fromPoints(last, closing.pointAt(1. / 3), closing.pointAt(2. / 3), first));
    }
    const int n = int(curves.size());
    std::array<int, 4> role{-1, -1, -1, -1};
    const std::array<QPointF, 4> end{right.p1(), right.p2(), left.p1(), left.p2()};
    // Fixed sections can be closer together than the tolerance. Use the
    // nearest vertices for these sections and preserve the existing default.
    for (int k = 0; k < 4; ++k) {
        double nearest = .002;
        for (int i = 0; i < n; ++i) {
            const double distance = QLineF(end[k], curves[i].pt1()).length();
            if (distance < nearest) {
                role[k] = i;
                if (match == ConnectorEndpointMatch::FirstWithinTolerance) break;
                nearest = distance;
            }
        }
    }
    if (std::any_of(role.begin(), role.end(), [](int r) { return r < 0; }))
        return false;
    std::array<std::vector<QPointF>, 2> points;
    std::array<std::vector<double>, 2> distances;
    for (int k = 0; k < 2; ++k) {
        int at = role[k];
        const int forward = (at + 1) % n == role[1 - k] ? -1 : 1;
        auto& p = points[k];
        auto& d = distances[k];
        p.push_back(curves[at].pt1());
        d.push_back(0);
        for (int i = 0; i < n; ++i) {
            const auto& c = curves[(at + (forward < 0 ? n - 1 : 0)) % n];
            for (int j = 1; j <= 12; ++j) {
                const auto next = c.pointAt(forward > 0 ? j / 12. : 1 - j / 12.);
                d.push_back(d.back() + QLineF(p.back(), next).length());
                p.push_back(next);
            }
            at = (at + forward + n) % n;
            if (at == role[2] || at == role[3])
                break;
            if (at == role[1 - k])
                return false;
        }
        if (d.back() <= 1e-5)
            return false;
    }
    const double width = std::max(right.length(), left.length());
    const double shortSide = std::min(distances[0].back(), distances[1].back());
    const double longSide = std::max(distances[0].back(), distances[1].back());
    if (longSide - shortSide > 3.5 * width + .18 * shortSide ||
        (shortSide + longSide) / 2 > 2.2 * QLineF(right.center(), left.center()).length() + 2.8 * width)
        return false;
    auto sample = [&](int side, double t) {
        const auto& ds = distances[side];
        const auto& ps = points[side];
        const auto target = t * ds.back();
        const auto i =
            std::clamp(int(std::upper_bound(ds.begin(), ds.end(), target) - ds.begin()), 1, int(ds.size()) - 1);
        return QLineF(ps[i - 1], ps[i]).pointAt((target - ds[i - 1]) / std::max(ds[i] - ds[i - 1], 1e-9));
    };
    for (double f : {.15, .3, .5, .7, .85})
        if (QLineF(sample(0, f), sample(1, f)).length() > 2.7 * width)
            return false;
    return true;
}

// An open or enclosed medial hah has two lips. Across part of the head,
// a page-vertical section must meet both with a visible gap between them.
bool containsHeadLips(const QPainterPath& body, const QJsonObject& lips, QLineF* evidence) {
    const auto box = body.boundingRect();
    const double run = lips.value("minRun").toDouble(), gap = lips.value("minGap").toDouble();
    if (run <= 0 || gap <= 0 || box.width() < run || box.height() <= gap) return false;
    const int samples = std::max(1, int(std::ceil(box.width() / (.25 * run))));
    const double step = box.width() / samples;
    int consecutive = 0, longest = 0;
    std::vector<QLineF> sections;
    if (evidence) sections.resize(samples);
    for (int i = 0; i < samples; ++i) {
        const double x = box.left() + (i + .5) * step;
        auto hits = FindInterections(body, QLineF(x, box.top() - 1, x, box.bottom() + 1), true);
        std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.point.y() < b.point.y(); });
        bool divided = false, seenInk = false;
        double lastBottom = 0, lastMiddle = 0;
        for (size_t j = 1; j < hits.size(); ++j) {
            double top = hits[j - 1].point.y(), bottom = hits[j].point.y();
            if (bottom - top < .01 || !body.contains(QPointF(x, (top + bottom) / 2)))
                continue;
            if (seenInk && top - lastBottom >= gap) {
                divided = true;
                if (evidence) sections[i] = QLineF(x, lastMiddle, x, (top + bottom) / 2);
                break;
            }
            lastBottom = bottom;
            lastMiddle = (top + bottom) / 2;
            seenInk = true;
        }
        consecutive = divided ? consecutive + 1 : 0;
        if (consecutive * step >= run) {
            if (!evidence) return true;
            if (consecutive > longest) {
                longest = consecutive;
                *evidence = sections[i - consecutive / 2];
            }
        }
    }
    return longest > 0;
}

// Check the delivered partition. A normal connecting stroke can be very long.
// Only a fork with two ink bands separated by an opening is evidence of a head.
std::map<int, QLineF> missingHeadInk(const Word& word, const WordResultInfo::SubWordInfo& sub,
                                            const QString& letters, const QJsonObject& settings) {
    std::map<int, QLineF> missing;
    if (settings.isEmpty() || int(sub.baseGlyphs.size()) != letters.size() ||
        sub.kashidaPaths.size() + 1 != sub.baseGlyphs.size() || sub.kashidaCuts.size() != sub.kashidaPaths.size())
        return missing;
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& p) { return shape.transform.map(p).translated(shape.pos); };
    const auto opening = settings.value("_initialHeadLips").toObject();
    if (!opening.isEmpty() && !containsHeadLips(mapped(sub.baseGlyphs.front().path), opening))
        missing.emplace(0, QLineF{});
    const auto protectedLetters = settings.value("_connectorHeadLips").toObject();
    const int last = int(letters.size()) - 1;
    for (int letter = 1; !protectedLetters.isEmpty() && letter < last; ++letter) {
        const auto lips = protectedLetters.value(QString(letters[letter])).toObject();
        if (lips.isEmpty()) continue;
        for (const int join : {letter - 1, letter}) {
            if (sub.kashidaPaths[join].isEmpty()) continue;
            const auto ribbon = mapped(sub.kashidaPaths[join]);
            const auto& [right, left] = sub.kashidaCuts[join];
            QLineF witness;
            if (followsOneStroke(ribbon, right, left) || !containsHeadLips(ribbon, lips, &witness)) continue;
            // A folded upright can be the arm of the adjacent bowl. If the
            // medial head already has both lips, keep those source points with
            // it while the adjoining form reclaims its upright.
            const auto bounds = ribbon.boundingRect();
            if (bounds.height() > bounds.width()) {
                QLineF existing;
                if (containsHeadLips(mapped(sub.baseGlyphs[letter].path), lips, &existing)) witness = existing;
            }
            // Between two eligible letters the fork alone cannot identify its owner.
            const int neighbor = join == letter ? letter + 1 : letter - 1;
            if (neighbor != 0 && neighbor != last && protectedLetters.contains(QString(letters[neighbor])))
                witness = {};
            missing.emplace(letter, witness);
            break;
        }
    }
    return missing;
}

// A shared medial cut may take an ascender away from a valid initial proposal.
// Check the delivered body against the exposed sides and cap of the original pen stroke.
static bool retainsInitialShaft(const QPainterPath& body, const QPainterPath& source, const QJsonObject& spec) {
    const auto bounds = body.boundingRect();
    if (bounds.height() < spec.value("minHeight").toDouble() ||
        bounds.width() > spec.value("maxWidthToHeight").toDouble() * bounds.height()) return false;
    const double tol = spec.value("edgeTolerance").toDouble();
    int fullSections = 0;
    for (double fraction : {.12, .32, .55}) {
        const double y = bounds.top() + fraction * bounds.height();
        const auto span = singleHorizontalStroke(body, bounds, y);
        if (!span || span->length() < 2*tol) continue;
        if (fraction == .12 && source.contains(QPointF(span->center().x(), bounds.top()-2*tol))) return false;
        if (!source.contains(QPointF(span->x1()-tol,y)) && !source.contains(QPointF(span->x2()+tol,y)))
            ++fullSections;
    }
    return fullSections >= 2;
}

bool plausibleRecoveredBodies(const Word& word, const WordResultInfo::SubWordInfo& sub,
                                     const QJsonObject& settings) {
    const auto shape = word.bodyShape(sub);
    auto map = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path);
        p.translate(shape.pos);
        return p;
    };
    for (size_t i = 0; i < sub.kashidaPaths.size(); ++i) {
        if (sub.kashidaPaths[i].isEmpty())
            continue;
        const auto ribbon = map(sub.kashidaPaths[i]);
        const auto outlines = toSubpaths(ribbon);
        if (outlines.size() != 1)
            return false;
        const auto& [entry, exit] = sub.kashidaCuts[i];
        const auto start = entry.center(), end = exit.center();
        const double width = std::max(entry.length(), exit.length()), length = QLineF(start, end).length();
        if (width <= 1e-6 || length <= 1e-6)
            return false;
        const auto along = (end - start) / length;
        const QPointF across(-along.y(), along.x());
        const double margin = settings.value("connectorMargin").toDouble(.9) * width;
        const double bend = settings.value("connectorBend").toDouble(.25) * length + width;
        bool withinEnvelope = true;
        for (const auto& poly : ribbon.toSubpathPolygons(QTransform::fromScale(8, 8)))
            for (auto p : poly) {
                const auto d = p / 8. - start;
                const double projection = QPointF::dotProduct(d, along);
                if (projection < -margin || projection > length + margin ||
                    std::abs(QPointF::dotProduct(d, across)) > bend)
                    withinEnvelope = false;
            }
        const bool longJoin = length >= settings.value("longConnectorWidths").toDouble(4) * width;
        if ((longJoin || !withinEnvelope) && !followsOneStroke(ribbon, entry, exit))
            return false;
    }
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.isEmpty() || sub.baseGlyphs.empty())
        return false;
    const auto shaft = settings.value("initialShaft").toObject();
    if (shaft.value("letters").toString().contains(letters.front())) {
        auto source = map(shape.path);
        source.setFillRule(Qt::WindingFill);
        if (!retainsInitialShaft(map(sub.baseGlyphs.front().path), source, shaft)) return false;
    }
    const auto sizes = settings.value("bodyExtents").toArray();
    for (int i = 0; i < static_cast<int>(sub.baseGlyphs.size()); ++i) {
        const auto body = map(sub.baseGlyphs[i].path);
        const auto box = body.boundingRect();
        if (box.width() * box.height() < .12)
            return false;
        const auto position = i == 0
                                  ? QStringLiteral("initial")
                                  : (i + 1 == letters.size() ? QStringLiteral("final") : QStringLiteral("medial"));
        for (const auto& value : sizes) {
            const auto bounds = value.toObject();
            if (bounds.value("position") != position || !bounds.value("letters").toString().contains(letters[i]) ||
                (bounds.contains("template") &&
                 bounds.value("template") != sub.letterDiagnostics[i].toObject().value("template")))
                continue;
            if (box.width() < bounds.value("minWidth").toDouble() || box.height() < bounds.value("minHeight").toDouble() ||
                (bounds.contains("maxWidthToHeight") && box.width() > bounds.value("maxWidthToHeight").toDouble() * box.height()) ||
                (bounds.contains("lips") && !containsHeadLips(body, bounds.value("lips").toObject())))
                return false;
            // A descending bowl alone is insufficient: its raised head must stay above the incoming stroke.
            if (bounds.contains("minHeadRiseWidths")) {
                if (sub.joins.empty()) return false;
                const auto& join = *sub.joins.rbegin();
                QLineF entry(join.topBaseLine.point, join.bottomBaseLine.point);
                if (!sub.kashidaPaths.empty() && !sub.kashidaPaths.back().isEmpty()) {
                    if (sub.kashidaCuts.size() != sub.kashidaPaths.size()) return false;
                    entry = sub.kashidaCuts.back().second;
                }
                if (entry.isNull() || std::min(entry.y1(), entry.y2()) - box.top() <
                    bounds.value("minHeadRiseWidths").toDouble() * entry.length()) return false;
            }
        }
    }
    const auto bowl = settings.value("finalBowl").toObject();
    if (bowl.value("letters").toString().contains(letters.back())) {
        const auto full = map(shape.path).boundingRect(), final = map(sub.baseGlyphs.back().path).boundingRect();
        if (final.width() < bowl.value("minWidth").toDouble() ||
            final.height() < bowl.value("minHeight").toDouble() ||
            final.left() > full.left() + bowl.value("tipTolerance").toDouble(.2))
            return false;
    }
    return true;
}

} // namespace MedinaCutter
