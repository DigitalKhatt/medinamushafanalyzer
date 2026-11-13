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
    if (letters.size() < 2) return result;
    const auto& program = bodyCutProgram(config);
    const int last = letters.size() - 1;
    QMap<QByteArray, std::pair<QJsonObject, bool>> descriptions;
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
                if (!lamDescription(profile.body)) return;
                auto& [body, needsAlefArm] = descriptions[profile.key];
                body = profile.body;
                needsAlefArm |= hasFollowingLetter(letters, slot, body.value("alefLetters").toString(), true);
            };
            addProfile(form);
            if (slot + 1 == last) {
                const auto recovery = program.recoveryBodies.constFind(id);
                if (recovery != program.recoveryBodies.cend() && recovery->nextLetters.contains(letters.back()))
                    addProfile(recovery.value());
            }
        }
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
        const auto& [body, needsAlefArm] = it.value();
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
        if (needsAlefArm) {
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

static bool leavesConnectorRoom(const Stretching& sending, const Stretching& receiving, double minimumGap) {
    const auto chord = sending.bottomBaseLine.point - sending.topBaseLine.point;
    const QPointF along(-chord.y(), chord.x());
    const auto minimum = minimumGap * std::hypot(chord.x(), chord.y());
    const auto gap = [&](const PathExtrema& point) {
        return QPointF::dotProduct(point.point - sending.topBaseLine.point, along);
    };
    return gap(receiving.topBaseLine) > minimum && gap(receiving.bottomBaseLine) > minimum;
}

static bool leavesEntryClearance(const Stretching& sending, const Stretching& receiving,
                                 Orientation orientation, double minimumGap) {
    if (!cutPrecedes(sending, receiving, orientation)) return false;
    const QLineF start(sending.topBaseLine.point, sending.bottomBaseLine.point);
    const QLineF end(receiving.topBaseLine.point, receiving.bottomBaseLine.point);
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

static std::optional<WordResultInfo::SubWordInfo> recutBodyJoin(
        const Word& word, const WordResultInfo::SubWordInfo& sub, int slot, int bodySlot,
        const Stretching& sending, const std::optional<Stretching>& receiving, const QJsonObject& config) {
    auto trial = sub;
    const auto current = std::next(trial.joins.begin(), slot);
    const auto original = *current;
    auto join = original;
    if (join.counterEntry.pathIndex > 0) return {};
    join.topBaseLine = join.topRight = sending.topBaseLine;
    join.bottomBaseLine = join.bottomRight = sending.bottomBaseLine;
    join.topLeft = receiving ? receiving->topBaseLine : sending.topBaseLine;
    join.bottomLeft = receiving ? receiving->bottomBaseLine : sending.bottomBaseLine;
    join.referenceWidth = QLineF(sending.topBaseLine.point, sending.bottomBaseLine.point).length();
    join.length = 0;
    join.fixedRightBoundary = true;
    join.fixedLeftBoundary = receiving.has_value();
    join.kashidaOverrides.insert("enabled", receiving.has_value());
    trial.joins.erase(current);
    trial.joins.insert(join);
    if (!assembleBodyPartition(word, trial, config) || trial.baseGlyphs.size() != sub.baseGlyphs.size() ||
        trial.kashidaPaths.size() != sub.kashidaPaths.size() ||
        trial.kashidaPaths[slot].isEmpty() == receiving.has_value()) return {};

    // Keep the other cap of an existing connector and all unrelated regions
    // intact. A shared seam can also move the edge of its immediate neighbor.
    const bool atEntry = slot < bodySlot;
    const int neighbor = atEntry ? slot : slot + 1;
    const auto unchanged = [&](const QPainterPath& original, QPainterPath& rebuilt) {
        return atEntry ? preserveUnchangedOutline(original, rebuilt) : original == rebuilt;
    };
    for (size_t i = 0; i < sub.baseGlyphs.size(); ++i)
        if (i != size_t(bodySlot) && (receiving || i != size_t(neighbor)) &&
            !unchanged(sub.baseGlyphs[i].path, trial.baseGlyphs[i].path)) return {};
    for (size_t i = 0; i < sub.kashidaPaths.size(); ++i)
        if (i != size_t(slot) && (!unchanged(sub.kashidaPaths[i], trial.kashidaPaths[i]) ||
            trial.kashidaCuts[i] != sub.kashidaCuts[i])) return {};
    if (receiving) {
        const auto& [start, end] = trial.kashidaCuts[slot];
        if (start != QLineF(sending.topBaseLine.point, sending.bottomBaseLine.point) ||
            end != QLineF(receiving->topBaseLine.point, receiving->bottomBaseLine.point)) return {};
    } else {
        const auto shape = word.bodyShape(sub);
        const auto& before = sub.baseGlyphs[neighbor].path;
        const auto& after = trial.baseGlyphs[neighbor].path;
        const auto oldBounds = shape.transform.map(before).boundingRect();
        const auto newBounds = shape.transform.map(after).boundingRect();
        const auto outerEdge = [&](const QRectF& bounds) { return atEntry ? bounds.right() : bounds.left(); };
        constexpr double tolerance = 1e-5;
        if (toSubpaths(before).size() != toSubpaths(after).size() ||
            std::abs(outerEdge(oldBounds) - outerEdge(newBounds)) > tolerance) return {};
        if (std::abs(oldBounds.top() - newBounds.top()) > tolerance ||
            std::abs(oldBounds.bottom() - newBounds.bottom()) > tolerance) {
            // Moving an incoming shared cut forward can add a sloping
            // connector to the previous body and extend its bounds. Check the
            // ink between the two cuts to distinguish that from adding a head.
            if (!atEntry || !newBounds.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(oldBounds))
                return {};
            const auto regions = cutRegions(shape.path, {}, {original, sending}, sub.basePathOrientation, true);
            if (regions.size() != 3) return {};
            const auto ribbon = shape.transform.map(regions[1].path).translated(shape.pos);
            const QLineF oldCut(original.topBaseLine.point, original.bottomBaseLine.point);
            const QLineF newCut(sending.topBaseLine.point, sending.bottomBaseLine.point);
            if (toSubpaths(ribbon).size() != 1 || !followsOneStroke(ribbon, oldCut, newCut)) return {};
        }
    }
    if (!coversOriginalInk(word, trial)) return {};
    return trial;
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

void applySharedReturnEntry(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    if (sub.baseGlyphs.size() != 2 || sub.letterDiagnostics.size() != 2 || sub.joins.size() != 1 ||
        sub.kashidaPaths.size() != 1 || sub.kashidaCuts.size() != 1 ||
        sub.joins.begin()->counterEntry.pathIndex > 0 || sub.text.contains(QChar(0x0640))) return;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
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
    if (shapeType != "contour-route" && shapeType != "contour-stem") return;
    const bool keepStemArm = shapeType == "contour-stem";
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
        trial.templateTrace.append(idAt(1) + " · shared cut at the final fold");
    } else {
        auto diagnostic = trial.letterDiagnostics[1].toObject();
        diagnostic.insert("template", final.value("id"));
        trial.letterDiagnostics[1] = diagnostic;
        trial.matchedTemplates.removeAll(idAt(1));
        trial.matchedTemplates.append(final.value("id").toString());
        trial.templateTrace.append(final.value("id").toString() + " · shared cut across the returning lip");
    }
    sub = std::move(trial);
}

void applyInitialToothExit(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    if (sub.letterDiagnostics.isEmpty() || sub.joins.empty() || sub.kashidaPaths.empty() ||
        sub.kashidaCuts.empty() || sub.joins.begin()->counterEntry.pathIndex > 0) return;
    const auto id = sub.letterDiagnostics.first().toObject().value("template").toString();
    const auto& forms = bodyCutProgram(config).forms;
    const auto form = forms.constFind(id);
    if (form == forms.cend()) return;
    const double angle = form->cuts.value("initialToothTurnAngle").toDouble();
    if (angle <= 0) return;
    const auto kashida = config.value("kashidaDefaults").toObject();
    if (!kashida.value("enabled").toBool(true)) return;
    const double penWidth = config.value("strokeWidth").toDouble(1);
    const double clearance = penWidth * kashida.value("sharedCutTolerance").toDouble(.25);
    const double minSpanWidths = kashida.value("minSpanToStrokeRatio").toDouble(.75);
    const auto shape = word.bodyShape(sub);
    const auto inPage = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    WordResultInfo::SubWordInfo geometryOnly{};
    SearchContext source{.transformedPath = inPage(shape.path), .subword = geometryOnly};
    if (!readTerminalOutline(source)) return;
    std::vector<std::vector<QBezier>> curves;
    const auto sending = assembledJoinCap(sub, 0, false, source, curves);
    const auto receiving = assembledJoinCap(sub, 0, true, source, curves);
    if (!sending || !receiving) return;
    const auto chord = [](const Stretching& cut) { return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point); };
    const auto oldEnd = chord(*sending), nextStart = chord(*receiving);
    if (oldEnd.length() < .25 * penWidth || oldEnd.length() > 2.7 * penWidth) return;

    const auto side = getJoinTime(*sending, source.orientation);
    const auto crest = std::find_if(source.ySorted.rbegin(), source.ySorted.rend(),
        [&](const auto& point) { return side.isStrictRight(point.time); });
    const auto outer = source.xSorted.begin();
    if (crest == source.ySorted.rend() || !side.isStrictRight(outer->time)) return;
    for (const auto& start : {*crest, *outer}) {
        auto boundary = strokeFlankTurn(source, start, penWidth, -1, true, angle, .5);
        if (!boundary) continue;
        if (boundary->topBaseLine.point.y() > boundary->bottomBaseLine.point.y())
            std::swap(boundary->topBaseLine, boundary->bottomBaseLine);
        const auto end = chord(*boundary);
        const auto span = end.center() - nextStart.center();
        const auto bodySide = getJoinTime(*boundary, source.orientation);
        if (!bodySide.isStrictRight(crest->time) || !bodySide.isStrictRight(outer->time) ||
            !cutPrecedes(*boundary, *sending, source.orientation) ||
            end.center().x() - oldEnd.center().x() < minSpanWidths * end.length() ||
            span.x() < minSpanWidths * std::max(end.length(), nextStart.length()) ||
            std::abs(span.y()) > .6 * span.x() || !leavesEntryClearance(*boundary, *receiving, source.orientation, clearance))
            continue;
        auto trial = recutBodyJoin(word, sub, 0, 0, *boundary, receiving, config);
        if (!trial) continue;
        const auto ribbon = inPage(trial->kashidaPaths.front());
        if (toSubpaths(ribbon).size() != 1 || !followsOneStroke(ribbon, end, nextStart)) continue;
        InspectFormScope inspection(id);
        inspectCandidate("Initial tooth exit", *boundary);
        trial->templateTrace.append(id + " · exit at the tooth foot");
        sub = std::move(*trial);
        return;
    }
}

bool verifyBodyLandmarkOwnership(const Word& word, WordResultInfo::SubWordInfo& sub,
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
                    const auto width = [](const Stretching& cut) {
                        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
                    };
                    return width(*geometry.exit) > 2 * std::max(penWidth, width(*geometry.receiving));
                };
                attachment.canFitHollow = canFitHollow(attachment);
                const auto fit = fitToothCluster(source, form->body, form->cuts, mask, hasBowl, penWidth, attachment, true);
                error = fit.error;
                const auto accept = [&](WordResultInfo::SubWordInfo trial, ToothClusterAttachment revised,
                                        const Stretching& boundary, const QString& name, const QString& trace) {
                    if (!fitToothCluster(source, form->body, form->cuts, inPage(trial.baseGlyphs[slot].path),
                                         hasBowl, penWidth, revised, false).error.isEmpty()) return false;
                    InspectFormScope inspection(id);
                    inspectCandidate(name, boundary);
                    trial.templateTrace.append(id + " · " + trace);
                    sub = std::move(trial);
                    attachment = std::move(revised);
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
                        const double width = QLineF(receiving->topBaseLine.point, receiving->bottomBaseLine.point).length();
                        const double gap = openConnector ? minConnectorWidths * std::max({penWidth, boundary.referenceWidth, width})
                                                         : sharedTolerance;
                        if (!leavesConnectorRoom(boundary, *receiving, gap)) return false;
                    }
                    auto trial = recutBodyJoin(word, sub, slot, slot, boundary, receiving, config);
                    if (!trial) return false;
                    if (openConnector) {
                        const auto ribbon = inPage(trial->kashidaPaths[slot]);
                        const auto& [sending, entry] = trial->kashidaCuts[slot];
                        if (toSubpaths(ribbon).size() != 1 || !followsOneStroke(ribbon, sending, entry)) return false;
                    }
                    auto revised = attachment;
                    revised.exit = boundary;
                    revised.receiving = receiving;
                    revised.separateConnector = receiving.has_value();
                    revised.canFitHollow = canFitHollow(revised);
                    return accept(std::move(*trial), revised, boundary,
                        option.atHollow ? "Final tooth hollow" : "Third tooth exit",
                        option.atHollow ? "exit at final tooth hollow" : "exit fitted after the third tooth");
                };
                for (const auto& option : fit.exits) {
                    if (!recutExit(option)) continue;
                    // Pulling a cut out of a long connector can leave enough
                    // room to use the actual lower hollow of the third tooth.
                    if (!option.atHollow && attachment.canFitHollow &&
                        form->cuts.value("cluster").toObject().value("exitAtHollow").toBool()) {
                        const auto refined = fitToothCluster(source, form->body, form->cuts, inPage(sub.baseGlyphs[slot].path),
                                                             hasBowl, penWidth, attachment, true);
                        for (const auto& exit : refined.exits)
                            if (exit.atHollow && recutExit(exit)) break;
                    }
                    break;
                }
                // Finish the outgoing boundary before changing the incoming
                // one. An entry move must preserve the accepted exit exactly.
                if (fit.entry) {
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
                        auto trial = recutBodyJoin(word, sub, slot - 1, slot, connector ? *sending : boundary,
                            connector ? fit.entry : std::nullopt, config);
                        if (trial) accept(std::move(*trial), revised, boundary, "First tooth junction",
                                          "entry at first tooth junction");
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
        if (std::none_of(sub.letterDiagnostics.begin(), sub.letterDiagnostics.end(), [&](const auto& entry) {
                return entry.toObject().value("template") == id;
            })) sub.matchedTemplates.removeAll(id);
        sub.templateTrace.append(id + " · landmark ownership rejected · " + error);
        sub.correct = false;
    }
    return sub.correct;
}

} // namespace MedinaCutter
