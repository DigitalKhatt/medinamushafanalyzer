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
        cache.routeCuts.clear();
        cache.normalsByCenter.clear();
        auto prepared = std::make_shared<PreparedPassOutline>();
        prepared->ink = context.transformedPath;
        QVector<PathExtrema> raw;
        readContourLandmarks(prepared->ink, prepared->contours, raw);
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

const StemAttachmentEvidence* recognizedStemAttachment(const SubwordBodyRecognition& recognition,
                                                   const QJsonObject& description) {
    const auto found = recognition.stemProfiles.constFind(bodyDescriptionKey(description));
    return found == recognition.stemProfiles.cend() ? nullptr : &found.value();
}

SubwordBodyRecognition recognizeSubwordBodies(const Word& word,
        const WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    SubwordBodyRecognition result;
    if (sub.paths.empty() || sub.paths.front() < 0 || sub.paths.front() >= int(word.paths.size()) ||
        sub.secondaryBodyPath >= int(word.paths.size())) return result;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 2) return result;
    const auto& program = bodyCutProgram(config);
    const int last = letters.size() - 1;
    QMap<QByteArray, QJsonObject> descriptions;
    QSet<QString> junctionTemplates;
    for (int slot = 0; slot <= last; ++slot) {
        const size_t position = slot == 0 ? 0 : slot == last ? 2 : 1;
        const auto& forms = program.formsByPosition[position];
        const auto candidates = forms.constFind(letters[slot]);
        if (candidates == forms.cend()) continue;
        for (const auto& id : candidates.value()) {
            const auto& form = program.forms.constFind(id).value();
            if (!form.nextLetters.isEmpty() &&
                !hasFollowingLetter(letters, slot, form.nextLetters, position == 1)) continue;
            const auto addProfile = [&](const DeclaredBodyForm& profile) {
                if (hasStemFlanks(profile.body) || profile.body.contains("minFlankSpan"))
                    descriptions.insert(profile.key, profile.body);
                const auto junction = bodyCutPolicy(profile.cuts, letters, slot).value("junction").toObject();
                if (junction.value("at") == "inward-corners")
                    junctionTemplates.insert(junction.value("template").toString());
            };
            addProfile(form);
            if (slot + 1 == last) {
                const auto recovery = program.recoveryBodies.constFind(id);
                if (recovery != program.recoveryBodies.cend() && recovery->nextLetters.contains(letters.back()))
                    addProfile(recovery.value());
            }
        }
    }
    if (descriptions.isEmpty() && junctionTemplates.isEmpty()) return result;
    const auto shape = word.bodyShape(sub);
    result.source = shape.transform.map(shape.path).translated(shape.pos);
    WordResultInfo::SubWordInfo geometryOnly{};
    SearchContext context{.transformedPath = result.source, .subword = geometryOnly};
    if (!readTerminalOutline(context)) return result;
    result.orientation = context.orientation;
    result.contours = context.subPaths;
    for (const auto& point : context.timeSorted) result.landmarks.append(point);
    QMap<std::pair<double, bool>, StemForkEvidence> forkFeatures;
    for (auto it = descriptions.begin(); it != descriptions.end(); ++it) {
        const auto& body = it.value();
        StemAttachmentEvidence features;
        const auto span = body.value("minFlankSpan").toDouble();
        const bool verifyShaft = hasStemFlanks(body);
        const auto key = std::pair{span, verifyShaft};
        auto forks = forkFeatures.find(key);
        if (forks == forkFeatures.end()) forks = forkFeatures.insert(key, recognizeStemForkEvidence(context, span, verifyShaft));
        features.forkEvidence = forks.value();
        if (verifyShaft) {
            const auto curves = baseContourCurves(context.transformedPath);
            for (const auto& crest : features.forkEvidence.crests)
                features.shaftForks.push_back(stemForks(context, crest, features.forkEvidence.forks, curves));
        }
        result.stemProfiles.insert(it.key(), std::move(features));
    }
    for (const auto& id : junctionTemplates) {
        const auto receiver = resolvedLetterForm(config, id);
        Stretching attachment{};
        bool corners = false;
        if (receiver.value("current").toString().contains(letters.back()) &&
            declaredArmRoot(context, receiver, attachment, &corners) && corners)
            result.terminalJunctions.insert(id, attachment);
    }
    result.valid = true;
    return result;
}

SubwordCutPlan makeSubwordCutPlan(WordResultInfo::SubWordInfo proposals, const QJsonObject& config) {
    SubwordCutPlan plan{.partition = std::move(proposals)};
    const auto& sub = plan.partition;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (sub.letterDiagnostics.size() != letters.size()) return plan;
    const auto& program = bodyCutProgram(config);
    const std::vector<Stretching> joins(sub.joins.begin(), sub.joins.end());
    for (int slot = 0; slot < letters.size(); ++slot) {
        const auto diagnostic = sub.letterDiagnostics[slot].toObject();
        const auto id = diagnostic.value("template").toString();
        const auto& forms = diagnostic.contains("prefixTemplate") && program.recoveryBodies.contains(id)
            ? program.recoveryBodies : program.forms;
        const auto form = forms.constFind(id);
        if (form == forms.cend()) continue;
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
            add(separate ? BodyAttachmentPolicy::DetachedReturn : BodyAttachmentPolicy::SharedReturn);
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

static bool leavesConnectorRoom(const Stretching& sending, const Stretching& receiving, double minimumGap) {
    const auto chord = sending.bottomBaseLine.point - sending.topBaseLine.point;
    const QPointF along(-chord.y(), chord.x());
    const auto minimum = minimumGap * std::hypot(chord.x(), chord.y());
    const auto gap = [&](const PathExtrema& point) {
        return QPointF::dotProduct(point.point - sending.topBaseLine.point, along);
    };
    return gap(receiving.topBaseLine) > minimum && gap(receiving.bottomBaseLine) > minimum;
}

static QLineF sectionLine(const Stretching& cut) {
    return {cut.topBaseLine.point, cut.bottomBaseLine.point};
}

static bool leavesEntryClearance(const Stretching& sending, const Stretching& receiving,
                                 Orientation orientation, double minimumGap) {
    if (!cutPrecedes(sending, receiving, orientation)) return false;
    const auto start = sectionLine(sending), end = sectionLine(receiving);
    if (start.isNull() || end.isNull() || start.intersects(end, nullptr) == QLineF::BoundedIntersection)
        return false;
    // The sending cap can run along a sloping stroke. Measure the gap between
    // the finite cuts so the direction of that cap cannot hide a valid join.
    const auto distance = [](const QPointF& point, const QLineF& line) {
        const auto axis = line.p2() - line.p1();
        const double position = QPointF::dotProduct(point - line.p1(), axis) / QPointF::dotProduct(axis, axis);
        return QLineF(point, line.pointAt(std::clamp(position, 0., 1.))).length();
    };
    return std::min({distance(start.p1(), end), distance(start.p2(), end),
                     distance(end.p1(), start), distance(end.p2(), start)}) > minimumGap;
}

bool preserveUnchangedOutline(const QPainterPath& original, QPainterPath& rebuilt) {
    // Projecting an unchanged cap back onto the source can round its curve
    // time. Keep the original outline when its Bézier data agrees to within
    // that rounding error, and reject any actual change to the outline.
    if (original != rebuilt) {
        if (original.fillRule() != rebuilt.fillRule() || original.elementCount() != rebuilt.elementCount())
            return false;
        for (int i = 0; i < original.elementCount(); ++i) {
            const auto a = original.elementAt(i), b = rebuilt.elementAt(i);
            if (a.type != b.type || std::abs(a.x - b.x) > 1e-6 || std::abs(a.y - b.y) > 1e-6)
                return false;
        }
    }
    rebuilt = original;
    return true;
}

// Replace one cap of a connector, or both sides of a shared seam.
static bool placeBodyBoundary(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config,
        int bodySlot, bool atEntry, const Stretching& boundary, const std::optional<Stretching>& opposite,
        const QString& id, const QString& trace, bool oneStroke, const auto& retainsBody) {
    const int slot = bodySlot - atEntry;
    const auto& sending = atEntry && opposite ? *opposite : boundary;
    const auto receiving = atEntry && opposite ? std::optional{boundary} : opposite;
    auto trial = sub;
    const auto current = std::next(trial.joins.begin(), slot);
    const auto original = *current;
    auto join = original;
    if (join.counterEntry.pathIndex > 0) return false;
    join.topBaseLine = join.topRight = sending.topBaseLine;
    join.bottomBaseLine = join.bottomRight = sending.bottomBaseLine;
    join.topLeft = receiving ? receiving->topBaseLine : sending.topBaseLine;
    join.bottomLeft = receiving ? receiving->bottomBaseLine : sending.bottomBaseLine;
    join.referenceWidth = sectionLine(sending).length();
    join.length = 0;
    join.fixedRightBoundary = true;
    join.fixedLeftBoundary = receiving.has_value();
    join.kashidaOverrides.insert("enabled", receiving.has_value());
    trial.joins.erase(current);
    trial.joins.insert(join);
    if (!assembleBodyPartition(word, trial, config) || trial.baseGlyphs.size() != sub.baseGlyphs.size() ||
        trial.kashidaPaths.size() != sub.kashidaPaths.size() ||
        trial.kashidaPaths[slot].isEmpty() == receiving.has_value()) return false;

    // Keep the other cap of an existing connector and all unrelated regions
    // intact. A shared seam can also move the edge of its immediate neighbor.
    const int neighbor = atEntry ? slot : slot + 1;
    const auto unchanged = [&](const QPainterPath& original, QPainterPath& rebuilt) {
        return atEntry ? preserveUnchangedOutline(original, rebuilt) : original == rebuilt;
    };
    for (size_t i = 0; i < sub.baseGlyphs.size(); ++i)
        if (i != size_t(bodySlot) && (receiving || i != size_t(neighbor)) &&
            !unchanged(sub.baseGlyphs[i].path, trial.baseGlyphs[i].path)) return false;
    for (size_t i = 0; i < sub.kashidaPaths.size(); ++i)
        if (i != size_t(slot) && (!unchanged(sub.kashidaPaths[i], trial.kashidaPaths[i]) ||
            trial.kashidaCuts[i] != sub.kashidaCuts[i])) return false;
    if (receiving) {
        const auto& [start, end] = trial.kashidaCuts[slot];
        if (start != sectionLine(sending) || end != sectionLine(*receiving)) return false;
        if (oneStroke) {
            const auto shape = word.bodyShape(sub);
            const auto ribbon = shape.transform.map(trial.kashidaPaths[slot]).translated(shape.pos);
            if (toSubpaths(ribbon).size() != 1 || !followsOneStroke(ribbon, start, end)) return false;
        }
    } else {
        const auto shape = word.bodyShape(sub);
        const auto& before = sub.baseGlyphs[neighbor].path;
        const auto& after = trial.baseGlyphs[neighbor].path;
        const auto oldBounds = shape.transform.map(before).boundingRect();
        const auto newBounds = shape.transform.map(after).boundingRect();
        const auto outerEdge = [&](const QRectF& bounds) { return atEntry ? bounds.right() : bounds.left(); };
        constexpr double tolerance = 1e-5;
        if (toSubpaths(before).size() != toSubpaths(after).size() ||
            std::abs(outerEdge(oldBounds) - outerEdge(newBounds)) > tolerance) return false;
        if (std::abs(oldBounds.top() - newBounds.top()) > tolerance ||
            std::abs(oldBounds.bottom() - newBounds.bottom()) > tolerance) {
            // Moving an incoming shared cut forward can add a sloping
            // connector to the previous body and extend its bounds. Check the
            // ink between the two cuts to distinguish that from adding a head.
            if (!atEntry || !newBounds.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(oldBounds))
                return false;
            const auto regions = cutRegions(shape.path, {}, {original, sending}, sub.basePathOrientation, true);
            if (regions.size() != 3) return false;
            const auto ribbon = shape.transform.map(regions[1].path).translated(shape.pos);
            if (toSubpaths(ribbon).size() != 1 ||
                !followsOneStroke(ribbon, sectionLine(original), sectionLine(sending))) return false;
        }
    }
    if (!coversOriginalInk(word, trial) || !retainsBody(trial)) return false;
    InspectFormScope inspection(id);
    inspectCandidate("Accepted boundary", boundary);
    recordBoundaryTemplate(trial, bodySlot, atEntry, id);
    if (!opposite) recordBoundaryTemplate(trial, neighbor, !atEntry, id);
    trial.templateTrace.append(trace);
    sub = std::move(trial);
    return true;
}

static std::optional<Stretching> assembledJoinCap(const WordResultInfo::SubWordInfo& sub, int join,
        bool receiving, const SearchContext& source, std::vector<std::vector<QBezier>>& sourceCurves) {
    auto cap = *std::next(sub.joins.begin(), join);
    if (!sub.kashidaPaths[join].isEmpty()) {
        const auto& [right, left] = sub.kashidaCuts[join];
        const auto& line = receiving ? left : right;
        cap.topBaseLine = receiving ? cap.topLeft : cap.topRight;
        cap.bottomBaseLine = receiving ? cap.bottomLeft : cap.bottomRight;
        if (cap.topBaseLine.point != line.p1() || cap.bottomBaseLine.point != line.p2()) {
            if (sourceCurves.empty())
                for (const auto& contour : source.subPaths) sourceCurves.push_back(baseContourCurves(contour));
            cap.topBaseLine.point = line.p1();
            cap.bottomBaseLine.point = line.p2();
            if (!sourceLandmark(cap.topBaseLine, sourceCurves) || !sourceLandmark(cap.bottomBaseLine, sourceCurves))
                return {};
        }
    }
    if (cap.topBaseLine.pathIndex != 0 || cap.bottomBaseLine.pathIndex != 0 ||
        cap.topBaseLine.time == cap.bottomBaseLine.time) return {};
    return cap;
}

static void shareFoldedStemReturn(const Word& word, WordResultInfo::SubWordInfo& sub,
                                  const QJsonObject& config, const QString& letters) {
    const int slot = letters.size() - 2;
    if (slot < 1 || sub.joins.size() + 1 != sub.baseGlyphs.size() ||
        sub.baseGlyphs.size() != size_t(letters.size()) || sub.letterDiagnostics.size() != letters.size() ||
        sub.kashidaPaths.size() != sub.joins.size() || sub.kashidaCuts.size() != sub.joins.size() ||
        sub.kashidaPaths.back().isEmpty() || hasTatweelBeforeFinal(sub.text)) return;
    const auto id = sub.letterDiagnostics[slot].toObject().value("template").toString();
    const auto& forms = bodyCutProgram(config).forms;
    const auto form = forms.constFind(id);
    if (form == forms.cend()) return;
    const auto fold = form->cuts.value("sharedFold").toObject();
    if (!fold.value("letters").toString().contains(letters.back()) ||
        sub.joins.rbegin()->counterEntry.pathIndex > 0) return;
    const auto& [first, last] = sub.kashidaCuts.back();
    const double width = config.value("strokeWidth").toDouble(1);
    const auto descent = last.center() - first.center();
    if (descent.y() < width || std::abs(descent.x()) > fold.value("maxSlope").toDouble() * descent.y())
        return;

    const auto shape = word.bodyShape(sub);
    const auto inPage = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    WordResultInfo::SubWordInfo geometryOnly{};
    SearchContext source{.transformedPath = inPage(shape.path), .subword = geometryOnly, .baseText = letters};
    if (!readTerminalOutline(source)) return;
    std::vector<std::vector<QBezier>> curves;
    const auto seam = assembledJoinCap(sub, slot, true, source, curves);
    if (!seam || !interiorChord(source.transformedPath, seam->topBaseLine, seam->bottomBaseLine)) return;

    // Walk out of the bowl on both flanks. The first turn on each side is
    // the shared cut of this ligature. The upper end of the stem is earlier
    // on the same contour, where a fork cut would split the descending arm.
    const auto receiver = namedLetterForm(config, fold.value("template").toString());
    source.nextIter = std::next(source.xSorted.begin());
    if (receiver.isEmpty() || !declaredSharedBoundary(source, resolvedBodyForm(config, receiver))) return;
    const auto cut = sectionLine(source.res);
    if (QLineF(cut.p1(), last.p1()).length() > 1e-5 ||
        QLineF(cut.p2(), last.p2()).length() > 1e-5) return;

    const auto before = inPage(sub.baseGlyphs[slot].path);
    placeBodyBoundary(word, sub, config, slot + 1, true, *seam, {}, receiver.value("id").toString(),
        id + " · stem return kept at the final fold", false, [&](auto& trial) {
            const auto after = inPage(trial.baseGlyphs[slot].path);
            return preserveUnchangedOutline(sub.baseGlyphs.back().path, trial.baseGlyphs.back().path) &&
                toSubpaths(before).size() == toSubpaths(after).size() &&
                std::abs(before.boundingRect().top() - after.boundingRect().top()) <= 1e-5;
        });
}

void applySharedReturnEntry(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() > 2) {
        shareFoldedStemReturn(word, sub, config, letters);
        return;
    }
    if (sub.baseGlyphs.size() != 2 || sub.letterDiagnostics.size() != 2 || sub.joins.size() != 1 ||
        sub.kashidaPaths.size() != 1 || sub.kashidaCuts.size() != 1 ||
        sub.joins.begin()->counterEntry.pathIndex > 0 || sub.text.contains(QChar(0x0640))) return;
    if (letters.size() != 2) return;
    const auto line = [](const Stretching& cut) { return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point); };
    const bool hasConnector = !sub.kashidaPaths.front().isEmpty();
    const auto current = hasConnector ? sub.kashidaCuts.front().second : line(*sub.joins.begin());
    if (current.length() <= 2 * config.value("strokeWidth").toDouble(1)) return;

    const auto idAt = [&](int slot) { return sub.letterDiagnostics[slot].toObject().value("template").toString(); };
    const auto& program = bodyCutProgram(config);
    const auto initial = program.forms.constFind(idAt(0));
    if (initial == program.forms.cend()) return;
    const auto shapeType = initial->body.value("shape");
    if (shapeType != "contour-route") return;
    const bool keepStemArm = initial->cuts.value("shareTerminalReturn").toBool();
    if (keepStemArm && !hasConnector) return;
    const auto finalRules = config.value("final").toArray();
    const auto returning = std::find_if(finalRules.begin(), finalRules.end(), [&](const auto& value) {
        const auto rule = value.toObject();
        return rule.value("returningStroke").toBool() && rule.value("ownsLeftTip").toBool() &&
            rule.value("body").toObject().value("shape") == "contour-route" &&
            rule.value("current").toString().contains(letters.back()) && rule.value("id") != idAt(1);
    });
    if (returning == finalRules.end()) return;
    const auto final = bodyConstraintForm(config, returning->toObject());
    const auto start = bodyConstraintForm(config, namedLetterForm(config, idAt(0)));

    const auto shape = word.bodyShape(sub);
    const auto inPage = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    WordResultInfo::SubWordInfo geometryOnly{};
    SearchContext source{.transformedPath = inPage(shape.path), .subword = geometryOnly, .baseText = letters};
    if (!readTerminalOutline(source)) return;
    source.nextIter = std::next(source.xSorted.begin());
    source.res = {};
    if (!proposeTemplateBoundary(source, config, start, false, false, nullptr)) return;
    const auto sending = line(source.res);
    if (current.length() <= 2 * sending.length()) return;

    source.nextIter = std::next(source.xSorted.begin());
    source.res = {};
    if (!declaredSharedBoundary(source, final)) return;
    auto seam = source.res;
    const auto cut = line(seam);
    // The two forms locate the same shoulder. The short normal crosses its
    // neck, whereas the long entry slices along the returning lip.
    if (cut.length() > sending.length() || QLineF(sending.p1(), cut.p1()).length() > 1e-6 ||
        !interiorChord(source.transformedPath, seam.topBaseLine, seam.bottomBaseLine) ||
        !matchesBoundaryBody(source, start, seam, true) || !matchesBoundaryBody(source, final, seam, false)) return;
    if (hasConnector && QLineF(sub.kashidaCuts.front().first.p1(), cut.p1()).length() > 1e-6)
        return;

    if (keepStemArm) {
        // The stem turns into a short arm directly above the final fold.
        // Measure its reach against the existing fold before sharing that cut.
        const auto& startCap = sub.kashidaCuts.front().first;
        const double reach = startCap.center().x() - current.center().x();
        if (reach <= 0 || reach > current.length()) return;
        std::vector<std::vector<QBezier>> curves;
        const auto entry = assembledJoinCap(sub, 0, true, source, curves);
        if (!entry || !interiorChord(source.transformedPath, entry->topBaseLine, entry->bottomBaseLine) ||
            !matchesBoundaryBody(source, start, *entry, true)) return;
        seam = *entry;
    }

    seam.topRight = seam.topLeft = seam.topBaseLine;
    seam.bottomRight = seam.bottomLeft = seam.bottomBaseLine;
    seam.fixedRightBoundary = true;
    seam.fixedLeftBoundary = false;
    seam.length = 0;
    seam.kashidaOverrides.insert("enabled", false);
    auto trial = sub;
    trial.joins.clear();
    trial.joins.insert(seam);
    if (!assembleBodyPartition(word, trial, config) || trial.baseGlyphs.size() != 2 || trial.kashidaPaths.size() != 1 ||
        !trial.kashidaPaths.front().isEmpty() || !coversOriginalInk(word, trial)) return;
    const auto first = inPage(trial.baseGlyphs.front().path);
    const auto last = inPage(trial.baseGlyphs.back().path);
    if (toSubpaths(first).size() != 1 || toSubpaths(last).size() != 1 ||
        std::abs(first.boundingRect().top() - inPage(sub.baseGlyphs.front().path).boundingRect().top()) > 1e-6)
        return;
    if (keepStemArm) {
        if (!preserveUnchangedOutline(sub.baseGlyphs.back().path, trial.baseGlyphs.back().path)) return;
        const auto source = trial.letterDiagnostics[1].toObject().value("entryTemplate").toString(idAt(1));
        recordBoundaryTemplate(trial, 0, false, source);
        trial.templateTrace.append(idAt(1) + " · shared cut at the final fold");
    } else {
        const auto id = final.value("id").toString();
        recordAssembledForm(trial, 1, id);
        recordBoundaryTemplate(trial, 0, false, id);
        recordBoundaryTemplate(trial, 1, true, id);
        trial.templateTrace.append(id + " · shared cut across the returning lip");
    }
    sub = std::move(trial);
}

static void placeDeclaredFlankExit(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    if (sub.letterDiagnostics.isEmpty() || sub.joins.empty() || sub.kashidaPaths.empty() ||
        sub.kashidaCuts.empty() || sub.joins.begin()->counterEntry.pathIndex > 0) return;
    const auto id = sub.letterDiagnostics.first().toObject().value("template").toString();
    const auto& forms = bodyCutProgram(config).forms;
    const auto form = forms.constFind(id);
    if (form == forms.cend() || !form->cuts.contains("flankExitAngle")) return;
    const auto settings = config.value("kashidaDefaults").toObject();
    if (!settings.value("enabled").toBool(true)) return;

    const auto shape = word.bodyShape(sub);
    WordResultInfo::SubWordInfo geometryOnly{};
    SearchContext source{.transformedPath = shape.transform.map(shape.path).translated(shape.pos), .subword = geometryOnly};
    if (!readTerminalOutline(source)) return;
    std::vector<std::vector<QBezier>> curves;
    const auto sending = assembledJoinCap(sub, 0, false, source, curves);
    const auto receiving = assembledJoinCap(sub, 0, true, source, curves);
    if (!sending || !receiving) return;
    const auto start = sectionLine(*sending), end = sectionLine(*receiving);
    const double penWidth = config.value("strokeWidth").toDouble(1);
    if (start.length() < .25 * penWidth || start.length() > 2.7 * penWidth) return;
    const double clearance = penWidth * settings.value("sharedCutTolerance").toDouble(.25);
    const double spanWidths = settings.value("minSpanToStrokeRatio").toDouble(.75);
    InspectFormScope inspection(id);
    for (const auto& boundary : declaredFlankExits(source, form->body, form->cuts, penWidth)) {
        const auto cut = sectionLine(boundary);
        const auto span = cut.center() - end.center();
        if (!cutPrecedes(boundary, *sending, sub.basePathOrientation) ||
            cut.center().x() - start.center().x() < spanWidths * cut.length() ||
            span.x() < spanWidths * std::max(cut.length(), end.length()) ||
            std::abs(span.y()) > .6 * span.x() ||
            !leavesEntryClearance(boundary, *receiving, sub.basePathOrientation, clearance)) continue;
        if (placeBodyBoundary(word, sub, config, 0, false, boundary, receiving, id, id + " · exit at the tooth foot",
                              true, [](const auto&) { return true; })) return;
    }
}

bool assembleToothClusterCuts(const Word& word, WordResultInfo::SubWordInfo& sub,
                             const QJsonObject& config, bool penLiftAtEnd) {
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 2) return true;
    const auto& program = bodyCutProgram(config);
    const auto isClusterLetter = [&](QChar ch) { return program.toothClusterLetters.contains(ch); };
    if (std::none_of(letters.begin(), letters.end(), isClusterLetter)) return true;
    if (sub.baseGlyphs.size() != size_t(letters.size()) || sub.letterDiagnostics.size() != letters.size()) {
        sub.templateTrace.append("Landmark ownership rejected · missing assembled letter masks");
        sub.correct = false;
        return false;
    }

    const auto shape = word.bodyShape(sub);
    const auto inPage = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    const double penWidth = config.value("strokeWidth").toDouble(1);
    const double sharedTolerance = config.value("kashidaDefaults").toObject()
        .value("sharedCutTolerance").toDouble(.25) * penWidth;
    WordResultInfo::SubWordInfo geometryOnly{};
    auto ink = inPage(shape.path);
    ink.setFillRule(Qt::WindingFill);
    OutlineSearchCache cache;
    SearchContext source{.transformedPath = ink, .bbox = ink.boundingRect(), .subword = geometryOnly,
                         .outlineCache = &cache};
    const bool hasOutline = preparePassOutline(source, cache);
    std::vector<std::vector<QBezier>> sourceCurves;
    const bool alignedJoins = sub.joins.size() + 1 == size_t(letters.size()) &&
        sub.kashidaPaths.size() == sub.joins.size() && sub.kashidaCuts.size() == sub.joins.size();
    const auto actualCap = [&](int join, bool receiving) {
        return assembledJoinCap(sub, join, receiving, source, sourceCurves);
    };
    for (int slot = 0; slot < letters.size(); ++slot) {
        if (!isClusterLetter(letters[slot])) continue;
        auto diagnostic = sub.letterDiagnostics[slot].toObject();
        const auto id = diagnostic.value("template").toString();
        const auto component = diagnostic.value("componentTemplate").toString();
        const auto form = program.forms.constFind(component.isEmpty() ? id : component);
        QString error = QStringLiteral("no tooth-cluster landmark evidence");
        if (form != program.forms.cend() && form->body.value("shape") == "tooth-cluster") {
            const auto mask = inPage(sub.baseGlyphs[slot].path);
            const bool requiresBowl = form->cuts.value("cluster").toObject().value("terminalBowl").toBool();
            if (!component.isEmpty()) {
                // The pen lift ends the component inside the word.
                // Verify its teeth with the same bowl requirement used to cut
                // the component. A loose cluster can fit part of the free rim.
                int matches = 0;
                for (const auto& part : recognizeSourceComponents(shape.path)) {
                    SearchContext context{.transformedPath = inPage(part.ink), .subword = geometryOnly};
                    if (readTerminalOutline(context) &&
                        fitToothCluster(context, form->body, form->cuts, mask, requiresBowl, penWidth, {}, false)
                            .error.isEmpty()) ++matches;
                }
                error = matches == 1 ? QString{} : QStringLiteral("cannot verify a unique tooth cluster on the pen-lift component");
            } else if (hasOutline) {
                ToothClusterAttachment attachment;
                const bool terminal = slot + 1 == letters.size();
                const bool hasBowl = terminal && (!penLiftAtEnd || requiresBowl);
                const double minConnectorWidths = slot == 0 ? form->cuts.value("minConnectorWidths").toDouble() : 0;
                if (alignedJoins) {
                    if (slot > 0) attachment.entry = actualCap(slot - 1, true);
                    if (!terminal) {
                        attachment.exit = actualCap(slot, false);
                        attachment.separateConnector = !sub.kashidaPaths[slot].isEmpty();
                        if (attachment.separateConnector) attachment.receiving = actualCap(slot, true);
                        else if (minConnectorWidths > 0) attachment.receiving = attachment.exit;
                        if (slot + 2 == letters.size() && !hasTatweelBeforeFinal(sub.text)) {
                            const auto& variants = program.formsByPosition[slot == 0 ? 0 : 1];
                            for (const auto& variant : variants.value(letters[slot])) {
                                const auto& candidate = program.forms.constFind(variant).value();
                                if (!candidate.sharedFoldWithFinal.contains(letters.back())) continue;
                                attachment.followingFold = attachment.separateConnector ? attachment.receiving : attachment.exit;
                                break;
                            }
                        }
                    }
                    attachment.sameContour = (!slot || attachment.entry) && (terminal || attachment.exit);
                }
                attachment.requiresFold = !form->sharedFoldWithFinal.isEmpty();
                const auto canFitHollow = [&](const ToothClusterAttachment& geometry) {
                    if (!geometry.sameContour || !geometry.exit || !geometry.receiving ||
                        diagnostic.value("cutPolicy") == "tooth-exit-walk") return false;
                    if (!geometry.separateConnector ||
                        leavesConnectorRoom(*geometry.exit, *geometry.receiving, sharedTolerance)) return true;
                    // A diagonal through the connector can be both too wide
                    // and too close to the receiving cut. Test the true hollow.
                    return sectionLine(*geometry.exit).length() >
                        2 * std::max(penWidth, sectionLine(*geometry.receiving).length());
                };
                attachment.canFitHollow = canFitHollow(attachment);
                const auto fitBody = [&](const QPainterPath& candidate, const ToothClusterAttachment& geometry,
                                         bool proposeBoundaries = false) {
                    return fitToothCluster(source, form->body, form->cuts, candidate, hasBowl, penWidth,
                                           geometry, proposeBoundaries);
                };
                const auto fit = fitBody(mask, attachment, true);
                error = fit.error;
                const auto place = [&](bool entry, const Stretching& boundary, const std::optional<Stretching>& opposite,
                        const ToothClusterAttachment& revised, const QString& trace, bool openConnector = false) {
                    if (!placeBodyBoundary(word, sub, config, slot, entry, boundary, opposite, id, id + " · " + trace,
                            openConnector, [&](const auto& trial) {
                                return fitBody(inPage(trial.baseGlyphs[slot].path), revised).error.isEmpty();
                            }))
                        return false;
                    attachment = revised;
                    error.clear();
                    return true;
                };
                const auto recutExit = [&](const ToothClusterExit& option) {
                    const auto& boundary = option.cut;
                    const bool openConnector = option.atHollow && !attachment.separateConnector;
                    const auto receiving = attachment.separateConnector || openConnector
                        ? attachment.receiving : std::nullopt;
                    if ((attachment.separateConnector || openConnector) && !receiving) return false;
                    if (option.atHollow) {
                        const double width = sectionLine(*receiving).length();
                        const double gap = openConnector ? minConnectorWidths * std::max({penWidth, boundary.referenceWidth, width})
                                                         : sharedTolerance;
                        if (!leavesConnectorRoom(boundary, *receiving, gap)) return false;
                    }
                    auto revised = attachment;
                    revised.exit = boundary;
                    revised.receiving = receiving;
                    revised.separateConnector = receiving.has_value();
                    revised.canFitHollow = canFitHollow(revised);
                    return place(false, boundary, receiving, revised,
                        option.atHollow ? "exit at final tooth hollow" : "exit fitted after the third tooth", openConnector);
                };
                for (const auto& option : fit.exits) {
                    if (!recutExit(option)) continue;
                    // Pulling a cut out of a long connector can leave enough
                    // room to use the actual lower hollow of the third tooth.
                    if (!option.atHollow && attachment.canFitHollow &&
                        form->cuts.value("cluster").toObject().value("exitAtHollow").toBool()) {
                        const auto refined = fitBody(inPage(sub.baseGlyphs[slot].path), attachment, true);
                        for (const auto& exit : refined.exits)
                            if (exit.atHollow && recutExit(exit)) break;
                    }
                    break;
                }
                // Finish the outgoing boundary before changing the incoming
                // one. An entry move must preserve the accepted exit exactly.
                if (fit.entry && attachment.entry &&
                    (QLineF(fit.entry->topBaseLine.point, attachment.entry->topBaseLine.point).length() >= 1e-5 ||
                     QLineF(fit.entry->bottomBaseLine.point, attachment.entry->bottomBaseLine.point).length() >= 1e-5)) {
                    const auto& boundary = *fit.entry;
                    const bool connector = !sub.kashidaPaths[slot - 1].isEmpty();
                    const auto sending = actualCap(slot - 1, false);
                    // On a shared join the previous body may gain the stroke
                    // up to this junction, but must not lose its own landmarks.
                    const bool fits = sending && (connector
                        ? leavesEntryClearance(*sending, boundary, source.orientation, sharedTolerance)
                        : cutPrecedes(*sending, boundary, source.orientation));
                    if (fits) {
                        auto revised = attachment;
                        revised.entry = boundary;
                        place(true, boundary, connector ? sending : std::nullopt, revised, "entry at first tooth junction");
                    }
                }
            }
        }
        if (error.isEmpty()) continue;
        InspectFormScope inspection(id);
        inspectReason("Ownership", error);
        diagnostic.insert("status", "no-match");
        diagnostic.insert("template", "");
        diagnostic.insert("ownershipRejection", error);
        sub.letterDiagnostics[slot] = diagnostic;
        sub.templateTrace.append(id + " · landmark ownership rejected · " + error);
        sub.correct = false;
    }
    return sub.correct;
}

bool finalizeBodyCuts(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    if (!assembleToothClusterCuts(word, sub, config)) return false;
    applyTerminalJunction(word, sub, config, TerminalEntryPolicy::LowerShoulder);
    placeDeclaredFlankExit(word, sub, config);
    return true;
}

} // namespace MedinaCutter
