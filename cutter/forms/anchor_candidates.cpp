#include "../detail.h"

namespace MedinaCutter {

static void applyAnchorOverrides(QJsonObject& form, const QJsonObject& settings) {
    for (auto field = settings.begin(); field != settings.end(); ++field) {
        if (field.value().isNull()) form.remove(field.key());
        else form.insert(field.key(), field.value());
    }
}

QJsonObject anchorConfigForPass(QJsonObject config, AnchorPass pass) {
    const bool continuation = pass == AnchorPass::Continuation;
    for (const auto* position : {"initial", "medial", "final"}) {
        auto forms = config.value(position).toArray();
        for (qsizetype i = 0; i < forms.size(); ++i) {
            auto form = forms.at(i).toObject();
            const auto overrides = form.take("anchorOverrides").toObject();
            const auto priority = form.take("continuationPriority");
            auto standard = overrides.value("standard").toObject();
            if (continuation) standard.remove("priorityWithinPass");
            applyAnchorOverrides(form, standard);
            if (continuation) {
                if (form.value("anchorPass") == "continuation") form.remove("anchorPass");
                if (!priority.isUndefined()) form.insert("priorityWithinPass", priority);
                applyAnchorOverrides(form, overrides.value("continuation").toObject());
            }
            forms.replace(i, form);
        }
        config.insert(position, forms);
    }
    return config;
}

static QVector<PathExtrema> terminalCrest(const SearchContext& source, const Stretching& section,
                                          const QJsonObject& form) {
    QVector<PathExtrema> landmarks;
    if (!form.value("retainEntryCrest").toBool()) return landmarks;
    const auto side = getJoinTime(section, source.orientation);
    const QPointF cap = (section.topBaseLine.point + section.bottomBaseLine.point) / 2;
    const double width = QLineF(section.topBaseLine.point, section.bottomBaseLine.point).length();
    for (auto it = source.ySorted.rbegin(); it != source.ySorted.rend(); ++it) {
        const auto& landmark = *it;
        if (side.isStrictLeft(landmark.time) && landmark.point.x() <= cap.x() + width &&
            landmark.point.x() > cap.x() - 2 * width) {
            landmarks.push_back(landmark);
            break;
        }
    }
    return landmarks;
}

static std::optional<PathExtrema> shallowCrestTurn(const SearchContext& source, const Stretching& seam) {
    const auto& lower = seam.bottomBaseLine;
    const auto& upper = seam.topBaseLine;
    const auto original = getJoinTime(seam, source.orientation);
    const bool forward = (upper.time < lower.time) == original.startAtLeft;
    const auto advance = [&](auto point) { return forward ? source.prev(point) : source.next(point); };
    const auto start = source.timeSorted.find(upper);
    if (start == source.timeSorted.end()) return {};
    const auto crest = advance(start);
    const auto turn = advance(crest);
    if (crest->isSharp || !crest->isY || !turn->isSharp ||
        crest->point.y() >= upper.point.y() || crest->point.y() >= turn->point.y() ||
        turn->point.y() >= lower.point.y() || crest->point.x() >= lower.point.x() ||
        turn->point.x() <= crest->point.x() ||
        !original.isStrictRight(crest->time) || !original.isStrictRight(turn->time) ||
        QLineF(turn->point, lower.point).length() > sectionLine(seam).length() ||
        !interiorChord(source.transformedPath, *turn, lower)) return {};
    return *turn;
}

static std::optional<Stretching> risingReturnShoulder(SearchContext& source, const Stretching& seam,
                                                     const QJsonObject& settings) {
    const auto restored = cutAfterEntryCrest(source, seam, nullptr, true);
    auto entry = restored.value_or(seam);
    const auto lower = entry.bottomBaseLine;
    const auto upper = entry.topBaseLine;
    if (settings.isEmpty() || !lower.isSharp || !upper.isSharp)
        return restored;
    if (const auto turn = shallowCrestTurn(source, entry)) {
        // Keep the shallow crest and cut at the turn on its upper flank.
        entry.topBaseLine = *turn;
    } else {
        // Cross from the outside shoulder when the first tail corners
        // meet below the head on opposite sides of the returning stroke.
        if (restored || upper.point.y() <= lower.point.y() || !projectUpwardCut(source, lower, entry))
            return restored;
    }
    const QLineF shoulder = sectionLine(entry);
    const auto side = getJoinTime(entry, source.orientation);
    double floor = lower.point.y();
    for (const auto& point : source.timeSorted)
        if (side.isLeft(point.time)) floor = std::max(floor, point.point.y());
    if (!side.isStrictLeft(upper.time) ||
        shoulder.length() > settings.value("maxWidthToDepth").toDouble() * (floor - shoulder.y1()) ||
        QLineF(upper.point, shoulder.p1()).length() > settings.value("maxShiftWidths").toDouble() * shoulder.length())
        return restored;
    entry.referenceWidth = shoulder.length();
    return entry;
}

static std::optional<Stretching> initialFootExit(const SearchContext& source, const Stretching& section,
                                                double penWidth) {
    const auto axis = section.bottomBaseLine.point - section.topBaseLine.point;
    if (std::abs(axis.y()) >= std::abs(axis.x()) || QLineF(QPointF{}, axis).length() >= .75 * penWidth)
        return {};
    const auto side = getJoinTime(section, source.orientation);
    const auto& bend = section.topBaseLine;
    const bool forward = side.startAtLeft != (bend.time == side.minTime);
    auto limit = bend;
    limit.time += (forward ? 1 : -1) * int(baseContourCurves(source.transformedPath).size());
    // The inner bend can be vertical at the landmark itself. A short
    // walk exposes the lower edge of the foot to the same downward probe.
    for (const double walk : {0., .25, .5}) {
        auto top = bend;
        QPointF tangent;
        if (walk > 0 && !advanceOnContour(source.transformedPath, bend, limit, forward, walk * penWidth, top, tangent))
            continue;
        const QLineF ray(top.point, {top.point.x(), source.bbox.bottom() + 1});
        const auto hits = FindInterections(source.transformedPath, ray, true);
        for (const auto& bottom : hits) {
            if (bottom.point.y() <= top.point.y() + 1e-5) continue;
            if (bottom.point.y() - top.point.y() < .75 * penWidth ||
                bottom.point.y() - top.point.y() > 2 * penWidth ||
                !interiorChord(source.transformedPath, top, bottom)) break;
            auto cut = section;
            cut.topBaseLine = top;
            cut.bottomBaseLine = bottom;
            cut.referenceWidth = bottom.point.y() - top.point.y();
            return cut;
        }
    }
    return {};
}

QString compileAnchorForms(const QJsonObject& config, AnchorForms& forms, bool standardAlternatives) {
    forms = {};
    AnchorForms compiled;
    const auto attachmentSettings = [&](const QJsonObject& declaration) -> QJsonValue {
        const auto raw = declaration.value("upperAttachment");
        auto attachment = raw.toObject();
        auto foot = attachment.value("stemFoot").toObject();
        if (foot.isEmpty()) return raw;
        const auto stem = namedLetterForm(config, foot.value("template").toString());
        foot.insert("body", formDescription(config, stem, "body"));
        attachment.insert("stemFoot", foot);
        return attachment;
    };
    QHash<QChar, QString> foldedToothPreceders;
    for (const auto& value : config.value("medial").toArray()) {
        const auto form = value.toObject();
        const auto finals = form.value("sharedFoldWithFinal").toString();
        if (finals.isEmpty() || formDescription(config, form, "body").value("shape") != "tooth-cluster") continue;
        for (const auto letter : finals)
            foldedToothPreceders[letter] += form.value("current").toString();
    }
    const std::array positions{"initial", "medial", "final"};
    for (size_t position = 0; position < positions.size(); ++position) {
        auto declarations = config.value(positions[position]).toArray();
        if (standardAlternatives && config.contains("anchorSearch")) {
            QJsonArray candidates;
            for (const auto& value : std::as_const(declarations)) {
                candidates.append(value);
                const auto declared = value.toObject();
                auto settings = declared.value("anchorOverrides").toObject().value("standard").toObject();
                const auto priority = settings.take("priorityWithinPass");
                auto alternative = declared;
                applyAnchorOverrides(alternative, settings);
                if (alternative == declared) continue;
                if (!priority.isUndefined()) alternative.insert("priorityWithinPass", priority);
                candidates.append(alternative);
            }
            declarations = std::move(candidates);
        }
        for (const auto& value : declarations) {
            const auto declaration = value.toObject();
            const auto id = declaration.value("id").toString();
            if (declaration.contains("anchorPass")) continue;
            const auto maxLetters = declaration.value("fixedAnchorsMaxLetters");
            if (!maxLetters.isUndefined() && maxLetters.toInt() < 2)
                return id + ": fixedAnchorsMaxLetters must be an integer of at least two";
            const auto ending = declaration.value("fixedAnchorsEnding");
            if (!ending.isUndefined() && (position == 2 || !ending.isString() || ending.toString().isEmpty()))
                return id + ": fixedAnchorsEnding needs final letters on an initial or medial form";
            const auto beforeFinal = declaration.value("fixedAnchorsBeforeFinal");
            if (!beforeFinal.isUndefined() && (position != 1 || beforeFinal.toString().isEmpty()))
                return id + ": fixedAnchorsBeforeFinal needs final letters on a medial form";
            const auto priority = declaration.value("preferSharedAttachment");
            if (!priority.isUndefined() && (position != 0 || !priority.isBool() ||
                declaration.value("ligatureWith").toString().isEmpty()))
                return id + ": preferSharedAttachment requires an initial form with ligatureWith";
            auto form = bodyConstraintForm(config, declaration);
            form.remove("anchorOverrides");
            form.remove("continuationPriority");
            if (form.contains("upperAttachment")) form.insert("upperAttachment", attachmentSettings(form));
            const auto body = form.value("body").toObject();
            const auto cuts = form.value("cuts").toObject();
            const auto shape = body.value("shape");
            const auto assembly = declaration.value("assembly").toObject();
            const bool exposedHead = position == 1 && assembly.value("operation") == "exposed-head";
            if (exposedHead) {
                QJsonArray headForms;
                for (const auto& name : assembly.value("bodyForms").toArray())
                    headForms.append(bodyConstraintForm(config, namedLetterForm(config, name.toString())));
                form.insert("_headForms", headForms);
            }
            const bool supported = position == 0 ? shape == "contour-route" || shape == "horizontal-counters" ||
                shape == "suspended-loop" || shape == "counter-neck" || shape == "counter-loop" ||
                shape == "profile-neck" || shape == "tooth-cluster" || shape == "layered-head" ||
                shape == "enclosed-lips" || shape == "descending-stem" || shape == "stacked-tooth" ||
                shape == "counter-attachment" || shape == "counter-outline" :
                position == 1 ? toothDescription(body) || shape == "stacked-counters" || shape == "counter-loop" ||
                shape == "open-head" || shape == "crested-loop" || shape == "paired-route" || shape == "counter-rim" ||
                shape == "counter-attachment" || shape == "counter-outline" ||
                shape == "forked-stem" || shape == "turning-shaft" || shape == "returning-lips" ||
                shape == "enclosed-lips" || shape == "inclined-blade" || shape == "parallel-return" ||
                (shape == "tooth-cluster" && (form.contains("sharedFoldWithFinal") ||
                    form.value("cuts").toObject().value("cluster").toObject().value("entryAtJunction").toBool() &&
                    form.value("cuts").toObject().value("cluster").toObject().value("exitAtHollow").toBool())) :
                shape == "stemmed-bowl" || shape == "tail-branch" || shape == "contour-route" ||
                shape == "counter-neck" || shape == "counter-shoulder" || shape == "bowl-turn" ||
                shape == "upright-stem" || shape == "open-bowl" || shape == "profile-neck" || shape == "terminal-fork" ||
                shape == "folded-return" || shape == "rising-return" || shape == "tapered-arm" ||
                (shape == "tooth-cluster" && cuts.value("cluster").toObject().value("entryAtJunction").toBool()) ||
                shape == "forked-bowl" || shape == "counter-outline";
            if (!supported && !exposedHead)
                return id + ": this body does not supply fixed anchors for its position";
            if (shape == "stacked-tooth") {
                const auto partner = body.value("attachment").toObject().value("template").toString();
                form.insert("_upperAttachment", attachmentSettings(namedLetterForm(config, partner)));
            } else if (shape == "paired-route" && !form.contains("upperAttachment")) {
                const auto inherit = declaration.value("body").toObject().value("inherit").toString();
                if (!inherit.isEmpty()) {
                    const auto parent = namedLetterForm(config, inherit);
                    form.insert("_sharedAttachment", attachmentSettings(parent));
                }
            }
            const auto incoming = form.value("cuts").toObject().value("sharedEntryAtAttachment").toString();
            if (!incoming.isEmpty())
                form.insert("_sharedAttachment", attachmentSettings(namedLetterForm(config, incoming)));
            const auto outgoing = form.value("cuts").toObject().value("sharedExitAtAttachment").toObject();
            if (!outgoing.isEmpty())
                form.insert("_upperAttachment", attachmentSettings(namedLetterForm(config, outgoing.value("template").toString())));
            for (const auto letter : form.value("current").toString()) {
                auto& entry = compiled[position][letter].emplace_back(form);
                if (position == 2 && foldedToothPreceders.contains(letter))
                    entry.insert("_foldedToothPreceders", foldedToothPreceders.value(letter));
            }
        }
    }
    for (auto& position : compiled)
        for (auto& candidates : position)
            std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                return a.value("priorityWithinPass").toInt() < b.value("priorityWithinPass").toInt();
            });
    forms = std::move(compiled);
    return {};
}

static std::optional<PathExtrema> walkBoundaryLimit(const SearchContext& source, const PathExtrema& start,
        PathExtrema end, bool forward, double distance) {
    if (distance <= 0) return {};
    if (forward != (end.time > start.time) || end.time == start.time)
        end.time += (forward ? 1 : -1) * int(baseContourCurves(source.transformedPath).size());
    PathExtrema point;
    QPointF tangent;
    if (advanceOnContour(source.transformedPath, start, end, forward, distance, point, tangent)) return point;
    return {};
}

static std::optional<Stretching> normalAtLimit(const SearchContext& source, const PathExtrema& anchor,
        bool forward, const PathExtrema& ownedPoint, bool entry) {
    Stretching cut{};
    if (!contourNormalCut(source, anchor, forward, false, cut)) return {};
    orientSection(source, cut, ownedPoint.time, entry);
    return cut;
}

static std::optional<Stretching> insetLimit(const SearchContext& source, const AnchoredBody& body,
                                          double distance, bool entry) {
    if (distance <= 0) return {};
    const auto& cap = entry ? *body.entry : *body.exit;
    const auto& opposite = entry ? body.exit : body.entry;
    if (body.landmarks.isEmpty() && !opposite) return {};
    const auto& witness = body.landmarks.isEmpty() ? opposite->topBaseLine : body.landmarks.front();
    const auto side = getJoinTime(cap, source.orientation);
    for (const auto& end : {cap.topBaseLine, cap.bottomBaseLine}) {
        const bool forward = entry != (side.startAtLeft == (end.time == side.minTime));
        const auto point = walkBoundaryLimit(source, end, end, forward, distance);
        if (!point) continue;
        const auto limit = normalAtLimit(source, *point, forward, witness, entry);
        if (limit && (entry ? orderedSections(cap, *limit, source.orientation)
                           : orderedSections(*limit, cap, source.orientation)) &&
            retainsLandmarks(body, entry ? &*limit : body.entry ? &*body.entry : nullptr,
                entry ? body.exit ? &*body.exit : nullptr : &*limit, source.orientation))
            return limit;
    }
    return {};
}

static void keepLowerLip(const SearchContext& source, AnchoredBody& body, const MouthLips& lips,
        double distance) {
    const auto protect = [&](const PathExtrema& point) {
        if (std::none_of(body.landmarks.begin(), body.landmarks.end(), [&](const auto& mark) {
                return mark.time == point.time;
            })) body.landmarks.append(point);
    };
    protect(lips.notch);
    protect(lips.upper);
    if (distance <= 0) return;
    const auto limit = walkBoundaryLimit(source, lips.notch, lips.lower, lips.lowerForward, distance);
    if (!limit) return;
    body.exitLimit = limit;
    body.minimumExit = normalAtLimit(source, *limit, lips.lowerForward, lips.upper, false);
}

std::vector<AnchoredBody> anchoredBodies(SearchContext& source, const QJsonObject& form, size_t position,
                                        double penWidth, bool tryEntryTurn, const Stretching* sharedExit, AnchorPass pass) {
    const auto id = form.value("id").toString();
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto shape = body.value("shape");
    const bool continuation = pass == AnchorPass::Continuation;
    std::vector<AnchoredBody> result;
    InspectFormScope inspection(form, source.transformedPath);
    source.res = {};
    source.nextIter = std::next(source.xSorted.begin());
    const auto routeCut = cuts.value("route").toObject();
    const auto turn = routeCut.value("descendingTurn");
    if (shape == "contour-route" && position == 2 && turn.isDouble()) {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->route) return result;
        const auto& route = *evidence->route;
        const auto policy = cuts.value("route").toObject();
        for (const auto& [cut, fork] : descendingRouteEntries(source, route, policy,
                evidence->tail ? &*evidence->tail : nullptr)) {
            inspectLandmark("Entry limit at fork", fork);
            if (matchesBoundaryBody(source, form, cut, false))
                result.push_back({.formId = id, .entry = cut, .landmarks = {route.origin}, .entryLimit = fork});
        }
    } else if (shape == "stacked-tooth") {
        const auto attachment = body.value("attachment").toObject();
        const auto settings = form.value("_upperAttachment").toObject();
        for (auto seam : upperAttachmentSections(source, settings, UpperAttachmentMode::Original)) {
            const QLineF section(seam.topBaseLine.point, seam.bottomBaseLine.point);
            // Find the crest of the upper piece. Another letter's
            // ascender can be taller than the stacked tooth.
            const double maxCrestY = section.y1() - attachment.value("minRise").toDouble();
            for (auto it = source.ySorted.rbegin(); it != source.ySorted.rend(); ++it) {
                const auto& crest = *it;
                if (crest.point.y() > maxCrestY) break;
                if (crest.point.x() < std::min(section.x1(), section.x2())) continue;
                orientSection(source, seam, crest.time, false);
                const auto pieces = cutRegions(source.transformedPath, {}, {seam}, source.orientation, true);
                if (pieces.size() != 2) continue;
                const auto& stroke = pieces.front().path;
                const auto bounds = stroke.boundingRect();
                if (toSubpaths(stroke).size() != 1 || crest.point.y() - bounds.top() > 1.5 * penWidth ||
                    std::abs(bounds.bottom() - std::max(section.y1(), section.y2())) > 1e-5 ||
                    bounds.width() > attachment.value("maxWidthToHeight").toDouble() * bounds.height()) continue;
                // The upper pen shape can double back. Check the descending
                // arm near the attachment, below that turn.
                // The owned crest can be lower than a later letter's ascender.
                const bool preferred = (continuation || it == source.ySorted.rbegin()) &&
                    std::abs(bounds.top() - crest.point.y()) <= 1e-5 &&
                    singleHorizontalStroke(stroke, bounds, bounds.top() + .85 * bounds.height());
                if (!preferred && (!singleHorizontalStroke(stroke, bounds, bounds.top() + .90 * bounds.height()) ||
                    !singleHorizontalStroke(stroke, bounds, bounds.top() + .95 * bounds.height()))) continue;
                result.push_back({.formId = id, .exit = seam, .landmarks = {crest},
                                  .requiresSharedExit = true, .alternateAnchor = !preferred});
                break;
            }
        }
    } else if (shape == "contour-route") {
        const bool initial = position == 0;
        const auto bodyWitness = [&](const Stretching& section, const QVector<PathExtrema>& landmarks) {
            if (!continuation || initial) return landmarks;
            const auto side = getJoinTime(section, source.orientation);
            const auto pieces = cutRegions(source.transformedPath, {}, {section}, source.orientation, false);
            if (pieces.size() != 2) return landmarks;
            const auto bounds = pieces.back().path.boundingRect();
            QVector<PathExtrema> retained;
            for (const auto& mark : landmarks)
                if (side.isStrictLeft(mark.time)) retained.append(mark);
            if (retained.isEmpty() && bounds.left() <= source.bbox.left() + 1e-5) {
                for (auto it = source.xSorted.rbegin(); it != source.xSorted.rend(); ++it) {
                    if (it->point.x() > bounds.left() + .5 * penWidth) break;
                    if (side.isStrictLeft(it->time)) {
                        retained.append(*it);
                        break;
                    }
                }
            }
            return retained.isEmpty() ? landmarks : retained;
        };
        if (declaredSharedBoundary(source, form) &&
            matchesOffset(form.value("chord").toObject(), source.res.bottomBaseLine.point - source.res.topBaseLine.point) &&
            matchesBoundaryBody(source, form, source.res, initial)) {
            const auto projected = source.res;
            QVector<size_t> counters;
            if (form.contains("bodyCounters")) {
                const auto pieces = cutRegions(source.transformedPath, {}, {projected}, source.orientation, true);
                if (pieces.size() != 2) return result;
                const auto contours = toSubpaths(pieces[initial ? 0 : 1].path);
                if (contours.size() - 1 != form.value("bodyCounters").toInt()) return result;
                for (size_t i = 1; i < source.subPaths.size(); ++i)
                    if (contours.contains(source.subPaths[i])) counters.append(i);
            }
            if (initial) {
                const QLineF original(projected.topBaseLine.point, projected.bottomBaseLine.point);
                if (cuts.value("preferFootExit").toBool()) {
                    if (const auto foot = initialFootExit(source, projected, penWidth);
                        foot && matchesBoundaryBody(source, form, *foot, true))
                        result.push_back({.formId = id, .exit = foot, .landmarks = {*source.xSorted.begin()},
                                          .footExitCandidate = true});
                }
                if (cuts.value("preferFlankExit").toBool()) {
                    for (const auto& exit : declaredFlankExits(source, body, cuts, penWidth)) {
                        const QLineF shorter(exit.topBaseLine.point, exit.bottomBaseLine.point);
                        // Use a flank exit to reclaim a connector. Sub-stroke
                        // shifts at the foot belong to the contour route.
                        if (shorter.center().x() - original.center().x() < shorter.length() ||
                            !cutPrecedes(exit, projected, source.orientation) ||
                            !matchesBoundaryBody(source, form, exit, true)) continue;
                        result.push_back({.formId = id, .exit = exit, .landmarks = {*source.xSorted.begin()}});
                    }
                }
                const auto start = body.value("route").toObject().value("start");
                const auto evidence = recognizedSharedShape(source, body);
                const auto defaultOrigin = start == "top" ? *source.ySorted.rbegin() : *source.xSorted.begin();
                auto origin = start == "lower-right" && evidence->route ? evidence->route->origin : defaultOrigin;
                const bool stemReturn = cuts.value("shareTerminalReturn").toBool();
                if (stemReturn && evidence->route) {
                    result.push_back({.formId = id, .exit = projected, .landmarks = {*source.ySorted.rbegin()},
                                      .counters = counters, .hasStemReturn = true, .alternateAnchor = true});
                }
                const auto region = getJoinTime(projected, source.orientation);
                if (continuation && (!region.isStrictRight(origin.time) ||
                    origin.time == projected.topBaseLine.time || origin.time == projected.bottomBaseLine.time)) {
                    // A tall terminal can extend to the right of the initial
                    // tooth. Verify its own crest on the contour of the routed body.
                    const auto pieces = cutRegions(source.transformedPath, {}, {projected}, source.orientation, false);
                    if (pieces.size() == 2) {
                        const auto bounds = pieces.front().path.boundingRect();
                        const auto crest = std::find_if(source.ySorted.rbegin(), source.ySorted.rend(),
                            [&](const auto& p) {
                                return std::abs(p.point.y() - bounds.top()) <= 1e-5 &&
                                    region.isStrictRight(p.time);
                            });
                        if (crest != source.ySorted.rend() && bounds.height() >= penWidth) origin = *crest;
                    }
                }
                const auto ownedCrest = cuts.value("useOwnedCrest");
                auto attachmentOrigin = origin;
                if (!region.isStrictRight(origin.time) && (ownedCrest.toBool() || ownedCrest.isString())) {
                    const auto crest = std::find_if(source.ySorted.rbegin(), source.ySorted.rend(),
                        [&](const auto& landmark) { return region.isStrictRight(landmark.time); });
                    if (crest != source.ySorted.rend()) {
                        attachmentOrigin = *crest;
                        result.push_back({.formId = id, .exit = projected, .landmarks = {*crest},
                                          .counters = counters, .alternateAnchor = true});
                    }
                }
                const auto attachment = cuts.value("sharedExitAtAttachment").toObject();
                if (!attachment.isEmpty()) {
                    const bool corners = attachment.value("oppositeCorners").toBool();
                    const auto mode = corners ? UpperAttachmentMode::Corners : UpperAttachmentMode::All;
                    const auto witness = corners ? attachmentOrigin : origin;
                    for (auto seam : upperAttachmentSections(source, form.value("_upperAttachment").toObject(), mode)) {
                        orientSection(source, seam, witness.time, false);
                        const bool sameCorner = corners && seam.bottomBaseLine.time == projected.bottomBaseLine.time;
                        if ((!sameCorner && !orderedSections(projected, seam, source.orientation)) ||
                            !getJoinTime(seam, source.orientation).isStrictRight(witness.time) ||
                            QLineF(original.center(), sectionLine(seam).center()).length() >
                                attachment.value("maxDistanceWidths").toDouble() * penWidth) continue;
                        auto measured = form;
                        // The angled pen stroke can widen at a two-corner
                        // seam. Measure the body against the arm's thickness.
                        if (corners && seam.referenceWidth > 0 && measured.value("_bodyPenWidth").toDouble() <= 0)
                            measured.insert("_bodyPenWidth", seam.referenceWidth);
                        if (!matchesBoundaryBody(source, measured, seam, true)) continue;
                        result.push_back({.formId = id, .exit = seam, .landmarks = {witness},
                                          .counters = counters, .requiresSharedExit = true});
                    }
                }
                std::optional<Stretching> minimum;
                if (cuts.value("minimumExitAtFlank").toBool()) {
                    for (const auto& flank : declaredFlankExits(source, body, cuts, penWidth))
                        if (orderedSections(flank, projected, source.orientation) &&
                            matchesBoundaryBody(source, form, flank, true)) {
                            minimum = flank;
                            break;
                        }
                }
                result.push_back({.formId = id, .exit = projected, .landmarks = {origin},
                                  .counters = std::move(counters), .minimumExitAtFlank = minimum.has_value(),
                                  .minimumExitBefore = cuts.value("minimumExitBefore").toString(),
                                  .minimumExit = minimum});
            } else {
                if (form.value("retainEntryCrest").toBool()) {
                    const auto crest = cutAfterEntryCrest(source, source.res, nullptr, true);
                    // The section must rise along the return without widening
                    // into the preceding tooth or shaft.
                    if (crest && crest->topBaseLine.point.y() < source.res.topBaseLine.point.y() &&
                        crest->bottomBaseLine.point.y() < source.res.bottomBaseLine.point.y() &&
                        sectionLine(*crest).length() <= sectionLine(source.res).length() &&
                        matchesBoundaryBody(source, form, *crest, false))
                        source.res = *crest;
                }
                auto landmarks = terminalCrest(source, source.res, form);
                landmarks.push_back(*source.xSorted.rbegin());
                const auto entry = source.res;
                const bool fixedEntry = cuts.value("fixEntryJunction").toBool();
                if (cuts.value("turnBoundaries").toObject().value("right") == "stem-foot") {
                    const QLineF chord(entry.topBaseLine.point, entry.bottomBaseLine.point);
                    const double width = chord.length();
                    if (width >= .25 * penWidth && width <= 2.7 * penWidth && std::abs(chord.dy()) >= .72 * width) {
                        auto turn = strokeFlankTurn(source, entry.bottomBaseLine, width, -1, false, 43, 1.0);
                        if (turn) {
                            std::swap(turn->topBaseLine, turn->bottomBaseLine);
                            if (turn->referenceWidth <= 1.65 * width && cutPrecedes(entry, *turn, source.orientation) &&
                                matchesBoundaryBody(source, form, *turn, false))
                                result.push_back({.formId = id, .entry = *turn, .landmarks = landmarks,
                                    .stemFootSkipAfter = cuts.value("skipEntryTurnAfter").toString()});
                        }
                    }
                }
                result.push_back({.formId = id, .entry = source.res, .landmarks = bodyWitness(entry, landmarks),
                                  .fixedEntryJunction = fixedEntry,
                                  .sharesFinalEntry = continuation && form.value("sharedBoundary").toBool() &&
                                      form.value("sharedBoundaryWith").toString().isEmpty(),
                                  .shareEntryWith = form.value("sharedBoundaryWith").toString()});
                const auto shoulder = cuts.value("shoulderEntry").toObject();
                if (continuation && !shoulder.isEmpty() && entry.bottomBaseLine.isSharp) {
                    Stretching upper{};
                    const auto tip = *source.xSorted.rbegin();
                    if (projectUpwardCut(source, entry.bottomBaseLine, upper)) {
                        orientSection(source, upper, tip.time, true);
                        const QLineF projected(upper.topBaseLine.point, upper.bottomBaseLine.point);
                        const QLineF original(entry.topBaseLine.point, entry.bottomBaseLine.point);
                        if (projected.dy() > 0 && projected.y1() < original.y1() - 1e-5 &&
                            projected.length() <= shoulder.value("maxWidthToDepth").toDouble() *
                                (source.bbox.bottom() - projected.y1()) &&
                            QLineF(projected.p1(), original.p1()).length() <=
                                shoulder.value("maxShiftWidths").toDouble() * projected.length() &&
                            matchesBoundaryBody(source, form, upper, false))
                            result.insert(result.end() - 1, {.formId = id, .entry = upper,
                                          .landmarks = bodyWitness(upper, landmarks),
                                          .fixedEntryJunction = fixedEntry});
                    }
                }
            }
        }
        if (!initial && continuation) {
            const auto evidence = recognizedSharedShape(source, body);
            const auto range = cuts.value("route").toObject().value("range").toObject();
            if (evidence->tail && !range.isEmpty())
                for (auto entry : contourShoulderCuts(source, *evidence->tail,
                        body.value("route").toObject().value("shoulder").toObject(), range)) {
                    if (!entry) continue;
                    const auto tip = evidence->tail->tip;
                    orientSection(source, *entry, tip.time, true);
                    if (matchesBoundaryBody(source, form, *entry, false))
                        result.push_back({.formId = id, .entry = entry, .landmarks = {tip}, .alternateAnchor = true});
                }
        }
    } else if (position == 0 && (shape == "layered-head" || shape == "enclosed-lips")) {
        const auto evidence = recognizedSharedShape(source, body);
        std::vector<Stretching> exits;
        if (declaredSharedBoundary(source, form)) exits.push_back(source.res);
        if (continuation && evidence->layeredHead) {
            auto rims = cutLayeredHeadRims(source, *evidence->layeredHead, cuts.value("head").toObject(), penWidth);
            exits.insert(exits.end(), rims.begin(), rims.end());
        }
        for (const auto& exit : exits) {
            if (!matchesBoundaryBody(source, form, exit, true)) continue;
            const auto pieces = cutRegions(source.transformedPath, {}, {exit}, source.orientation, true);
            if (pieces.size() != 2) continue;
            const auto contours = toSubpaths(pieces.front().path);
            QVector<size_t> counters;
            for (size_t i = 1; i < source.subPaths.size(); ++i)
                if (contours.contains(source.subPaths[i])) counters.append(i);
            const bool openHead = continuation && evidence->layeredHead &&
                !evidence->layeredHead->exitCounter;
            const bool hasLips = openHead &&
                containsHeadLips(pieces.front().path, {{"minRun", .5 * penWidth}, {"minGap", .15 * penWidth}});
            if ((counters.isEmpty() && !hasLips) ||
                (form.contains("bodyCounters") && counters.size() != form.value("bodyCounters").toInt())) continue;
            if (evidence->layeredHead && evidence->layeredHead->exitCounter &&
                std::none_of(counters.begin(), counters.end(), [&](size_t i) {
                    return source.subPaths[i].boundingRect() == *evidence->layeredHead->exitCounter;
                })) continue;
            if (hasLips) counters.clear();
            result.push_back({.formId = id, .exit = exit, .landmarks = {*source.xSorted.begin()},
                              .counters = std::move(counters)});
        }
    } else if (shape == "horizontal-counters" || shape == "suspended-loop" ||
               (position == 0 && shape == "counter-neck")) {
        const auto evidence = recognizedSharedShape(source, body);
        QVector<size_t> counters;
        if (evidence->horizontalPair) {
            const auto& pair = *evidence->horizontalPair;
            counters = {pair.right.index, pair.left.index};
        } else if (evidence->head) {
            counters = {evidence->head->counter.index};
        } else if (evidence->initialNeck) {
            counters = {evidence->initialNeck->counter.index};
        }
        if (!counters.isEmpty() && declaredSharedBoundary(source, form) &&
            matchesBoundaryBody(source, form, source.res, true)) {
            const auto exit = source.res;
            const auto crest = *source.xSorted.begin();
            std::optional<Stretching> minimum;
            if (evidence->head && evidence->head->edge) {
                const auto& counter = evidence->head->counter;
                const auto cap = sectionLine(exit);
                Stretching section{};
                if (medialChordAt(source, counter.box.center().x(), cap.center().y(), 2 * cap.length(), section) &&
                    strokeNormalCut(source, section) &&
                    std::min(section.topBaseLine.point.y(), section.bottomBaseLine.point.y()) >= counter.box.bottom()) {
                    orientSection(source, section, crest.time, false);
                    if (orderedSections(section, exit, source.orientation)) {
                        const auto pieces = cutRegions(source.transformedPath, {}, {section}, source.orientation, true);
                        const QPointF rim((evidence->head->edge->point.x() + counter.box.left()) / 2,
                                          counter.box.center().y());
                        if (pieces.size() == 2 && pieces.front().path.contains(rim) &&
                            toSubpaths(pieces.front().path).contains(source.subPaths[counter.index]))
                            minimum = section;
                    }
                }
            }
            const bool exitAtTurn = cuts.value("minimumExitAtTurn").toBool();
            std::optional<Stretching> loopLimit;
            if (exitAtTurn && evidence->head && evidence->head->descendingTurn) {
                const auto& turn = *evidence->head->descendingTurn;
                const auto limit = normalAtLimit(source, turn.point, turn.forward, crest, false);
                if (limit && orderedSections(*limit, exit, source.orientation)) {
                    const auto parts = cutRegions(source.transformedPath, {}, {*limit}, source.orientation, true);
                    if (parts.size() == 2 &&
                        toSubpaths(parts.front().path).contains(source.subPaths[evidence->head->counter.index]))
                        loopLimit = limit;
                }
            }
            result.push_back({.formId = id, .exit = exit, .landmarks = {crest},
                              .counters = std::move(counters), .minimumExitAtTurn = exitAtTurn,
                              .minimumExit = loopLimit, .rimExitLimit = minimum});
        }
    } else if (shape == "stacked-counters") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = cuts.value("pair").toObject();
        for (const auto& pair : evidence->stackedPairs)
            if (const auto sections = cutStackedPair(source, pair, policy))
                result.push_back({.formId = id, .entry = sections->first, .exit = sections->second,
                                  .counters = {pair.upper.index, pair.lower.index}});
    } else if (shape == "counter-rim") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto settings = shapeSettings(form, "rim");
        for (const auto& rim : evidence->rims)
            for (const auto& [entry, exit] : cutRimBodies(source, {rim}, settings)) {
                QVector<PathExtrema> landmarks;
                if (rim.crownTime) {
                    const auto crown = std::find_if(source.timeSorted.begin(), source.timeSorted.end(),
                        [&](const auto& p) { return p.time == *rim.crownTime; });
                    if (crown != source.timeSorted.end()) landmarks.push_back(*crown);
                }
                result.push_back({.formId = id, .entry = entry, .exit = exit, .landmarks = std::move(landmarks),
                                  .counters = {rim.counter.index}, .usesSharedEntry = form.value("sharedEntry").toBool()});
            }
    } else if (shape == "counter-loop") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto settings = shapeSettings(form, "loop");
        const auto rimPolicy = cuts.value("entryRim").toObject();
        auto loops = evidence->loops;
        std::stable_sort(loops.begin(), loops.end(), [](const auto& a, const auto& b) {
            return a.anchor.box.center().x() > b.anchor.box.center().x();
        });
        for (const auto& loop : loops) {
            if (settings.value("counterPair") == "vertical" && !loop.stackedFeet) continue;
            const auto sections = cutLoopBody(source, loop, settings, position == 1);
            if (!sections) continue;
            QVector<size_t> counters{loop.anchor.index};
            if (loop.horizontalPartner) counters.append(*loop.horizontalPartner);
            if (loop.stackedFeet) counters = {loop.stackedFeet->upper.index, loop.stackedFeet->lower.index};
            if (position == 1 && counters.size() == 1) {
                const auto& entry = sections->first;
                const QLineF chord(entry.topBaseLine.point, entry.bottomBaseLine.point);
                if (needsCounterRim(chord, rimPolicy, penWidth)) {
                    for (const auto& rim : counterRimEntryCuts(source, loop.anchor.box, entry))
                        result.push_back({.formId = id, .entry = rim, .exit = sections->second,
                                          .counters = counters, .retainsCounterRim = true,
                                          .sharesRimEntry = form.value("tightJoin").toBool()});
                }
            }
            const auto attachment = cuts.value("sharedExitAtAttachment").toObject();
            if (position == 0 && !attachment.isEmpty()) {
                const auto& minimum = sections->second;
                const auto& counter = source.subPaths[loop.anchor.index];
                const auto side = getJoinTime(minimum, source.orientation);
                const auto& box = loop.anchor.box;
                const auto crest = std::find_if(source.ySorted.rbegin(), source.ySorted.rend(), [&](const auto& p) {
                    return p.point.y() < box.top() && p.point.x() >= box.left() - penWidth &&
                        p.point.x() <= box.right() + penWidth && side.isStrictRight(p.time);
                });
                if (crest != source.ySorted.rend()) {
                    for (auto seam : upperAttachmentSections(source, form.value("_upperAttachment").toObject())) {
                        orientSection(source, seam, crest->time, false);
                        if (!orderedSections(minimum, seam, source.orientation) ||
                            QLineF(sectionLine(minimum).center(), sectionLine(seam).center()).length() >
                                attachment.value("maxDistanceWidths").toDouble() * penWidth) continue;
                        const auto pieces = cutRegions(source.transformedPath, {}, {seam}, source.orientation, true);
                        if (pieces.size() == 2 && toSubpaths(pieces.front().path).contains(counter))
                            result.push_back({.formId = id, .exit = seam, .landmarks = {*crest}, .counters = counters,
                                .requiresSharedExit = true, .minimumExit = minimum});
                    }
                }
            }
            result.push_back({.formId = id,
                              .entry = position == 1 ? std::optional{sections->first} : std::nullopt,
                              .exit = sections->second,
                              .counters = std::move(counters)});
            if (continuation && position == 1 &&
                form.value("sharedEntry").toBool())
                result.back().usesSharedEntry = true;
            if (continuation && position == 1 && loop.stackedFeet &&
                cuts.value("sharedEntryAtFeet").toBool())
                result.back().sharesJunctionEntry = true;
        }
    } else if (shape == "counter-outline") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto outlineSettings = counterOutlineSettings(form);
        const auto chord = [](const Stretching& cut) {
            return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
        };
        for (const auto& outline : evidence->outlines) {
            const auto& eye = outline.eye;
            const auto& tip = position == 0 ? *source.xSorted.begin() : *source.xSorted.rbegin();
            const auto sectionsAt = [&](const char* key, bool entry) {
                std::vector<std::optional<Stretching>> sections;
                const auto attachment = outlineSettings.value(key).toObject();
                if (attachment.isEmpty()) {
                    sections.emplace_back();
                    return sections;
                }
                const auto x = attachment.value("x").toArray(), y = attachment.value("y").toArray();
                const auto width = attachment.value("width").toArray();
                if (x.size() != 2 || y.size() != 2 || width.size() != 2) return sections;
                const double maxWidth = width[1].toDouble() * eye.height();
                const double minWidth = width[0].toDouble() * eye.height();
                const QRectF bounds(QPointF(eye.left() + x[0].toDouble() * eye.width(),
                                            eye.top() + y[0].toDouble() * eye.height()),
                                    QPointF(eye.left() + x[1].toDouble() * eye.width(),
                                            eye.top() + y[1].toDouble() * eye.height()));
                const auto rim = bounds.adjusted(-maxWidth / 2, -maxWidth / 2, maxWidth / 2, maxWidth / 2);
                for (const auto& corner : source.timeSorted) {
                    if (!rim.contains(corner.point)) continue;
                    for (auto cut : cornerSections(source, corner)) {
                        const auto cap = chord(cut);
                        if (!bounds.contains(cap.center()) || cap.length() < minWidth || cap.length() > maxWidth) continue;
                        if (position != 1) orientSection(source, cut, tip.time, entry);
                        sections.emplace_back(cut);
                    }
                }
                return sections;
            };
            QVector<size_t> counters;
            for (const auto& hole : outline.counters) counters.append(hole.index);
            auto entries = sectionsAt("entry", true);
            auto exits = sharedExit ? std::vector<std::optional<Stretching>>{*sharedExit} : sectionsAt("exit", false);
            if (sharedExit && continuation && form.value("rimOffset").isDouble()) {
                Stretching rim{};
                const double x = eye.right() + form.value("rimOffset").toDouble() * eye.height();
                if (entries.empty() && medialChordAt(source, x, eye.center().y(), 1.3 * eye.height(), rim)) {
                    orientSection(source, rim, sharedExit->topBaseLine.time, true);
                    entries.push_back(rim);
                }
            }
            for (const auto& entry : entries)
                for (const auto& exit : exits) {
                    std::vector<Stretching> sections;
                    if (entry) sections.push_back(*entry);
                    if (exit) sections.push_back(*exit);
                    if (sections.size() != (position == 1 ? 2 : 1)) continue;
                    const auto parts = cutRegions(source.transformedPath, {}, sections, source.orientation, true);
                    if (parts.size() != sections.size() + 1 ||
                        !matchesCounterOutline(source, parts[entry ? 1 : 0].path, form,
                                               entry ? chord(*entry) : QLineF{}, exit ? chord(*exit) : QLineF{})) continue;
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .landmarks = position == 1 ? QVector<PathExtrema>{} : QVector<PathExtrema>{tip},
                                      .counters = counters, .requiresSharedExit = sharedExit != nullptr,
                                      .requiredExitTemplate = form.value("requiresExitTemplate").toString()});
                }
        }
    } else if (position == 2 && (shape == "folded-return" || shape == "rising-return" ||
                                shape == "tapered-arm" || shape == "forked-bowl")) {
        const auto evidence = recognizedSharedShape(source, body);
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, false)) {
            const bool shared = form.value("sharedBoundary").toBool();
            QVector<PathExtrema> landmarks;
            QVector<size_t> counters;
            if (evidence->returnSeam) landmarks = {evidence->returnSeam->tip};
            if (shape == "forked-bowl") landmarks = {*source.xSorted.rbegin()};
            if (evidence->boundaryCounter) counters = {evidence->boundaryCounter->index};
            if (cuts.value("turnBoundaries").toObject().value("right") == "stem") {
                if (const auto foot = uprightStemFoot(source, source.res, penWidth);
                    foot && matchesBoundaryBody(source, form, *foot, false))
                    result.push_back({.formId = id, .entry = *foot, .landmarks = landmarks,
                                      .counters = counters, .sharesFinalEntry = shared});
            }
            auto entry = source.res;
            std::optional<Stretching> minimum;
            if (shape == "rising-return") {
                if (const auto shoulder = risingReturnShoulder(source, entry, cuts.value("shoulderEntry").toObject())) {
                    const bool movedLower = shoulder->bottomBaseLine.point != entry.bottomBaseLine.point;
                    const bool extendsAcrossCrest = getJoinTime(*shoulder, source.orientation).isStrictLeft(entry.topBaseLine.time);
                    if ((movedLower || extendsAcrossCrest) && matchesBoundaryBody(source, form, *shoulder, false)) {
                        entry = *shoulder;
                        if (movedLower) minimum = shoulder;
                    }
                }
            }
            result.push_back({.formId = id, .entry = entry, .landmarks = std::move(landmarks),
                              .counters = std::move(counters), .sharesFinalEntry = shared,
                              .requiresDeclaredReturn = shape == "tapered-arm" &&
                                  cuts.value("arm").toObject().value("preferJunction").toBool(),
                              .minimumEntry = minimum});
        }
    } else if (shape == "counter-attachment") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = shapeSettings(form, "attachment");
        const CounterAttachmentLoop* loop = nullptr;
        if (evidence->attachment && cutCounterAttachments(source, *evidence->attachment, policy, &loop)) {
            const auto exit = source.res;
            const auto receiver = position == 0 ? form.value("ligatureWith").toString() : policy.value("letters").toString();
            // The two bridges bisect a counter and delimit a shared seam.
            if (position == 0) {
                result.push_back({.formId = id, .exit = exit, .landmarks = {*source.xSorted.begin()},
                                  .requiresSharedExit = true, .sharedCounterReceiver = receiver});
            } else {
                for (const auto& entry : loopShoulderCuts(source, loop->box, loop->shoulders,
                        policy.value("shoulder").toObject(), exit.topBaseLine.time, false))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .requiresSharedExit = true, .sharedCounterReceiver = receiver});
            }
        }
    } else if (position == 2 && shape == "profile-neck") {
        const auto evidence = recognizedSharedShape(source, body);
        for (const auto& candidate : evidence->bowlNecks) {
            if (!cutProfileNeck(source, {candidate}, nullptr, cuts.value("probe").toObject())) continue;
            const auto section = source.res;
            source.nextIter = std::next(source.xSorted.begin());
            if (matchesBoundaryBody(source, form, section, false))
                result.push_back({.formId = id, .entry = section, .sharesFinalEntry = form.value("sharedBoundary").toBool(),
                                  .entryLimit = section.topBaseLine});
        }
    } else if (position == 0 && (shape == "profile-neck" || shape == "descending-stem")) {
        if (declaredSharedBoundary(source, form) && matchesBoundaryBody(source, form, source.res, true)) {
            auto exit = source.res;
            auto landmark = shape == "descending-stem" ? *source.ySorted.rbegin() : *source.xSorted.begin();
            if (shape == "descending-stem" && continuation) {
                // The taller counterstroke of the following letter can be
                // outside this shaft's descending contour span.
                const auto side = getJoinTime(exit, source.orientation);
                if (!side.isStrictRight(landmark.time)) {
                    const auto crest = std::find_if(source.ySorted.rbegin(), source.ySorted.rend(),
                        [&](const auto& p) {
                            return p.point.y() + body.value("stem").toObject().value("minRise").toDouble() <=
                                exit.topBaseLine.point.y() && side.isStrictRight(p.time);
                        });
                    if (crest != source.ySorted.rend()) landmark = *crest;
                }
            }
            std::optional<PathExtrema> footLimit;
            if (shape == "descending-stem" && form.value("tightJoin").toBool()) {
                const auto side = getJoinTime(exit, source.orientation);
                const auto& foot = exit.bottomBaseLine;
                const bool towardShaft = side.startAtLeft == (foot.time == side.minTime);
                footLimit = walkBoundaryLimit(source, foot, landmark, towardShaft, penWidth);
            }
            result.push_back({.formId = id, .exit = exit, .landmarks = {landmark},
                              .exitLimit = footLimit});
        }
    } else if (shape == "returning-lips" || shape == "enclosed-lips" ||
               shape == "inclined-blade" || shape == "parallel-return") {
        const auto evidence = recognizedSharedShape(source, body);
        if (shape == "returning-lips") {
            const auto ownAttachment = form.value("upperAttachment").toObject();
            const auto ownSections = upperAttachmentSections(source, ownAttachment);
            const auto shared = form.value("_sharedAttachment").toObject();
            const auto attachments = upperAttachmentSections(source, shared, UpperAttachmentMode::Original);
            const auto normalAttachments = upperAttachmentSections(source, shared, UpperAttachmentMode::ArmNormal);
            for (const auto& head : evidence->mouths) {
                const auto& lips = head.lips;
                const double distance = cuts.value("minimumExitWidths").toDouble() * penWidth;
                for (const auto& [entry, exit] : cutOpenMouths(source, {head}, shapeSettings(form, "mouth"))) {
                    AnchoredBody candidate{.formId = id, .entry = entry, .exit = exit,
                        .landmarks = {lips.notch, lips.upper},
                        .usesSharedEntry = form.value("sharedEntry").toBool() || form.value("reclaimEntry").toBool()};
                    if (continuation || !ownAttachment.isEmpty()) keepLowerLip(source, candidate, lips, distance);
                    if (candidate.minimumExit && !retainsLandmarks(candidate, &entry, &exit, source.orientation))
                        candidate.exit = candidate.minimumExit;
                    if (!ownAttachment.isEmpty()) {
                        for (auto seam : ownSections) {
                            orientSection(source, seam, lips.upper.time, true);
                            if (!orderedSections(seam, *candidate.exit, source.orientation) ||
                                !retainsLandmarks(candidate, &seam, &*candidate.exit, source.orientation)) continue;
                            auto joined = candidate;
                            joined.entry = seam;
                            joined.requiresSharedEntry = true;
                            joined.requiresSharedSource = true;
                            result.push_back(std::move(joined));
                        }
                        continue;
                    }
                    if (continuation && !retainsLandmarks(candidate, &entry, &*candidate.exit, source.orientation))
                        continue;
                    for (auto seam : normalAttachments) {
                        orientSection(source, seam, lips.upper.time, true);
                        if (!orderedSections(seam, *candidate.exit, source.orientation) ||
                            !retainsLandmarks(candidate, &seam, &*candidate.exit, source.orientation)) continue;
                        auto joined = candidate;
                        joined.entry = seam;
                        joined.requiresSharedEntry = true;
                        joined.requiresSharedSource = true;
                        result.push_back(std::move(joined));
                    }
                    for (auto seam : attachments) {
                        orientSection(source, seam, lips.upper.time, true);
                        if (seam.bottomBaseLine.time != entry.bottomBaseLine.time ||
                            !orderedSections(seam, *candidate.exit, source.orientation) ||
                            !retainsLandmarks(candidate, &seam, &*candidate.exit, source.orientation)) continue;
                        candidate.upperAttachment = seam;
                        break;
                    }
                    result.push_back(std::move(candidate));
                    if (continuation) break;
                }
            }
        } else if (shape == "enclosed-lips") {
            for (const auto& head : evidence->enclosedLips)
                for (const auto& [entry, exit] : cutEnclosedLips(source, {head}, cuts.value("head").toObject(), false))
                    for (size_t i = 1; i < source.subPaths.size(); ++i)
                        if (source.subPaths[i].boundingRect() == head.counter)
                            result.push_back({.formId = id, .entry = entry, .exit = exit,
                                              .landmarks = {head.nose}, .counters = {i}});
        } else if (shape == "inclined-blade") {
            for (const auto& blade : evidence->blades) {
                const auto limit = walkBoundaryLimit(source, blade.fork, blade.fork, blade.forward,
                    continuation ? cuts.value("minimumEntryWidths").toDouble() * penWidth : 0);
                const auto minimum = limit ? normalAtLimit(source, *limit, blade.forward, blade.tip, true)
                                           : std::nullopt;
                for (const auto& [entry, exit] : cutBlades(source, {blade}, cuts.value("blade").toObject())) {
                    const auto add = [&](const Stretching& preferred) {
                        AnchoredBody candidate{.formId = id, .entry = preferred, .exit = exit,
                            .landmarks = {blade.elbow, blade.tip},
                            .usesSharedEntry = continuation &&
                                form.value("reclaimEntry").toBool(),
                            .entryLimit = limit, .minimumEntry = minimum};
                        if (minimum && !retainsLandmarks(candidate, &preferred, &exit, source.orientation))
                            candidate.entry = minimum;
                        if (continuation && !retainsLandmarks(candidate, &*candidate.entry, &exit, source.orientation))
                            return false;
                        result.push_back(std::move(candidate));
                        return true;
                    };
                    if (continuation) {
                        // The fork fixes the body limit. The template picks its
                        // preferred section before testing any neighboring body.
                        const auto turn = cuts.value("turnBoundaries").toObject().value("right") == "descender"
                            ? descenderEntry(source, entry, penWidth) : std::nullopt;
                        if (add(turn.value_or(entry))) break;
                        continue;
                    }
                    if (tryEntryTurn && cuts.value("turnBoundaries").toObject().value("right") == "descender")
                        if (const auto turn = descenderEntry(source, entry, penWidth)) add(*turn);
                    if (tryEntryTurn && !form.value("fixedAnchorsEnding").toString().isEmpty()) continue;
                    add(entry);
                }
            }
        } else {
            for (const auto& fold : evidence->parallelReturns)
                for (const auto& [entry, exit] : cutParallelReturns(source, {fold}, cuts.value("bands").toObject()))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                        .samples = {fold.first.first.center(), fold.first.second.center(),
                                    fold.second.first.center(), fold.second.second.center()}});
        }
    } else if (shape == "turning-shaft") {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->medialStem) return result;
        for (const auto& candidate : turningShaftCuts(source, *evidence->medialStem, body.value("shaft").toObject(), cuts))
            result.push_back({.formId = id, .entry = candidate.right, .exit = candidate.left,
                              .landmarks = {*candidate.stemCrest},
                              .sharedEntryFrom = cuts.value("sharedEntryCorner").toObject().value("initialTemplate").toString(),
                              .preservesTerminalBody = true,
                              .entryLimit = candidate.stemFork});
    } else if (shape == "open-head" || shape == "crested-loop") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = cuts.value("head").toObject();
        if (shape == "crested-loop") {
            for (const auto& head : evidence->crestedHeads)
                for (const auto& [entry, exit] : cutCrestedLoops(source, {head}, policy))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .landmarks = {head.crest}, .counters = {head.counter.index}});
        } else {
            for (const auto& head : evidence->openHeads)
                for (const auto& [entry, exit] : cutOpenHeads(source, {head}, policy))
                    result.push_back({.formId = id, .entry = entry, .exit = exit,
                                      .landmarks = {head.crest, head.rightEdge, head.leftEdge}});
        }
        std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return Stretching::cmpbyx{}(*a.entry, *b.entry);
        });
    } else if (shape == "forked-stem") {
        auto forkBody = body;
        forkBody.remove("entryRoute");
        forkBody.remove("exitRoutes");
        const auto evidence = recognizedSharedShape(source, forkBody);
        const double offset = cuts.value("forkOffset").toDouble();
        if (!evidence->medialStem || offset <= 0) return result;
        const auto& stems = *evidence->medialStem;
        const bool tight = cuts.value("tightJoin") == "tip-normal";
        const bool levelCap = continuation;
        for (const auto& stem : stems.stems) {
            for (const auto index : stem.forks) {
                const auto& fork = stems.features.forks[index];
                const auto entryAtTip = tight ? cutForkFlank(source, fork.tip, {}, fork.rightForward, 1, 0) : std::nullopt;
                const auto exitAtTip = tight ? cutForkFlank(source, fork.tip, {}, !fork.rightForward, -1, 0) : std::nullopt;
                const auto preferredEntry = cutForkFlank(source, fork.tip, {}, fork.rightForward, 1, offset);
                const auto preferredExit = cutForkFlank(source, fork.tip, {}, !fork.rightForward, -1, offset);
                const auto& entry = preferredEntry ? preferredEntry : entryAtTip;
                const auto& exit = preferredExit ? preferredExit : exitAtTip;
                if (!entry || !exit) continue;

                const double stroke = std::min(entry->referenceWidth, exit->referenceWidth);
                if (stroke <= 1e-6 ||
                    fork.tip.point.y() - stem.crest.point.y() < body.value("minRise").toDouble() *
                        (levelCap ? std::min(stroke, penWidth) : stroke) ||
                    !getJoinTime(*entry, source.orientation).isStrictLeft(stem.crest.time) ||
                    !getJoinTime(*exit, source.orientation).isStrictRight(stem.crest.time)) continue;
                const auto pieces = cutRegions(source.transformedPath, {}, {*entry, *exit}, source.orientation, true);
                if (pieces.size() != 3) continue;
                const auto& mask = pieces[1].path;
                const auto bounds = mask.boundingRect();
                if ((levelCap ? !bounds.adjusted(-.01, -.01, .01, .01).contains(stem.crest.point) ||
                                 stem.crest.point.y() - bounds.top() > .5 * penWidth
                               : std::abs(bounds.top() - stem.crest.point.y()) > .01) ||
                    bounds.width() > body.value("maxWidthToHeight").toDouble() * bounds.height() ||
                    (body.contains("counters") && toSubpaths(mask).size() - 1 != body.value("counters").toInt())) continue;
                result.push_back({.formId = id, .entry = *entry, .exit = *exit, .landmarks = {stem.crest},
                    .preservesTerminalBody = true,
                    .minimumEntry = entryAtTip && orderedSections(*entry, *entryAtTip, source.orientation)
                        ? entryAtTip : std::nullopt,
                    .minimumExit = exitAtTip && orderedSections(*exitAtTip, *exit, source.orientation)
                        ? exitAtTip : std::nullopt,
                    .forkEntry = std::pair{fork.tip.time, preferredEntry ? offset : 0.},
                    .sharedFold = cuts.value("sharedFold").toObject()});
            }
        }
        if (continuation && sharedExit) {
            const auto stemEvidence = recognizedSharedShape(source, body);
            if (!stemEvidence->medialStem) return result;
            for (const auto& stem : stemEvidence->medialStem->stems) {
                if (!getJoinTime(*sharedExit, source.orientation).isStrictRight(stem.crest.time)) continue;
                for (const auto& entryRoute : stem.entryRoutes) {
                    source.res = {};
                    if (!projectSavedRoute(source, entryRoute, body.value("entryRoute").toObject(),
                                           cuts.value("entryRoute").toObject()) ||
                        !cutPrecedes(source.res, *sharedExit, source.orientation)) continue;
                    const auto pieces = cutRegions(source.transformedPath, {}, {source.res, *sharedExit}, source.orientation, false);
                    if (pieces.size() != 3 ||
                        std::abs(pieces[1].path.boundingRect().top() - stem.crest.point.y()) > .01) continue;
                    result.push_back({.formId = id, .entry = source.res, .exit = *sharedExit,
                                      .landmarks = {stem.crest}, .requiresSharedExit = true});
                }
            }
        }
    } else if (shape == "paired-route") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto route = body.value("routes").toObject();
        const auto policy = cuts.value("routes").toObject();
        QStringList overlappingSenders;
        if (tryEntryTurn || continuation) {
            const auto sender = cuts.value("shareOverlappingEntryAfter");
            const auto senders = sender.isString() ? QJsonArray{sender} : sender.toArray();
            for (const auto& id : senders)
                overlappingSenders.append(id.toString());
        }
        const bool atUpperAttachment = form.contains("upperAttachment");
        const auto attachmentShape = (atUpperAttachment ? form.value("upperAttachment")
                                                       : form.value("_sharedAttachment")).toObject();
        // The two-corner seam belongs to the returning-lips body, which
        // verifies both lips before accepting it.
        const auto sharedSections = upperAttachmentSections(source, attachmentShape, UpperAttachmentMode::Probes);
        const double lipDistance = cuts.value("minimumExitWidths").toDouble() * penWidth;
        const auto lips = continuation && lipDistance > 0 ? mouthLips(source, body.value("mouth").toObject())
                                                         : std::vector<MouthLips>{};
        for (const auto& candidate : evidence->pairedRoutes) {
            for (const auto& [entry, exit] : cutPairedRoutes(source, {candidate}, route, policy, sharedExit)) {
                // The attachment corner must connect the head flank to
                // the rising stroke. The mouth can keep the same exit
                // even when its original entry walk stops above this junction.
                std::vector<std::pair<Stretching, bool>> entries{{entry, false}};
                if (atUpperAttachment) entries.clear();
                for (const auto& seam : sharedSections)
                    if (getJoinTime(seam, source.orientation).isStrictLeft(candidate.anchor.time) &&
                        cutPrecedes(seam, exit, source.orientation))
                        entries.emplace_back(seam, seam.topBaseLine.time != entry.topBaseLine.time ||
                                                   seam.bottomBaseLine.time != entry.bottomBaseLine.time);
                if (!atUpperAttachment && tryEntryTurn &&
                    cuts.value("turnBoundaries").toObject().value("right") == "descender") {
                    if (const auto turn = descenderEntry(source, entry, penWidth))
                        entries.insert(entries.begin(), {*turn, false});
                }
                for (const auto& [section, attachment] : entries) {
                    if (form.contains("bodyCounters")) {
                        const auto pieces = cutRegions(source.transformedPath, {}, {section, exit},
                                                       source.orientation, true);
                        if (pieces.size() != 3 || toSubpaths(pieces[1].path).size() - 1 !=
                            form.value("bodyCounters").toInt()) continue;
                    }
                    AnchoredBody body{.formId = id, .entry = section, .exit = exit, .landmarks = {candidate.anchor},
                                      .sharesExitAtCorners = cuts.value("sharedExit") == "receiving-corners",
                                      .usesSharedEntry = form.value("sharedEntry").toBool(),
                                      .sharesOverlappingEntryAfter = !atUpperAttachment && !attachment
                                          ? overlappingSenders : QStringList{},
                                      .requiresSharedEntry = atUpperAttachment || attachment,
                                      .requiresSharedExit = sharedExit != nullptr,
                                      .requiresSharedSource = !atUpperAttachment && attachment};
                    if (cuts.value("turnBoundaries").toObject().value("right") == "descender")
                        body.entryLimit = section.topBaseLine;
                    body.minimumEntry = insetLimit(source, body, cuts.value("minimumEntryWalk").toDouble() * penWidth, true);
                    if (!lips.empty() && body.requiresSharedEntry && !body.requiresSharedExit) {
                        const auto entrySide = getJoinTime(section, source.orientation);
                        const auto exitSide = getJoinTime(exit, source.orientation);
                        const auto inside = [&](const PathExtrema& point) {
                            return entrySide.isStrictLeft(point.time) && exitSide.isStrictRight(point.time);
                        };
                        const MouthLips* matchedLip = nullptr;
                        double nearest = std::numeric_limits<double>::infinity();
                        for (const auto& lip : lips) {
                            if (!inside(lip.notch) || !inside(lip.upper)) continue;
                            const double gap = QLineF(lip.notch.point, candidate.anchor.point).length();
                            if (gap < nearest) {
                                nearest = gap;
                                matchedLip = &lip;
                            }
                        }
                        if (matchedLip) {
                            keepLowerLip(source, body, *matchedLip, lipDistance);
                            if (body.minimumExit && !retainsLandmarks(body, &section, &exit, source.orientation))
                                body.exit = body.minimumExit;
                        }
                    }
                    if (!body.minimumExit && !body.exitLimit) {
                        body.minimumExit = insetLimit(source, body, cuts.value("minimumExitWalk").toDouble() * penWidth, false);
                        body.minimumExitBefore = cuts.value("minimumExitBefore").toString();
                    }
                    result.push_back(std::move(body));
                }
            }
        }
    } else if (shape == "tooth-cluster") {
        const auto evidence = recognizedSharedShape(source, body);
        const auto policy = cuts.value("cluster").toObject();
        const auto settings = toothClusterSettings(form);
        for (const auto& cluster : evidence->clusters) {
            if (sharedExit) {
                Stretching entry{}, exit{};
                std::array<PathExtrema, 3> tips;
                if (cutToothClusters(source, {cluster}, settings, source.bbox.left(), source.bbox.right() + 1e-6,
                                     position == 1 ? &entry : nullptr, &exit, sharedExit, &tips)) {
                    if (position == 1 && policy.value("entryAtJunction").toBool()) {
                        if (const auto junction = clusterEntryAtJunction(source, cluster, penWidth);
                            junction && cutPrecedes(*junction, *sharedExit, source.orientation)) entry = *junction;
                    }
                    result.push_back({.formId = id,
                                      .entry = position == 1 ? std::optional{entry} : std::nullopt,
                                      .exit = *sharedExit, .landmarks = {tips.begin(), tips.end()},
                                      .usesSharedEntry = form.value("sharedEntry").toBool(), .requiresSharedExit = true});
                }
            } else if (position == 0) {
                if (!cluster.ordinary && cluster.wide) {
                    std::optional<Stretching> minimum;
                    if (const auto exit = wideClusterExit(source, cluster, settings, minimum))
                        result.push_back({.formId = id, .exit = exit,
                            .landmarks = {cluster.turns.begin(), cluster.turns.end()}, .minimumExit = minimum});
                    continue;
                }
                const auto minimum = cuts.value("minimumExitAtBend").toBool()
                    ? clusterExitAtBend(source, cluster, settings) : std::nullopt;
                const auto addPreferred = [&](const Stretching& exit) {
                    result.push_back({.formId = id, .exit = exit, .landmarks = {cluster.turns.begin(), cluster.turns.end()},
                        .minimumExitBefore = cuts.value("minimumExitBefore").toString(),
                        .minimumExit = minimum && orderedSections(*minimum, exit, source.orientation) ? minimum : std::nullopt});
                };
                const auto firstCandidate = result.size();
                if (policy.value("exitAtHollow").toBool())
                    if (const auto exit = clusterExitAtHollow(source, cluster, penWidth, 2 * penWidth, policy))
                        addPreferred(*exit);
                Stretching projected{};
                if (cutToothClusters(source, {cluster}, settings, source.bbox.left(), source.bbox.right() + 1e-6,
                                     nullptr, &projected))
                    addPreferred(projected);
                if (result.size() == firstCandidate)
                    if (const auto bend = minimum ? minimum : clusterExitAtBend(source, cluster, settings))
                        addPreferred(*bend);
            } else if (position == 2) {
                Stretching entry{};
                std::optional<std::array<PathExtrema, 2>> bowl;
                if (!cutToothClusters(source, {cluster}, settings, source.bbox.left(),
                                      source.bbox.right() + 1e-6, &entry, nullptr, nullptr, nullptr, &bowl, penWidth))
                    continue;
                QVector<PathExtrema> landmarks(cluster.turns.begin(), cluster.turns.end());
                if (bowl) {
                    landmarks.append(bowl->front());
                    landmarks.append(bowl->back());
                } else if (!settings.value("_penLiftExit").toBool()) continue;
                result.push_back({.formId = id, .entry = entry, .landmarks = std::move(landmarks),
                    .fixedEntryJunction = true, .keepsEntryJunction = true});
            } else {
                const auto geometry = body.value("cluster").toObject();
                if (!cluster.ordinary && cluster.wide) {
                    const auto entry = clusterEntryAtJunction(source, cluster, penWidth,
                        policy.value("entryCornerOptional").toBool());
                    std::optional<Stretching> minimum;
                    const auto exit = entry ? wideClusterExit(source, cluster, settings, minimum) : std::nullopt;
                    if (exit && orderedSections(*entry, *exit, source.orientation))
                        result.push_back({.formId = id, .entry = entry, .exit = exit,
                            .landmarks = {cluster.turns.begin(), cluster.turns.end()},
                            .fixedEntryJunction = true, .keepsEntryJunction = true, .minimumExit = minimum});
                    continue;
                }
                const auto sections = medialClusterAnchors(source, cluster, evidence->clusters, geometry, policy, penWidth);
                if (!sections) continue;
                result.push_back({.formId = id, .entry = sections->first, .exit = sections->second,
                                  .landmarks = {cluster.turns.begin(), cluster.turns.end()},
                                  .sharesJunctionEntry = form.value("sharedEntry").toBool(),
                                  .sharesLoopNeck = policy.value("shareLoopNeck").toBool(),
                                  .fixedEntryJunction = true, .keepsEntryJunction = true, .keepsExitHollow = true,
                                  .toothExitFlank = {{cluster.turns.back(), cluster.advancePrevious}}});
                auto& candidate = result.back();
                candidate.minimumExit = insetLimit(source, candidate,
                    cuts.value("minimumExitWalk").toDouble() * penWidth, false);
                candidate.minimumExitBefore = cuts.value("minimumExitBefore").toString();
            }
        }
        std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return a.landmarks.front().point.x() > b.landmarks.front().point.x();
        });
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
            result.back().sharesIncoming = cuts.value("tightJoin") == "shared-boundary";
            result.back().sharesLoopNeck = cuts.value("shareLoopNeck").toBool();
            auto& candidate = result.back();
            candidate.minimumEntry = insetLimit(source, candidate, cuts.value("minimumEntryWalk").toDouble() * penWidth, true);
            candidate.minimumEntryAfterForm = cuts.value("minimumEntryAfterForm").toString();
        }
    } else if (shape == "upright-stem") {
        const auto evidence = recognizedSharedShape(source, body);
        Stretching foot{};
        const bool hasFoot = evidence->descendingShaft &&
            cutDescendingShaft(source, *evidence->descendingShaft, cuts.value("stem").toObject(), foot);
        std::optional<Stretching> stemSection;
        if (evidence->uprightStem && declaredSharedBoundary(source, form)) {
            stemSection = source.res;
            if (cuts.value("turnBoundaries").toObject().value("right") == "stem") {
                if (const auto turn = uprightStemFoot(source, *stemSection, penWidth))
                    stemSection = *turn;
            }
        }
        if (hasFoot || stemSection) {
            const auto& tip = hasFoot ? evidence->descendingShaft->tip : evidence->uprightStem->tip;
            if (hasFoot) orientSection(source, foot, tip.time, true);
            if (stemSection) orientSection(source, *stemSection, tip.time, true);
            const bool transverse = stemSection && std::abs(sectionLine(*stemSection).dy()) <=
                .05 * sectionLine(*stemSection).length();
            const bool useFoot = hasFoot && (!stemSection || transverse);
            // A measured turn is preferred. A higher section of the same
            // shaft can bound how far an overlapping join may move the cut.
            AnchoredBody candidate{.formId = id, .entry = useFoot ? foot : *stemSection, .landmarks = {tip}};
            if (hasFoot) {
                candidate.samples.append(evidence->descendingShaft->section.center());
                const auto& limit = useFoot ? stemSection : std::optional{foot};
                if (limit && orderedSections(*candidate.entry, *limit, source.orientation) &&
                    QLineF(sectionLine(*candidate.entry).center(), sectionLine(*limit).center()).length() <=
                        evidence->descendingShaft->width)
                    candidate.minimumEntry = limit;
            }
            result.push_back(std::move(candidate));
        }
    } else if (shape == "terminal-fork") {
        Stretching entry{};
        PathExtrema fork{};
        std::optional<Stretching> minimum;
        if (cutTerminalStemFork(source, shapeSettings(form, "stem"), entry, &fork, &minimum))
            result.push_back({.formId = id, .entry = entry, .landmarks = {*source.xSorted.rbegin()},
                .fixedEntryJunction = !minimum && (entry.topBaseLine.time == fork.time ||
                    entry.bottomBaseLine.time == fork.time),
                .entryLimit = fork, .minimumEntry = minimum});
    } else if (shape == "stemmed-bowl") {
        const auto evidence = recognizedSharedShape(source, body);
        auto stems = evidence->terminalStems;
        std::stable_sort(stems.begin(), stems.end(), [](const auto& a, const auto& b) {
            return a.crest.point.x() < b.crest.point.x();
        });
        const auto settings = shapeSettings(form, "stem"), policy = cuts.value("stem").toObject();
        WordResultInfo::SubWordInfo geometryOnly;
        SearchContext forkOutline{.transformedPath = source.transformedPath, .subword = geometryOnly};
        StemForkEvidence forks;
        const bool forkEntry = cuts.value("boundaries").toObject().value("right") == "fork-normal";
        if (forkEntry && readTerminalOutline(forkOutline))
            forks = recognizeStemForkEvidence(forkOutline, body.value("minFlankSpan").toDouble(), false);
        for (const auto& stem : stems) {
            if (!cutTerminalStemBodies(source, {stem}, settings, policy)) continue;
            const AnchoredBody candidate{.formId = id, .entry = source.res, .landmarks = {stem.crest, stem.bowlEdge},
                .samples = {stem.sample, stem.bowlBottom - QPointF(0, .01 * stem.stroke)},
                .stemmedBowl = true};
            const auto side = getJoinTime(*candidate.entry, source.orientation);
            const double minRise = body.value("minRise").toDouble();
            const double maxLean = body.value("maxWidthToHeight").toDouble();
            const StemFork* best = nullptr;
            double bestLean = std::numeric_limits<double>::infinity();
            const StemFork* entryFork = nullptr;
            for (const auto& fork : forks.forks) {
                if (!fork.isConcave(source.orientation) || (!fork.fork && !fork.folded())) continue;
                const double rise = fork.tip.point.y() - stem.crest.point.y();
                const double lean = std::abs(fork.tip.point.x() - stem.crest.point.x());
                if (rise < minRise || lean > maxLean * rise) continue;
                // The preliminary stem cut may already meet the fork on its lower edge.
                if (fork.tip.time == candidate.entry->bottomBaseLine.time) entryFork = &fork;
                if (!side.isStrictLeft(fork.tip.time) || lean / rise >= bestLean) continue;
                best = &fork;
                bestLean = lean / rise;
            }
            if (!best) best = entryFork;
            if (best) {
                for (double distance : {cuts.value("forkOffset").toDouble(), 0.}) {
                    if (distance == 0 && cuts.value("tightJoin") != "tip-normal") continue;
                    const auto entry = cutForkFlank(forkOutline, best->tip, {}, best->rightForward, 1, distance);
                    if (!entry) continue;
                    auto atFork = candidate;
                    atFork.entry = entry;
                    atFork.entryLimit = best->tip;
                    atFork.stemFootSkipAfter = cuts.value("preserveEntryAfter").toString();
                    result.push_back(std::move(atFork));
                }
            }
            result.push_back(candidate);
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
    } else if (shape == "open-bowl") {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->openBowl || !cutOpenBowl(source, *evidence->openBowl, cuts.value("bowl").toObject()))
            return result;
        const auto& bowl = *evidence->openBowl;
        const auto junction = cuts.value("junction").toObject();
        auto entry = source.res;
        if (!junction.isEmpty()) {
            // The rounded bowl turn and its short corner can occupy nearly
            // the same spot. Use the full contour inventory to refine the cut.
            SearchContext outline{.transformedPath = source.transformedPath, .res = entry,
                                  .subword = source.subword};
            if (readTerminalOutline(outline) && refineContourJunction(outline, junction) &&
                retainsOpenBowl(source, bowl, outline.res))
                entry = outline.res;
        }
        if (!matchesBoundaryBody(source, form, entry, false)) return result;
        QVector<PathExtrema> landmarks{bowl.tip, bowl.floor};
        if (bowl.crest) landmarks.append(*bowl.crest);
        result.push_back({.formId = id, .entry = entry, .landmarks = std::move(landmarks),
                          .entryLimit = entry.topBaseLine});
    } else if (shape == "bowl-turn") {
        const auto evidence = recognizedSharedShape(source, body);
        if (evidence->bowlTurn && declaredSharedBoundary(source, form) &&
            matchesBoundaryBody(source, form, source.res, false))
            result.push_back({.formId = id, .entry = source.res, .landmarks = {evidence->bowlTurn->tip}});
    } else if (shape == "counter-neck" || shape == "counter-shoulder") {
        const auto evidence = recognizedSharedShape(source, body);
        if (!evidence->boundaryCounter) return result;
        const auto& counter = *evidence->boundaryCounter;
        const bool preferred = declaredSharedBoundary(source, form);
        const auto preferredEntry = source.res;
        std::optional<Stretching> minimum;
        if (cuts.value("minimumEntryAtCounter").toBool()) {
            auto policy = cuts;
            auto counterCut = policy.value("counter").toObject();
            auto corner = counterCut.value("entryCorner").toObject();
            corner.insert("strokeOffsetX", 0);
            corner.insert("required", false);
            counterCut.insert("entryCorner", corner);
            counterCut.insert("normalCut", true);
            policy.insert("counter", counterCut);
            auto atCorner = form;
            atCorner.insert("cuts", policy);
            if (declaredSharedBoundary(source, atCorner)) minimum = source.res;
        }
        if (!preferred && !minimum) return result;
        const auto entry = preferred ? preferredEntry : *minimum;
        const double bridgeWidth = cuts.value("entryBridge").toObject().value("maxWidth").toDouble();
        if (!preferred || (minimum && !orderedSections(entry, *minimum, source.orientation))) minimum.reset();
        const auto rim = cuts.value("entryRim").toObject();
        const QLineF section(entry.topBaseLine.point, entry.bottomBaseLine.point);
        const bool fixedEntry = cuts.value("fixEntryJunction").toBool();
        const auto add = [&](const Stretching& boundary, bool retainsRim) {
            if (!matchesBoundaryBody(source, form, boundary, false)) return false;
            result.push_back({.formId = id, .entry = boundary, .landmarks = {*source.xSorted.rbegin()},
                              .counters = {counter.index}, .retainsCounterRim = retainsRim,
                              .sharesRimEntry = retainsRim && form.value("tightJoin").toBool(),
                              .fixedEntryJunction = fixedEntry,
                              .coversSharedEntry = bridgeWidth > 0 &&
                                  retainsCounterBridge(source, counter, boundary, bridgeWidth),
                              .tightEntryAfter = cuts.value("tightEntryAfter").toString(), .minimumEntry = minimum});
            return true;
        };
        if (needsCounterRim(section, rim, penWidth))
            for (const auto& boundary : counterRimEntryCuts(source, counter.box, entry))
                if (add(boundary, true) && fixedEntry && continuation) break;
        if (result.empty()) add(entry, false);
    }
    if (continuation && position == 0) {
        for (auto& candidate : result) {
            if (!candidate.exit || candidate.counters.isEmpty() || candidate.landmarks.size() != 1) continue;
            const auto side = getJoinTime(*candidate.exit, source.orientation);
            if (side.isStrictRight(candidate.landmarks.front().time)) continue;
            const auto& counter = source.subPaths[candidate.counters.front()].boundingRect();
            const auto crest = std::find_if(source.ySorted.rbegin(), source.ySorted.rend(), [&](const auto& p) {
                return p.point.x() >= counter.left() - penWidth && p.point.x() <= counter.right() + penWidth &&
                    p.point.y() < counter.top() && side.isStrictRight(p.time);
            });
            if (crest != source.ySorted.rend()) candidate.landmarks = {*crest};
        }
    }
    const auto attachment = cuts.value("sharedExitAtAttachment").toObject();
    if (!attachment.isEmpty() && (shape == "open-head" || shape == "suspended-loop" || shape == "descending-stem")) {
        const auto seams = upperAttachmentSections(source, form.value("_upperAttachment").toObject());
        const double reach = attachment.value("maxDistanceWidths").toDouble() * penWidth;
        std::vector<AnchoredBody> joined;
        for (const auto& candidate : result) {
            if (!candidate.exit || candidate.landmarks.isEmpty()) continue;
            for (auto seam : seams) {
                orientSection(source, seam, candidate.landmarks.front().time, false);
                const bool keepsFoot = shape == "descending-stem" &&
                    matchesBoundaryBody(source, form, seam, true) &&
                    (!candidate.exitLimit || getJoinTime(seam, source.orientation).isRight(candidate.exitLimit->time));
                if (QLineF(sectionLine(*candidate.exit).center(), sectionLine(seam).center()).length() > reach ||
                    (!keepsFoot && !orderedSections(*candidate.exit, seam, source.orientation)) ||
                    !retainsLandmarks(candidate, candidate.entry ? &*candidate.entry : nullptr, &seam, source.orientation))
                    continue;
                auto body = candidate;
                body.exit = seam;
                body.requiresSharedExit = true;
                joined.push_back(std::move(body));
            }
        }
        result.insert(result.begin(), joined.begin(), joined.end());
    }
    if (position == 0 && routeCut.value("bottomProbe") == "corner" && routeCut.value("anchorSide") == "bottom")
        for (auto& candidate : result)
            if (candidate.exit) candidate.exitLimit = candidate.exit->bottomBaseLine;
    if (form.value("alternateAnchor").toBool())
        for (auto& candidate : result) candidate.alternateAnchor = true;
    if (continuation && position == 0 &&
        (shape == "horizontal-counters" || shape == "descending-stem" || shape == "layered-head" ||
         cuts.value("shareTerminalReturn").toBool()))
        for (auto& candidate : result) candidate.sharedEntryBody = form;
    if (continuation) {
        const bool tightIncoming = position == 1 && shape == "counter-loop" && form.value("sharedEntry").toBool();
        for (auto& candidate : result) {
            candidate.keepsEntryConnector = position == 1 &&
                cuts.value("mouth").toObject().value("separateSharedEntry").toBool();
            candidate.keepInitialExitAfter = cuts.value("sharedEntryAfter").toString();
            if (position == 0 && shape == "descending-stem")
                candidate.requiresSharedExit = form.value("tightJoin").toBool();
            candidate.prefersTightIncoming = tightIncoming;
            candidate.needsSharedInwardCorner = position == 2 && shape == "rising-return" &&
                cuts.value("return").toObject().value("maxChord").isDouble();
        }
    }
    if (cuts.value("counterJoin") == "preserve-entry" &&
        cuts.value("boundaries").toObject().value("right") == "fork-normal")
        for (auto& candidate : result) candidate.keepsSharedCounterEntry = true;
    if (cuts.value("acceptCounterRim").toBool())
        for (auto& candidate : result) candidate.acceptsCounterRim = true;
    if (cuts.value("tightJoin").toString() == "shared-boundary" || form.value("tightJoin").toBool() ||
        (position == 2 && (shape == "counter-neck" || shape == "counter-shoulder" ||
                           shape == "upright-stem")))
        for (auto& candidate : result) candidate.acceptsCloseSharedJoin = true;
    const auto terminal = cuts.value("beforeFinal").toObject();
    if (terminal.value("boundaries").toObject().value("left") == "shared-return")
        for (auto& candidate : result) {
            candidate.sharedReturnWith = terminal.value("letters").toString();
            candidate.sharedReturnJunction = terminal.value("junction").toObject().value("template").toString();
        }
    if (position == 1 && (shape == "counter-loop" || shape == "counter-outline") && cuts.contains("minimumExitWalk")) {
        const double distance = cuts.value("minimumExitWalk").toDouble() * penWidth;
        const auto following = cuts.value("minimumExitBefore").toString();
        for (auto& candidate : result) {
            candidate.minimumExit = insetLimit(source, candidate, distance, false);
            candidate.minimumExitBefore = following;
        }
    }
    if (const auto sources = form.value("sharedFixedEntryAfter").toArray(); !sources.isEmpty()) {
        QStringList allowed;
        for (const auto& sender : sources) allowed.append(sender.toString());
        for (auto& candidate : result)
            if (candidate.fixedEntryJunction) candidate.sharedFixedEntryAfter = allowed;
    }
    if (const auto sources = form.value("connectorStrokeWidthAfter").toArray(); !sources.isEmpty()) {
        QStringList allowed;
        for (const auto& sender : sources) allowed.append(sender.toString());
        for (auto& candidate : result) candidate.connectorStrokeWidthAfter = allowed;
    }
    const auto rimNormal = cuts.value("rimNormal").toObject();
    if (!rimNormal.isEmpty()) {
        for (auto& candidate : result) {
            if (candidate.counters.size() != 1) continue;
            const auto& eye = source.subPaths[candidate.counters.front()];
            if (candidate.entry && !candidate.requiresSharedEntry && !candidate.retainsCounterRim) {
                const auto normal = counterShoulderNormal(source, eye, *candidate.entry, true,
                    rimNormal.value("entry").toDouble(), penWidth);
                if (normal && (!candidate.exit || orderedSections(*normal, *candidate.exit, source.orientation)) &&
                    retainsLandmarks(candidate, &*normal, candidate.exit ? &*candidate.exit : nullptr, source.orientation)) {
                    candidate.entry = normal;
                    candidate.fixedEntryJunction = true;
                    candidate.sharesRimEntry = true;
                }
            }
            if (candidate.exit && !candidate.requiresSharedExit) {
                const auto normal = counterShoulderNormal(source, eye, *candidate.exit, false,
                    rimNormal.value("exit").toDouble(), penWidth);
                if (normal && (!candidate.entry || orderedSections(*candidate.entry, *normal, source.orientation)) &&
                    retainsLandmarks(candidate, candidate.entry ? &*candidate.entry : nullptr, &*normal, source.orientation))
                    candidate.exit = normal;
            }
        }
    }
    if (inspectingForm) for (const auto& candidate : result) {
        for (const auto& mark : candidate.landmarks) inspectLandmark("Body landmark", mark);
        if (candidate.entry) inspectCandidate("Entry anchor", *candidate.entry);
        if (candidate.exit) inspectCandidate("Exit anchor", *candidate.exit);
    }
    return result;
}

std::vector<AnchoredBody> anchoredBodiesAtSharedEntry(SearchContext& source, const QJsonObject& form,
        std::span<const AnchoredBody* const> intrinsic, const Stretching& incoming, double penWidth, bool counter) {
    std::vector<AnchoredBody> result;
    const auto policy = form.value("cuts").toObject();
    if (form.contains("_headForms")) {
        const auto pieces = cutRegions(source.transformedPath, {}, {incoming}, source.orientation, true);
        if (pieces.size() != 2) return result;
        WordResultInfo::SubWordInfo remainder;
        OutlineSearchCache cache;
        SearchContext exposed{.transformedPath = pieces.back().path, .subword = remainder, .outlineCache = &cache};
        if (!readTerminalOutline(exposed)) return result;
        std::vector<std::vector<QBezier>> curves;
        for (const auto& path : source.subPaths) curves.push_back(baseContourCurves(path));
        const auto id = form.value("id").toString();
        for (const auto& headForm : form.value("_headForms").toArray()) {
            for (const auto& head : anchoredBodies(exposed, headForm.toObject(), 0, penWidth)) {
                if (!head.exit) continue;
                auto exit = *head.exit;
                if (!sourceLandmark(exit.topBaseLine, curves) || !sourceLandmark(exit.bottomBaseLine, curves) ||
                    !orderedSections(incoming, exit, source.orientation)) continue;
                QVector<PathExtrema> landmarks;
                const auto entrySide = getJoinTime(incoming, source.orientation);
                const auto exitSide = getJoinTime(exit, source.orientation);
                const auto owns = [&](const PathExtrema& point) {
                    return entrySide.isStrictLeft(point.time) && exitSide.isStrictRight(point.time);
                };
                for (auto point : head.landmarks)
                    if (sourceLandmark(point, curves) && owns(point)) landmarks.append(point);
                if (landmarks.isEmpty()) {
                    // An exposed route can start at the entry seam. Use the
                    // first contour landmark inside the recognized head.
                    for (const auto& point : source.timeSorted)
                        if (owns(point)) {
                            landmarks.append(point);
                            break;
                        }
                }
                if (landmarks.isEmpty() || !head.counters.isEmpty()) continue;
                result.push_back({.formId = id, .entry = incoming, .exit = exit, .landmarks = std::move(landmarks),
                                  .requiresSharedEntry = true});
            }
        }
        return result;
    }
    if (counter && form.value("body").toObject().value("shape") == "forked-stem") {
        const auto shape = form.value("body").toObject();
        const auto evidence = recognizedSharedShape(source, shape);
        const auto routes = shape.value("exitRoutes").toArray(), routeCuts = policy.value("exitRoutes").toArray();
        if (evidence->medialStem) {
            const auto& section = incoming;
            const auto entry = getJoinTime(section, source.orientation);
            const double width = section.referenceWidth > 0 ? section.referenceWidth : penWidth;
            const auto limit = sectionLine(section).center();
            for (qsizetype i = 0; i < routes.size(); ++i)
                for (const auto& stem : evidence->medialStem->stems) {
                    if (!entry.isStrictLeft(stem.crest.time) || stem.crest.point.x() >= limit.x() ||
                        limit.y() - stem.crest.point.y() < shape.value("minRise").toDouble() * width)
                        continue;
                    for (const auto& route : stem.exitRoutes[i]) {
                        source.res = {};
                        if (!projectSavedRoute(source, route, routes[i].toObject(), routeCuts[i].toObject()) ||
                            !orderedSections(section, source.res, source.orientation)) continue;
                        const auto pieces = cutRegions(source.transformedPath, {}, {section, source.res},
                                                       source.orientation, false);
                        if (pieces.size() != 3) continue;
                        const auto& body = pieces[1].path;
                        const auto bounds = body.boundingRect();
                        if (std::abs(bounds.top() - stem.crest.point.y()) > .01 ||
                            bounds.width() > shape.value("maxWidthToHeight").toDouble() * bounds.height() ||
                            toSubpaths(body).size() - 1 != shape.value("counters").toInt()) continue;
                        result.push_back({.formId = form.value("id").toString(),
                            .entry = section, .exit = source.res, .landmarks = {stem.crest},
                            .requiresSharedEntry = true, .keepsSharedCounterEntry = true,
                            .preservesTerminalBody = true});
                    }
                }
        }
    }
    for (const auto* original : intrinsic) {
        const auto& body = *original;
        if (!body.exit || (counter && body.landmarks.size() != 1) ||
            !orderedSections(incoming, *body.exit, source.orientation) ||
            (!counter && QLineF(sectionLine(incoming).center(), sectionLine(*body.entry).center()).length() > 2 * penWidth) ||
            !retainsLandmarks(body, &incoming, &*body.exit, source.orientation)) continue;
        // The adjacent bridge and both sections must retain the recognized shaft.
        const auto pieces = cutRegions(source.transformedPath, {}, {incoming, *body.exit},
                                       source.orientation, false);
        if (pieces.size() != 3) continue;
        const auto& mask = pieces[1].path;
        if (counter) {
            if (std::abs(mask.boundingRect().top() - body.landmarks.front().point.y()) > .01) continue;
        } else if (form.value("body").toObject().value("shape") == "counter-outline") {
            const auto entry = sectionLine(incoming);
            const auto rim = sectionLine(*body.entry);
            if (QLineF(entry.center(), rim.center()).length() > .5 * penWidth ||
                !matchesCounterOutline(source, mask, form, entry, sectionLine(*body.exit))) continue;
        } else if (!containsHeadLips(mask, {{"minRun", .5 * penWidth}, {"minGap", .15 * penWidth}})) continue;
        auto candidate = body;
        candidate.entry = incoming;
        candidate.requiresSharedEntry = true;
        candidate.keepsSharedCounterEntry = counter;
        result.push_back(std::move(candidate));
    }
    return result;
}

} // namespace MedinaCutter
