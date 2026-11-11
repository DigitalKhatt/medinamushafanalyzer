#include "../detail.h"

using namespace MedinaCutter;

bool PageAnalysisResult::segmentSubwordPass(int pageIndex, int lineIndex, int wordIndex, int subWordIndex,
                                            AFont* font, bool refresh, const QJsonObject* draft, bool recoverTeeth,
                                            OutlineSearchCache& outlineCache, ToothSearchPlan* plan, const SeenExitRetry* seenRetry) {
    const auto seenRetryFor = [&](int slot, const QJsonObject& settings) {
        return seenRetry && seenRetry->slot == slot && seenRetry->supports(settings) ? seenRetry : nullptr;
    };
	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];
    const bool enumerate = plan && plan->enumerate;
	auto& subWord = word.wordResultInfo.subWords[subWordIndex];

	if (subWord.baseGlyphs.size() > 0 && !refresh) {
		return subWord.correct;
	}

	subWord.baseGlyphs.clear();
	subWord.kashidaPaths.clear();
	subWord.kashidaCuts.clear();

    subWord.joins.clear();
    subWord.extrema.clear();
    subWord.templateTrace.clear();
    subWord.matchedTemplates.clear();
    subWord.letterDiagnostics = {};
    if (subWord.paths.empty() || subWord.paths.front() < 0 || subWord.paths.front() >= word.paths.size() ||
        word.paths[subWord.paths.front()].path.isEmpty()) {
        subWord.correct = false;
        return false;
    }
	const auto baseText = bodyLetters(subWord.text);
    const auto text = QStringView(subWord.text);
    bool hasFinalTatweel = false, pastFinal = false;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        const auto ch = *it;
        if (ch.isMark()) continue;
        if (pastFinal) {
            hasFinalTatweel = ch == QChar(0x0640);
            break;
        }
        if (ch != QChar(0x0640)) pastFinal = true;
    }

	const auto shape = word.bodyShape(subWord);

	if (baseText.size() < 2) {
		subWord.correct = !baseText.isEmpty();
		GlyphInfo glyphInfo{ baseText ,shape.path };
		subWord.baseGlyphs.push_back(glyphInfo);
		if (!baseText.isEmpty()) subWord.letterDiagnostics.append(QJsonObject{
            {"letterIndex", 1}, {"character", baseText}, {"position", "isolated"},
            {"status", "not-required"}, {"template", ""}, {"candidates", QJsonArray{}}});
		if (baseText.isEmpty()) subWord.templateTrace.append("No body letter in transcript · needs review");
		return subWord.correct;
	}


	const auto& config = draft ? *draft : letterForms();
	SearchContext context{ .joinDefaults = config.value("joinDefaults").toObject(),
	                       .subword = subWord, .baseText = baseText, .outlineCache = &outlineCache };

	auto& subPaths = context.subPaths;

	auto pos = shape.pos;
	context.transformedPath = shape.path * shape.transform;
	context.transformedPath.translate(pos.x(), pos.y());
	context.transformedPath.setFillRule(Qt::WindingFill);

	context.bbox = context.transformedPath.boundingRect();

	if (!preparePassOutline(context, outlineCache)) { subWord.correct = false; return false; }
	subWord.basePathOrientation = context.orientation;

    if (formInspection) {
        for (const auto *phase : {"initial", "medial", "final"}) {
            const auto text = QString(phase) == "initial" ? baseText.left(1)
                              : QString(phase) == "final" ? baseText.right(1)
                                                          : baseText.mid(1, baseText.size() - 2);
            for (const auto &value : config.value(phase).toArray()) {
                const auto form = resolvedBodyForm(config, value.toObject());
                if (!sharedShapeDescription(form.value("body").toObject()) ||
                    std::none_of(text.begin(), text.end(),
                                 [&](QChar c) { return form.value("current").toString().contains(c); }))
                    continue;
                const auto evidence = recognizedSharedShape(context, form.value("body").toObject());
                InspectFormScope inspection(form, context.transformedPath);
                inspectSharedShape(*evidence);
            }
        }
    }

    // Discover medial lām bodies before even the initial boundary is tried.
    // A failed preceding letter must not hide their evidence. Transcript
    // selectors limit which descriptions need evaluating, never their shape
    // search; the fixed-outline cache also serves every subsequent retry.
    if (baseText.size() > 2) {
        const auto medialText = QStringView(baseText).mid(1, baseText.size() - 2);
        for (const auto& value : config.value("medial").toArray()) {
            const auto form = value.toObject();
            const auto current = form.value("current").toString();
            if (std::none_of(medialText.begin(), medialText.end(), [&](QChar ch) { return current.contains(ch); }))
                continue;
            const auto body = formDescription(config, form, "body");
            if (body.value("shape") != "forked-stem" && body.value("shape") != "turning-shaft") continue;
            const auto evidence = recognizedMedialLamBodies(context, body);
            InspectFormScope inspection(form, context.transformedPath);
            inspectLamBodyEvidence(*evidence);
        }
    }

	context.nextIter = context.xSorted.size() != 0 ? std::next(context.xSorted.begin()) : context.xSorted.end();

	int lastIndex = baseText.length() - 1;

    const auto& preparedRules = preparePassRules(config, recoverTeeth, outlineCache);
    const auto& boundaryRules = preparedRules.boundaries;
    const auto& finalRules = preparedRules.finals;
    const auto& alefTurnRules = preparedRules.alefTurns;
    QVector<QString> bodyTemplateIds(baseText.size());
    QVector<int> sharedEntries;
    QMap<int, ToothBody> ownedTeeth;
    QMap<int, PathExtrema> ownedLamCrests;
    QMap<int, PathExtrema> matchedLamForks;
    QMap<int, PathExtrema> ownedOpenHeads;
    QVector<bool> bodyAttempted(baseText.size(), false);
    const auto applies = [&](const QJsonObject& rule, int index) {
        const auto current = rule.value("current");
        const auto next = rule.value("next");
        return (!current.isString() || current.toString().contains(baseText[index])) &&
               (!next.isString() || next.toString().contains(baseText[index + 1])) &&
               (!rule.contains("ligatureWith") || hasFollowingLetter(baseText, index, rule.value("ligatureWith").toString())) &&
               (!rule.contains("first") || rule.value("first").toBool() == (index == 0)) &&
               (!rule.contains("last") || rule.value("last").toBool() == (index + 1 == lastIndex));
    };
    QString retryRule;
    QMap<QString, Stretching> intrinsicFinalCuts;
    QMap<QString, Stretching> visibleFinalCuts;
    const auto attempt = [&](const QJsonObject& rule, int index, bool record = true, const Stretching* finalFold = nullptr) {
        InspectFormScope inspection(rule, context.transformedPath);
        const auto savedIter = context.nextIter;
        context.form = rule;
        context.res = {};
        const int owner = rule.value("bodyBoundary") == "current" ? 0 : lastIndex;
        bodyAttempted[owner] = true;
        bool found = proposeTemplateBoundary(context, config, rule, owner == lastIndex, recoverTeeth,
                                              seenRetryFor(owner, rule), finalFold,
                                              plan ? &plan->routeSearches[owner] : nullptr);
        QString rejectionReason;
        const bool bodyRejected = found && !matchesBoundaryBody(context, rule, context.res, owner == 0,
            config.value("_initialHeadLips").toObject(), &rejectionReason);
        if (bodyRejected) {
            inspectReason("Body", rejectionReason.isEmpty() ? "The candidate violates the configured body proportions." : rejectionReason);
            found = false;
        }
        if (found && rule.contains("bodyCounters")) {
            const auto bodies = cutRegions(context.transformedPath, {}, {context.res}, context.orientation, true);
            const int body = rule.value("bodyBoundary") == "current" ? 0 : 1;
            found = bodies.size() == 2 && static_cast<int>(toSubpaths(bodies[body].path).size()) - 1 == rule.value("bodyCounters").toInt();
        }
        bool accepted = found && matchesJoinFeatures(context);
        if (record && accepted && enumerate && owner == 0)
            accepted = plan->accepts(owner, context.res, context.res);
        if (accepted && owner == lastIndex && rule.value("retainBodyFeatures").toBool() &&
            !intrinsicFinalCuts.contains(rule.value("id").toString()))
            intrinsicFinalCuts.insert(rule.value("id").toString(), context.res);
        if (accepted && owner == lastIndex && rule.contains("visibleFrom") &&
            !visibleFinalCuts.contains(rule.value("id").toString()))
            visibleFinalCuts.insert(rule.value("id").toString(), context.res);
        if (!record) {
            context.nextIter = savedIter;
            return accepted;
        }
        const auto id = rule.value("id").toString();
        subWord.templateTrace.append(QString("%1 → %2 · %3 · %4")
            .arg(baseText.mid(index, 1), baseText.mid(index + 1, 1), id,
                 accepted ? QStringLiteral("matched") : bodyRejected ?
                    (rejectionReason.isEmpty() ? QStringLiteral("body proportions rejected") : rejectionReason) : QStringLiteral("no cut")));
        if (accepted) {
            subWord.matchedTemplates.append(id);
            bodyTemplateIds[owner] = id;
            if (retryRule.isEmpty() && rule.value("retryOnPartial").toBool()) retryRule = id;
            return true;
        }
        context.nextIter = savedIter;
        return false;
    };
    const auto preservesBody = [&](const QString& templateId, const Stretching& boundary, bool initial) {
        for (const auto& value : config.value(initial ? "initial" : "final").toArray()) {
            const auto rule = preparedRules.constraints.value(value.toObject().value("id").toString());
            if (rule.value("id") != templateId) continue;
            if (!matchesBoundaryBody(context, rule, boundary, initial, config.value("_initialHeadLips").toObject()))
                return false;
            if (!initial && intrinsicFinalCuts.contains(templateId)) {
                const auto& intrinsic = intrinsicFinalCuts[templateId];
                const auto expected = getJoinTime(intrinsic, context.orientation);
                const auto actual = getJoinTime(boundary, context.orientation);
                const QLineF seam(intrinsic.topBaseLine.point, intrinsic.bottomBaseLine.point);
                const auto vector = seam.p2() - seam.p1();
                for (const auto& point : context.timeSorted) {
                    if (!expected.isStrictLeft(point.time) || actual.isLeft(point.time)) continue;
                    const double t = std::clamp(QPointF::dotProduct(point.point - seam.p1(), vector) /
                        std::max(1e-12, QPointF::dotProduct(vector, vector)), 0., 1.);
                    if (QLineF(point.point, seam.pointAt(t)).length() > seam.length()) return false;
                }
            }
            return true;
        }
        return true;
    };
    const auto sharedBoundary = [&](const QJsonObject &rule) {
        return (rule.value("sharedBoundary").toBool() &&
                (!rule.contains("sharedBoundaryWith") ||
                 rule.value("sharedBoundaryWith").toString().contains(baseText[lastIndex - 1])));
    };
    const auto sharesCornerJunction = [&](const QString& id, const Stretching& exit, const Stretching& entry) {
        const auto form = preparedRules.constraints.value(id);
        if (!form.value("tightJoin").toBool() || !entry.topBaseLine.isSharp || !entry.bottomBaseLine.isSharp ||
            form.value("cuts").toObject()
                .value("boundaries").toObject().value("right") != "corner-junction" ||
            cutPrecedes(exit, entry, context.orientation)) return false;
        const double topGap = QLineF(exit.topBaseLine.point, entry.topBaseLine.point).length();
        const double bottomGap = QLineF(exit.bottomBaseLine.point, entry.bottomBaseLine.point).length();
        const double width = QLineF(exit.topBaseLine.point, exit.bottomBaseLine.point).length();
        const double tolerance = config.value("kashidaDefaults").toObject()
            .value("sharedCutTolerance").toDouble(.25) * width;
        // One flank locates the same junction. The other can lie farther along
        // the bowl when the preceding body uses a projected exit.
        return std::min(topGap, bottomGap) <= tolerance && std::max(topGap, bottomGap) <= width;
    };
    struct BodyPair {
        Stretching primary;
        Stretching other;
        QString templateId;
        bool primaryIsNext;
        bool tightJoin;
        bool ligature;
        bool returning;
    };
    const auto medialForms = config.value("medial").toArray();
    const auto matchesBody = [&](const QJsonObject& form, int letter) {
        const auto final = form.value("sharedFoldWithFinal").toString();
        return form.value("current").toString().contains(baseText[letter]) &&
            (final.isEmpty() || hasFollowingLetter(baseText, letter, final, true));
    };
    const auto canProposeMedialBoundary = [&](int letter) {
        return letter > 0 && letter < lastIndex &&
            std::any_of(medialForms.begin(), medialForms.end(), [&](const auto& value) {
                const auto form = value.toObject();
                return supportsOpenMedialBoundary(config, form) && matchesBody(form, letter);
            });
    };
    bool deferredFinalJoin = false;
    bool resolvedFinalJoin = false;
    const auto canDefer = [&](int index) {
        if (index == 0 || index + 1 != lastIndex ||
            subWord.joins.size() != static_cast<size_t>(index)) return false;
        const auto medial = config.value("medial").toArray();
        const bool hasTooth = std::any_of(medial.begin(), medial.end(), [&](const auto& value) {
            const auto form = value.toObject();
            return hasToothBody(form) &&
                form.value("current").toString().contains(baseText[index]);
        });
        return hasTooth && (std::any_of(finalRules.begin(), finalRules.end(), [&](const auto& value) {
            const auto rule = value.toObject();
            return rule.value("bodyBoundary") == "next" && rule.value("pairedOnly").toBool() &&
                applies(rule, index);
        }) || std::any_of(alefTurnRules.begin(), alefTurnRules.end(), [&](const auto& value) {
            return applies(value.toObject(), index);
        }));
    };
    const bool preserveReadingOrder = config.value("_checkRecoveredCuts").toBool();
    const auto storeJoins = [&](std::vector<Stretching>& cuts) {
        if (preserveReadingOrder)
            for (int i = 0; i < static_cast<int>(cuts.size()); ++i)
                cuts[i].readingOrder = i;
        subWord.joins = Joins(cuts.begin(), cuts.end());
    };
    std::vector<BodyPair> bodyPairs;
    // Preserve explicit boundaries of following letters before the medial pass
    // replaces a join's primary anchor with the medial body's outgoing cut.
    struct BodyBoundary {
        Stretching cut;
        QString templateId;
        bool ligature = false;
    };
    std::vector<std::optional<BodyBoundary>> nextBoundaries(lastIndex);
    const auto tryInitialFold = [&](const QJsonObject& initial) {
        const auto foldForm = initial.value("sharedFoldForm").toString();
        for (const auto& item : finalRules) {
            const auto final = item.toObject();
            if (!foldForm.isEmpty() && final.value("id") != foldForm) continue;
            if (final.value("pairedOnly").toBool() || !applies(final, 0)) continue;
            if (!attempt(final, 0, false)) {
                if (!final.value("tryNext").toBool()) return false;
                continue;
            }
            auto seam = context.res;
            if (!attempt(initial, 0, false, &seam)) return false;
            bodyTemplateIds[0] = initial.value("id").toString();
            bodyTemplateIds[1] = final.value("id").toString();
            retryRule = bodyTemplateIds[0];
            subWord.matchedTemplates << bodyTemplateIds[0] << bodyTemplateIds[1];
            subWord.templateTrace << bodyTemplateIds[0] + " · shared cut at the final fold";
            sharedEntries.append(0);
            seam.kashidaOverrides = final.value("kashida").toObject();
            seam.kashidaOverrides.insert("enabled", false);
            seam.length = 0;
            if (preserveReadingOrder) seam.readingOrder = 0;
            subWord.joins.insert(seam);
            return true;
        }
        return false;
    };
    for (int index = 0; index < lastIndex; ++index) {
        bool found = false;
        bool matchedFold = false;
        for (const auto& value : boundaryRules) {
            const auto rule = value.toObject();
            if (rule.value("pairedOnly").toBool() || !applies(rule, index)) continue;
            if (rule.contains("sharedFoldWithFinal")) {
                if (lastIndex != 1 || hasFinalTatweel || !matchesBody(rule, index)) continue;
                found = matchedFold = tryInitialFold(rule);
            } else if (rule.value("body").toObject().value("shape") == "upright-stem" && canDefer(index)) {
                deferredFinalJoin = true;
                break;
            } else {
                found = attempt(rule, index);
            }
            if (found || (!enumerate && !rule.value("tryNext").toBool())) break;
        }
        if (matchedFold) continue;
        if (!found) {
            if (canDefer(index)) deferredFinalJoin = true;
            break;
        }
        context.res.kashidaOverrides = context.form.value("kashida").toObject();
        if (context.res.counterEntry.pathIndex > 0) {
            context.res.kashidaOverrides.insert("enabled", false);
            if (index + 1 == lastIndex) bodyTemplateIds[lastIndex] = context.form.value("id").toString();
            subWord.templateTrace.append(context.form.value("id").toString() + " · shared ligature boundary across two ink bridges");
            if (preserveReadingOrder) context.res.readingOrder=index;
            subWord.joins.insert(context.res);
            continue;
        }
        const auto primary = context.res;
        const auto primaryForm = context.form;
        const auto side = primaryForm.value("bodyBoundary").toString();
        if (side == "next") nextBoundaries[index] = BodyBoundary{primary, primaryForm.value("id").toString(),
            sharedBoundary(primaryForm)};
        if (!side.isEmpty()) {
            const auto otherSide = side == "next" ? QStringLiteral("current") : QStringLiteral("next");
            context.pairedStrokeWidth = primaryForm.value("trimExitToNormal").toBool() && primary.referenceWidth > 0
                ? primary.referenceWidth : QLineF(primary.topBaseLine.point, primary.bottomBaseLine.point).length();
            const auto& pairingRules = boundaryRules;
            for (const auto& value : pairingRules) {
                const auto rule = value.toObject();
                if (rule.value("bodyBoundary").toString() != otherSide || rule.contains("sharedFoldWithFinal") ||
                    !applies(rule, index)) continue;
                if (attempt(rule, index, false)) {
                    bodyPairs.push_back({primary, context.res, rule.value("id").toString(), side == "next",
                                         rule.value("tightJoin").toBool() || primaryForm.value("tightJoin").toBool(),
                                         sharedBoundary(rule) || sharedBoundary(primaryForm),primaryForm.value("returningStroke").toBool() || rule.value("returningStroke").toBool()});
                }
            }
            context.pairedStrokeWidth = 0;
            context.res = primary;
            context.form = primaryForm;
        }
        if (preserveReadingOrder) context.res.readingOrder=index;
            subWord.joins.insert(context.res);
    }

    const double minInitialConnector = preparedRules.constraints.value(bodyTemplateIds[0])
        .value("cuts").toObject().value("minConnectorWidths").toDouble();
    const auto retainsInitialConnector = [&](const Stretching& exit, const Stretching& entry) {
        if (minInitialConnector <= 0 || !cutPrecedes(exit, entry, context.orientation)) return false;
        const QLineF sending(exit.topBaseLine.point, exit.bottomBaseLine.point);
        const QLineF receiving(entry.topBaseLine.point, entry.bottomBaseLine.point);
        const auto advance = sending.center() - receiving.center();
        const double width = std::max(sending.length(), receiving.length());
        // Both sections must cross a visible, low connecting stroke. A
        // near-horizontal cap belongs to a descending stacked attachment.
        if (advance.x() < minInitialConnector * width || std::abs(advance.y()) > .6 * advance.x() ||
            std::abs(sending.dy()) < .7 * sending.length() ||
            std::abs(receiving.dy()) < .7 * receiving.length()) return false;
        const auto pieces = cutRegions(context.transformedPath, {}, {exit, entry}, context.orientation, true);
        return pieces.size() == 3 && toSubpaths(pieces[1].path).size() == 1 &&
               followsOneStroke(pieces[1].path, sending, receiving);
    };

    // A join can carry one cut from each neighboring letter body.
    // Keep the original single-rule result when the pair cannot form a valid span.
    if (!bodyPairs.empty() && subWord.joins.size() == static_cast<size_t>(lastIndex)) {
        auto cuts = std::vector<Stretching>(subWord.joins.begin(), subWord.joins.end());
        const Stretching::cmpbyx before;
        const auto chord = [](const Stretching& cut) {
            return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
        };
        const auto& ink = context.transformedPath;
        const auto& localPath = shape.path;
        for (const auto& pair : bodyPairs) {
            if (!bodyTemplateIds[pair.primaryIsNext ? 0 : lastIndex].isEmpty()) continue;
            const auto original = std::find_if(cuts.begin(), cuts.end(), [&](const Stretching& cut) {
                return !before(cut, pair.primary) && !before(pair.primary, cut);
            });
            if (original == cuts.end()) continue;
            const auto index = std::distance(cuts.begin(), original);
            const auto& right = pair.primaryIsNext ? pair.other : pair.primary;
            const auto& left = pair.primaryIsNext ? pair.primary : pair.other;
            const double gap = chord(right).center().x() - chord(left).center().x();
            const auto incomingRegion = getJoinTime(right, context.orientation);
            const bool ordered = incomingRegion.isStrictLeft(left.topBaseLine.time) &&
                incomingRegion.isStrictLeft(left.bottomBaseLine.time);
            const bool closeInterleaved = !ordered && QLineF(chord(right).center(), chord(left).center()).length() <=
                std::min(chord(right).length(), chord(left).length());
            const bool atJunction = index + 1 == cuts.size() &&
                sharesCornerJunction(pair.primaryIsNext ? bodyTemplateIds[lastIndex] : pair.templateId, right, left);
            if ((pair.ligature || atJunction || (pair.tightJoin &&
                ((gap <= 0 && -gap <= chord(right).length()) || closeInterleaved))) &&
                (index != 0 || !retainsInitialConnector(right, left)) &&
                (index == 0 || before(cuts[index - 1], left)) &&
                (index + 1 == cuts.size() || before(left, cuts[index + 1])) &&
                interiorChord(ink, left.topBaseLine, left.bottomBaseLine)) {
                auto trial = cuts;
                trial[index] = left;
                const auto pieces = cutRegions(localPath, subWord.text, trial, subWord.basePathOrientation, true);
                if ((index != 0 || preservesBody(bodyTemplateIds[0], left, true)) &&
                    (index + 1 != cuts.size() || preservesBody(bodyTemplateIds[lastIndex], left, false)) &&
                    pieces.size() == trial.size() + 1 &&
                    std::none_of(pieces.begin(), pieces.end(), [](const GlyphInfo& piece) {
                        return piece.path.boundingRect().isEmpty();
                    })) {
                    original->topBaseLine = original->topLeft = original->topRight = left.topBaseLine;
                    original->bottomBaseLine = original->bottomLeft = original->bottomRight = left.bottomBaseLine;
                    original->fixedLeftBoundary = false;
                    original->length = 0;
                    original->kashidaOverrides.insert("enabled", false);
                    subWord.matchedTemplates.append(pair.templateId);
                    bodyTemplateIds[pair.primaryIsNext ? 0 : lastIndex] = pair.templateId;
                    subWord.templateTrace.append(pair.templateId + (pair.ligature
                            ? " · shared ligature cut" : atJunction
                            ? " · shared cut at contour corners" : " · shared cut for tight join"));
                    continue;
                }
            }
            const auto settings = withOverrides(config.value("kashidaDefaults").toObject(), original->kashidaOverrides);
            if (!settings.value("enabled").toBool(true) || (pair.returning ? !ordered : !before(right,left)) ||
                ((settings.value("minSpan").toDouble()>0 || !pair.returning) &&
                 chord(right).center().x() - chord(left).center().x() < settings.value("minSpan").toDouble()) ||
                (index > 0 && !before(cuts[index - 1], right)) ||
                (index + 1 < cuts.size() && !before(left, cuts[index + 1])) ||
                !interiorChord(ink, right.topBaseLine, right.bottomBaseLine, right.topEndpointTolerance) ||
                !interiorChord(ink, left.topBaseLine, left.bottomBaseLine, left.topEndpointTolerance)) continue;

            auto trial = cuts;
            trial[index] = right;
            trial.insert(trial.begin() + index + 1, left);
            bool crossed = false;
            for (size_t i = 0; i < trial.size(); ++i) {
                if (i != index && chord(right).intersects(chord(trial[i]), nullptr) == QLineF::BoundedIntersection)
                    crossed = true;
                if (i != index + 1 && chord(left).intersects(chord(trial[i]), nullptr) == QLineF::BoundedIntersection)
                    crossed = true;
            }
            if (crossed) continue;
            const auto pieces = cutRegions(localPath, subWord.text, trial, subWord.basePathOrientation, true);
            if (pieces.size() != trial.size() + 1 ||
                std::any_of(pieces.begin(), pieces.end(), [](const GlyphInfo& piece) {
                    return piece.path.boundingRect().isEmpty();
                })) continue;

            original->topBaseLine = right.topBaseLine;
            original->bottomBaseLine = right.bottomBaseLine;
            original->topLeft = left.topBaseLine;
            original->bottomLeft = left.bottomBaseLine;
            original->fixedRightBoundary = true;
            original->fixedLeftBoundary = true;
            subWord.matchedTemplates.append(pair.templateId);
            bodyTemplateIds[pair.primaryIsNext ? 0 : lastIndex] = pair.templateId;
            subWord.templateTrace.append(pair.templateId + " · paired body boundary matched");
        }
        storeJoins(cuts);
    }

    const auto missingJoin = static_cast<int>(subWord.joins.size());
    const bool openMedialJoin = !deferredFinalJoin && canProposeMedialBoundary(missingJoin);
    bool pendingMedialJoin = openMedialJoin;
    // A medial body contributes one boundary to each adjacent join.
    if (openMedialJoin || deferredFinalJoin || subWord.joins.size() == static_cast<size_t>(lastIndex)) {
        auto cuts = std::vector<Stretching>(subWord.joins.begin(), subWord.joins.end());
        // Reserve the final slot without proposing any join geometry.
        if (openMedialJoin || deferredFinalJoin) cuts.emplace_back();
        const auto before = [&](const Stretching& a, const Stretching& b) {
            return cutPrecedes(a, b, context.orientation);
        };
        const auto center = [](const Stretching& join) {
            return QLineF(join.topBaseLine.point, join.bottomBaseLine.point).center();
        };
        QMap<QString, std::vector<ToothBody>> localTeeth;
        auto& teethByForm = plan ? plan->teeth : localTeeth;
        struct MedialAttempt {
            QJsonObject form;
            const ToothBody* tooth = nullptr;
            bool requireFinal = false;
        };
        const auto medialAttempts = [&](int letter, bool open) {
            std::vector<MedialAttempt> attempts, teeth;
            std::optional<size_t> toothPosition;
            for (const auto& value : medialForms) {
                const auto form = value.toObject();
                if (!matchesBody(form, letter) || (open && !supportsOpenMedialBoundary(config, form))) continue;
                const auto toothSettings = toothRecognitionSettings(form);
                const int stage = form.value("recoveryStage").toInt();
                if ((!recoverTeeth && stage > 0) || (plan && stage > plan->stage)) continue;
                bodyAttempted[letter] = true;
                if (!hasToothBody(form)) {
                    attempts.push_back({form});
                    continue;
                }
                if (!toothPosition) toothPosition = attempts.size();
                const auto id = form.value("id").toString();
                if (!teethByForm.contains(id)) {
                    auto bodies = declaredToothBoundaries(context, form);
                    if (recoverTeeth && toothPairDescription(toothSettings) && toothSettings.value("baselineEntry").isObject()) {
                        const auto entry = toothSettings.value("baselineEntry").toObject();
                        const auto policy = toothPlacementSettings(form).value("baselineEntry").toObject();
                        for (auto& body : bodies) extendToothToBaseline(context, body, entry, policy);
                    }
                    teethByForm.insert(id, std::move(bodies));
                }
                for (const auto& candidate : teethByForm.constFind(id).value()) {
                    const bool used = std::any_of(ownedTeeth.cbegin(), ownedTeeth.cend(), [&](const ToothBody& owned) {
                        return std::abs(owned.tip.time - candidate.tip.time) < 1e-7;
                    });
                    if (!used) teeth.push_back({form, &candidate});
                }
            }
            // A short tooth may precede a tall tooth. Template priority only
            // breaks ties at one tip; successive tips follow reading order.
            std::stable_sort(teeth.begin(), teeth.end(), [](const auto& a, const auto& b) {
                return a.tooth->tip.point.x() > b.tooth->tip.point.x();
            });
            if (toothPosition) attempts.insert(attempts.begin() + *toothPosition, teeth.begin(), teeth.end());
            return attempts;
        };
        // During medial placement the tentative cuts live in 'cuts'. The
        // committed joins seen by attempt() stay fixed until storeJoins().
        // Reuse the independent final probe when another candidate asks the
        // same question at the same cursor and local stroke width.
        struct FinalProbe {
            bool matched;
            Stretching cut;
            QJsonObject form;
        };
        using ProbeKey = std::tuple<QString, xsortedtype::difference_type, double>;
        std::map<ProbeKey, FinalProbe> finalProbes;
        const auto probeFinal = [&](const QJsonObject& rule) {
            const auto cursor = std::distance(context.xSorted.begin(), context.nextIter);
            const ProbeKey key{rule.value("id").toString(), cursor, context.pairedStrokeWidth};
            auto hit = finalProbes.find(key);
            if (hit == finalProbes.end()) {
                const bool matched = attempt(rule, lastIndex - 1, false);
                finalProbes.emplace(key, FinalProbe{matched, context.res, context.form});
                return matched;
            }
            context.res = hit->second.cut;
            context.form = hit->second.form;
            return hit->second.matched;
        };
        for (int letter = 1; letter < static_cast<int>(cuts.size()); ++letter) {
            const bool open = pendingMedialJoin && letter + 1 == static_cast<int>(cuts.size());
            auto& incoming = cuts[letter - 1];
            auto& outgoing = cuts[letter];
            const bool deferred = deferredFinalJoin && letter + 1 == lastIndex;
            if (deferred && std::any_of(alefTurnRules.begin(), alefTurnRules.end(), [&](const auto& value) {
                    return applies(value.toObject(), letter);
                })) {
                recoverIncomingToothShoulder(context, incoming);
            }
            const auto entry = center(incoming);
            const auto ordinaryExit = center(outgoing);
            const double stroke = incoming.referenceWidth > 0 ? incoming.referenceWidth :
                QLineF(incoming.topBaseLine.point, incoming.bottomBaseLine.point).length();
            if (stroke <= 0 || (!open && !deferred && entry.x() <= ordinaryExit.x())) continue;

            auto attempts = medialAttempts(letter, open);
            // Try forms that preserve an independent final boundary before
            // accepting a partial reading at the end of a tooth sequence.
            if (!ownedTeeth.isEmpty() && letter + 1 == lastIndex &&
                bodyTemplateIds[lastIndex].isEmpty() && attempts.size() > 1) {
                const auto partialAttempts = attempts;
                for (auto& attempt : attempts) attempt.requireFinal = true;
                attempts.insert(attempts.end(), partialAttempts.begin(), partialAttempts.end());
            }
            for (const auto& medial : attempts) {
                const auto form = preparedRules.constraints.value(medial.form.value("id").toString());
                const bool composeSeenTail = plan && seenRetry && seenRetry->slot == letter;
                if (composeSeenTail && !seenRetry->supports(form.value("seenTeeth").toObject())) continue;
                const auto contour = pairedRouteSettings(form);
                const auto openHead = openHeadSettings(form);
                const auto tooth = toothRecognitionSettings(form);
                const auto bridge = form.value("counterLigature").toObject();
                const bool internalBody = hasIntrinsicMedialBody(config, form);
                auto proposals = proposeMedialBodies(context, form, plan, baseText[letter + 1],
                    letter > 1 ? &cuts[letter - 2] : nullptr,
                    open ? context.bbox.left() : ordinaryExit.x(), entry.x());
                if (!proposals) continue;
                auto candidates = std::move(*proposals);
                auto exit = ordinaryExit;
                std::optional<Stretching> nextBody;
                QString nextBodyId;
                if (!tooth.isEmpty() && letter + 1 == lastIndex) {
                    context.pairedStrokeWidth = stroke;
                    for (const auto& ruleValue : finalRules) {
                        const auto rule = ruleValue.toObject();
                        // An independent final stroke walk already specifies its
                        // cut. Keep the tooth candidates on its incoming side.
                        const bool suppliesExit =
                            rule.value("pairedOnly").toBool() ||
                            rule.value("body").toObject().value("shape") == "upright-stem" ||
                            rule.contains("penWidth") ||
                            formDescription(config, rule, "body").value("shape") == "terminal-fork" ||
                            rule.value("strokeSource") == "local";
                        if (!suppliesExit || !applies(rule, letter) || !probeFinal(rule)) continue;
                        nextBody = context.res;
                        nextBodyId = rule.value("id").toString();
                        exit = center(*nextBody);
                        break;
                    }
                    context.pairedStrokeWidth = 0;
                }
                if (deferred && tooth.isEmpty()) continue;
                if (medial.tooth)
                    candidates.push_back({*medial.tooth->right, medial.tooth->left});
                const auto lamBody = formDescription(config, form, "body");
                const bool forkedLam = lamBody.value("shape") == "forked-stem";
                const bool foldedFinal = form.contains("sharedFoldWithFinal");
                std::optional<BodyBoundary> sharedExit;
                if (!bridge.isEmpty() && !candidates.empty() && letter + 1 == lastIndex)
                    sharedExit = BodyBoundary{candidates.front().left, form.value("id").toString(), true};
                if ((forkedLam || foldedFinal) && letter + 1 == lastIndex) {
                    const auto& saved = nextBoundaries[letter];
                    if (saved && (foldedFinal || saved->ligature)) sharedExit = saved;
                    else {
                        const auto originalForm = context.form;
                        const auto originalResult = context.res;
                        for (const auto& ruleValue : finalRules) {
                            const auto rule = ruleValue.toObject();
                            if ((foldedFinal || sharedBoundary(rule)) && applies(rule, letter) &&
                                probeFinal(rule)) {
                                sharedExit = BodyBoundary{context.res, rule.value("id").toString(), true};
                                break;
                            }
                        }
                        context.form = originalForm;
                        context.res = originalResult;
                    }
                }
                if (foldedFinal && !sharedExit) continue;
                if (foldedFinal && !contour.isEmpty()) {
                    // Two bodies sharing a folded attachment have one seam.
                    // Generate the head around that seam and recheck its own
                    // retained features, rather than probing into the bowl below.
                    candidates.clear();
                    const auto evidence = form.value("body").toObject().value("shape") == "paired-route"
                                              ? recognizedSharedShape(context, form.value("body").toObject())
                                              : nullptr;
                    const auto proposals =
                        evidence ? cutPairedRoutes(context, evidence->pairedRoutes,
                                                   form.value("body").toObject().value("routes").toObject(),
                                                   form.value("cuts").toObject().value("routes").toObject(),
                                                   &sharedExit->cut)
                                 : contourBodyBoundaries(context, contour, &sharedExit->cut);
                    for (const auto &[right, left] : proposals)
                        candidates.emplace_back(right, left);
                }
                if (lamDescription(form.value("body").toObject())) {
                    const auto evidence = recognizedMedialLamBodies(context, lamBody);
                    const auto declarations = assembleMedialLamCuts(context, config, form, *evidence, incoming,
                        entry, exit, stroke, open, recoverTeeth, pageIndex >= 0,
                        sharedExit ? std::optional<Stretching>{sharedExit->cut} : std::nullopt);
                    candidates.insert(candidates.end(), declarations.begin(), declarations.end());
                }
                const auto teeth = form.value("seenTeeth").toObject();
                std::array<PathExtrema,3> seenTips{};
                if (!teeth.isEmpty()) {
                    Stretching right{}, left{};
                    // A compact form may reclaim teeth swallowed by a generic
                    // cut, but it must not count a recognized open head as a tooth.
                    const bool intrinsic = form.value("sharedEntry").toBool();
                    const auto previousHead = ownedOpenHeads.constFind(letter - 1);
                    const double rightLimit = !intrinsic ? entry.x() :
                        previousHead == ownedOpenHeads.cend() ? context.bbox.right() : previousHead->point.x();
                    const double leftLimit = intrinsic || open || foldedFinal ? context.bbox.left() : exit.x() - stroke;
                    if (cutToothClusters(context,
                                         recognizedSharedShape(context, form.value("body").toObject())->clusters, teeth,
                                         leftLimit, rightLimit, &right, &left, foldedFinal ? &sharedExit->cut : nullptr,
                                         &seenTips, seenRetryFor(letter, teeth)))
                        if (!intrinsic || letter == 1 || before(cuts[letter - 2], right))
                            candidates.emplace_back(right, left);
                }
                const bool allowWalk = !internalBody && !forkedLam && teeth.isEmpty();
                appendMedialWalks(context, form, entry, exit, stroke, open, allowWalk, candidates);
                bool matched = false;
                const bool preferFinalBoundary = forkedLam && letter + 1 == lastIndex &&
                    !outgoing.fixedLeftBoundary;
                for (int pass = 0; pass < (preferFinalBoundary ? 2 : 1) && !matched; ++pass) {
                for (auto [right, left, sharedJoins, hollowCuts, lamCrest, lamFork] : candidates) {
                    if (form.contains("bodyCounters")) {
                        const auto pieces = cutRegions(context.transformedPath, {}, {right, left}, context.orientation, true);
                        if (pieces.size() != 3 || static_cast<int>(toSubpaths(pieces[1].path).size()) - 1 !=
                            form.value("bodyCounters").toInt()) continue;
                    }
                    // A stacked ligature has one intrinsic interface between
                    // bodies. It replaces the preceding generic initial cut;
                    // no separate stretch of connecting ink exists there.
                    // A seen entry may overlap an earlier cut, but separated
                    // boundaries retain the genuine incoming connector.
                    sharedJoins |= form.value("reclaimEntry").toBool() && !before(incoming,right);
                    sharedJoins |= form.value("sharedEntry").toBool() &&
                        (teeth.isEmpty() || !before(incoming, right));
                    if (sharedJoins && letter == 1 && retainsInitialConnector(incoming, right))
                        sharedJoins = false;
                    if (medial.tooth && !bodyTemplateIds[letter - 1].isEmpty()) {
                        const auto side = getJoinTime(incoming, context.orientation);
                        // An initial or medial form may itself end in a tooth
                        // (notably seen and sad). A later letter cannot claim
                        // a tip and its bend already inside that matched body.
                        if (side.isStrictRight(medial.tooth->tip.time) && side.isStrictRight(medial.tooth->bend.time)) {
                            const auto rules = letter == 1 ? config.value("initial").toArray() : medialForms;
                            const bool intrinsicTooth = std::any_of(rules.begin(), rules.end(), [&](const auto& value) {
                                const auto prior = value.toObject();
                                return prior.value("id") == bodyTemplateIds[letter - 1] &&
                                       (hasToothBody(prior) || hasSeenBody(prior) ||
                                        loopSettings(prior).contains("exitTooth"));
                            });
                            if (intrinsicTooth) continue;
                        }
                    }
                    if (medial.tooth && !before(incoming, right) && before(incoming, left)) {
                        // The preceding body's cut can fall inside this tooth's
                        // incoming margin. Share it only if the actual remaining
                        // mask retains the complete tip and the facing bend.
                        const auto& feature = *medial.tooth;
                        const auto side = getJoinTime(incoming, context.orientation);
                        if (side.isStrictLeft(feature.tip.time) && side.isStrictLeft(feature.bend.time)) {
                            const auto parts = cutRegions(context.transformedPath, {}, {incoming, left}, context.orientation, true);

                            if (parts.size() == 3 && toSubpaths(parts[1].path).size() == 1 &&
                                feature.retainedBy(parts[1].path)) {
                                sharedJoins = true;
                                right = incoming;
                            }
                        }
                    }
                    if (!sharedJoins && !before(incoming, right) && form.value("tightJoin").toBool()) {
                        const double width = std::max(QLineF(incoming.topBaseLine.point,incoming.bottomBaseLine.point).length(),
                            QLineF(right.topBaseLine.point,right.bottomBaseLine.point).length());
                        sharedJoins = QLineF(center(incoming),center(right)).length() <= width;
                    }
                    if (sharedJoins && !ownedTeeth.isEmpty()) {
                        const auto sides = getJoinTime(right, context.orientation);
                        if (std::any_of(ownedTeeth.cbegin(), ownedTeeth.cend(), [&](const ToothBody& owned) {
                            return !sides.isStrictRight(owned.tip.time) || !sides.isStrictRight(owned.bend.time);
                        })) {
                            subWord.templateTrace.append(form.value("id").toString() +
                                " · shared entry would take a previously matched tooth");
                            continue;
                        }
                    }
                    if (sharedJoins && !teeth.isEmpty() && ownedOpenHeads.contains(letter - 1) &&
                        !getJoinTime(right, context.orientation).isStrictRight(ownedOpenHeads[letter - 1].time))
                        continue;
                    // A close shared seam may still cut around the whole lam
                    // shaft and give its crest to the following letter.
                    const auto previousLam = ownedLamCrests.constFind(letter - 1);
                    if (sharedJoins && previousLam != ownedLamCrests.cend() &&
                        !getJoinTime(right, context.orientation).isStrictRight(previousLam->time)) continue;
                    if (sharedJoins && letter == 1 && !preservesBody(bodyTemplateIds[0], right, true)) continue;
                    if ((!sharedJoins && !before(incoming, right)) || !before(right, left) ||
                        (!open && !nextBody && !internalBody && !forkedLam && std::abs(center(left).x() - exit.x()) > 2 * stroke) ||
                        (letter + 1 < static_cast<int>(cuts.size()) && !before(left, cuts[letter + 1]))) continue;

                    if (form.value("pairedCounters").toBool()) {
                        bool above = false, below = false;
                        const double middle = (center(right).y() + center(left).y()) / 2;
                        for (size_t i = 1; i < context.subPaths.size(); ++i) {
                            const auto bounds = context.subPaths[i].boundingRect();
                            const auto point = bounds.center();
                            if (bounds.left() < center(left).x() || bounds.right() > center(right).x() ||
                                context.transformedPath.contains(point)) continue;
                            above |= point.y() < middle;
                            below |= point.y() > middle;
                        }
                        if (!above || !below) continue;
                    }

                    auto trial = cuts;
                    trial[letter] = left;
                    if (sharedJoins) trial[letter - 1] = right;
                    else trial.insert(trial.begin() + letter, right);
                    const auto& path = shape.path;
                    const auto validCuts = [&](const std::vector<Stretching>& proposal) {
                        auto active = proposal;
                        // A later medial letter can still have an empty reserved
                        // slot. It is not a geometric cut and must not invalidate
                        // the complete body currently being checked.
                        if ((pendingMedialJoin || (deferredFinalJoin && !resolvedFinalJoin)) &&
                            letter + 1 < static_cast<int>(cuts.size())) active.pop_back();
                        const auto regions = cutRegions(path, subWord.text, active, subWord.basePathOrientation, true);
                        return regions.size() == active.size() + 1 &&
                            std::none_of(regions.begin(), regions.end(), [](const GlyphInfo& glyph) {
                                return glyph.path.boundingRect().isEmpty();
                            });
                    };

                    auto finalBoundary = nextBody;
                    auto finalTemplate = nextBodyId;
                    if (sharedExit) {
                        finalBoundary = sharedExit->cut;
                        finalTemplate = sharedExit->templateId;
                    } else if (finalBoundary) {
                        if (!before(left, *finalBoundary)) continue;
                        trial.insert(trial.begin() + letter + (sharedJoins ? 1 : 2), *finalBoundary);
                    } else if (letter + 1 == lastIndex && !outgoing.fixedLeftBoundary) {
                        // Reuse the boundary already matched for this letter. Re-running
                        // its search here starts after that join and can fail to find it.
                        const auto& saved = nextBoundaries[letter];
                        if (saved && sharesCornerJunction(saved->templateId, left, saved->cut) &&
                            cutPrecedes(right, saved->cut, context.orientation)) {
                            const auto& seam = saved->cut;
                            const auto side = getJoinTime(seam, context.orientation);
                            const auto bodies = cutRegions(context.transformedPath, {}, {right, seam},
                                                           context.orientation, true);
                            auto sharedTrial = trial;
                            sharedTrial[letter + (sharedJoins ? 0 : 1)] = seam;
                            if (bodies.size() == 3 &&
                                (!form.contains("bodyCounters") || int(toSubpaths(bodies[1].path).size()) - 1 ==
                                                                     form.value("bodyCounters").toInt()) &&
                                (!medial.tooth || medial.tooth->retainedBy(bodies[1].path)) &&
                                (teeth.isEmpty() || std::all_of(seenTips.begin(), seenTips.end(),
                                    [&](const auto& tip) { return side.isStrictRight(tip.time); })) &&
                                validCuts(sharedTrial)) {
                                trial = std::move(sharedTrial);
                                left = seam;
                                finalBoundary = seam;
                                finalTemplate = saved->templateId;
                                sharedExit = *saved;
                            }
                        }
                        if (!finalBoundary && saved && before(left, saved->cut)) {
                            trial.push_back(saved->cut);
                            if (validCuts(trial)) {
                                finalBoundary = saved->cut;
                                finalTemplate = saved->templateId;
                            } else trial.pop_back();
                        }
                        // Try the final letter's own boundary before accepting a shared cut.
                        for (const auto& value : finalRules) {
                            if (finalBoundary) break;
                            const auto rule = value.toObject();
                            if (!applies(rule, letter)) continue;
                            context.pairedStrokeWidth = left.referenceWidth > 0 ? left.referenceWidth :
                                QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length();
                            const bool found = probeFinal(rule);
                            context.pairedStrokeWidth = 0;
                            if (!found) continue;
                            if (!bodyTemplateIds[lastIndex].isEmpty() &&
                                !preservesBody(bodyTemplateIds[lastIndex], context.res, false)) continue;
                            if (!before(left, context.res)) {
                                // At a tight tooth/bowl attachment, two normal
                                // probes can straddle the same neck. Share the
                                // final's intrinsic seam only if the tooth keeps
                                // its complete axis and both masks remain valid.
                                if (medial.tooth && rule.value("retainBodyFeatures").toBool() &&
                                    before(right, context.res)) {
                                    const QLineF old(left.topBaseLine.point, left.bottomBaseLine.point);
                                    const QLineF fresh(context.res.topBaseLine.point, context.res.bottomBaseLine.point);
                                    const auto bodies = cutRegions(context.transformedPath, {}, {right, context.res},
                                                                   context.orientation, true);
                                    if (QLineF(old.center(), fresh.center()).length() <=
                                            std::max(old.length(), fresh.length()) &&
                                        bodies.size() == 3 && medial.tooth->retainedBy(bodies[1].path)) {
                                        const size_t exitIndex = letter + (sharedJoins ? 0 : 1);
                                        auto sharedTrial = trial;
                                        sharedTrial[exitIndex] = context.res;
                                        if (validCuts(sharedTrial)) {
                                            trial = std::move(sharedTrial);
                                            left = context.res;
                                            finalBoundary = context.res;
                                            finalTemplate = rule.value("id").toString();
                                            sharedExit = BodyBoundary{context.res, finalTemplate, true};
                                            break;
                                        }
                                    }
                                }
                                // A tight final can overlap the proposed exit of
                                // seen. Keep the final's cut only when it follows
                                // all three tooth tips and reaches the last tip.
                                if (recoverTeeth && !teeth.isEmpty() && !foldedFinal) {
                                    const auto side = getJoinTime(context.res,context.orientation);
                                    const double pitch = QLineF(seenTips.front().point,seenTips.back().point).length()/2;
                                    const auto endpoint = QLineF(context.res.topBaseLine.point,context.res.bottomBaseLine.point);
                                    const bool retained = std::all_of(seenTips.begin(),seenTips.end(),[&](const auto& tip) {
                                        return side.isStrictRight(tip.time) && getJoinTime(right,context.orientation).isStrictLeft(tip.time);
                                    });
                                    if (retained && QLineF(endpoint.center(),seenTips.back().point).length()<=1.5*pitch) {
                                        const size_t exitIndex = letter+(sharedJoins?0:1);
                                        auto sharedTrial=trial; sharedTrial[exitIndex]=context.res;
                                        if (validCuts(sharedTrial)) {
                                            trial=std::move(sharedTrial);
                                            left=context.res;
                                            finalBoundary=context.res;
                                            finalTemplate=rule.value("id").toString();
                                            sharedExit=BodyBoundary{context.res,finalTemplate,true};
                                            break;
                                        }
                                    }
                                }
                                // Independently found boundaries may cross at a
                                // tight join. Reconcile only near-coincident cuts;
                                // the final body's own boundary keeps its tail.
                                const double width = std::min(
                                    QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length(),
                                    QLineF(context.res.topBaseLine.point, context.res.bottomBaseLine.point).length());
                                const double tolerance = config.value("kashidaDefaults").toObject()
                                    .value("sharedCutTolerance").toDouble(0.25) * width;
                                if (QLineF(left.topBaseLine.point, context.res.topBaseLine.point).length() > tolerance ||
                                    QLineF(left.bottomBaseLine.point, context.res.bottomBaseLine.point).length() > tolerance) continue;
                                const size_t exitIndex = letter + (sharedJoins ? 0 : 1);
                                const auto original = trial[exitIndex];
                                trial[exitIndex] = context.res;
                                if (validCuts(trial)) {
                                    left = context.res;
                                    finalBoundary = context.res;
                                    finalTemplate = rule.value("id").toString();
                                    break;
                                }
                                trial[exitIndex] = original;
                                continue;
                            }
                            trial.push_back(context.res);
                            if (validCuts(trial)) {
                                finalBoundary = context.res;
                                finalTemplate = rule.value("id").toString();
                                break;
                            }
                            trial.pop_back();
                        }
                    }
                    if (composeSeenTail) {
                        const auto first = getJoinTime(right, context.orientation);
                        const auto last = getJoinTime(left, context.orientation);
                        const bool keepsTips = std::all_of(seenTips.begin(), seenTips.end(), [&](const auto& tip) {
                            return first.isStrictLeft(tip.time) && last.isStrictRight(tip.time);
                        });
                        const bool ownsTail = std::any_of(finalRules.begin(), finalRules.end(), [&](const auto& value) {
                            const auto rule = value.toObject();
                            return rule.value("id") == finalTemplate && rule.value("ownsLeftTip").toBool();
                        });
                        if (!keepsTips || !ownsTail || !finalBoundary || !before(left, *finalBoundary)) continue;
                    }
                    if (!validCuts(trial)) continue;
                    // A shared exit must still describe the already matched
                    // final body; a medial candidate cannot take its counter
                    // and leave only the tail under the final letter's name.
                    if (letter + 1 == lastIndex && !finalBoundary && !outgoing.fixedLeftBoundary &&
                        !preservesBody(bodyTemplateIds[lastIndex], left, false)) continue;
                    // First prefer an intrinsic lam route that composes with
                    // the final body's own cut. A partial route remains useful
                    // if none of the alternatives can complete that boundary.
                    if (((preferFinalBoundary && pass == 0) || medial.requireFinal) &&
                        !finalBoundary && bodyTemplateIds[lastIndex].isEmpty()) continue;

                    if (hollowCuts && !sharedJoins && !sharedExit &&
                        (!finalBoundary || before(hollowCuts->second, *finalBoundary))) {
                        // Refine only after the original route has passed the full partition check.
                        trial[letter] = hollowCuts->first;
                        trial[letter + 1] = hollowCuts->second;
                        if (validCuts(trial)) {
                            right = hollowCuts->first;
                            left = hollowCuts->second;
                        }
                    }
                    if (enumerate && !plan->accepts(letter, right, left)) continue;
                    if (sharedJoins) {
                        incoming.topBaseLine = incoming.topRight = right.topBaseLine;
                        incoming.bottomBaseLine = incoming.bottomRight = right.bottomBaseLine;
                        incoming.referenceWidth = right.referenceWidth;
                        incoming.fixedRightBoundary = true;
                        incoming.length = 0;
                        incoming.kashidaOverrides.insert("enabled", false);
                    }
                    incoming.topLeft = right.topBaseLine;
                    incoming.bottomLeft = right.bottomBaseLine;
                    incoming.fixedLeftBoundary = true;
                    if (left.counterEntry.pathIndex > 0) {
                        outgoing.counterBendWeights = left.counterBendWeights;
                        outgoing.counterEntry = left.counterEntry;
                        outgoing.counterExit = left.counterExit;
                        outgoing.kashidaOverrides.insert("enabled", false);
                    }
                    outgoing.topBaseLine = left.topBaseLine;
                    outgoing.bottomBaseLine = left.bottomBaseLine;
                    outgoing.referenceWidth = left.referenceWidth;
                    outgoing.fixedRightBoundary = true;
                    outgoing.length = 0;
                    // Sharing this body's entry does not make its exit shared.
                    // Keep the two independent endpoints when the transcript
                    // explicitly includes this connector. The ink between the
                    // endpoints still has to pass the contour checks.
                    const bool markedSpan = hasFinalTatweel && outgoing.fixedLeftBoundary;
                    if (sharedJoins && !forkedLam && !finalBoundary && !markedSpan && letter + 1 == lastIndex)
                        outgoing.kashidaOverrides.insert("enabled", false);
                    if (!finalTemplate.isEmpty()) {
                        bodyTemplateIds[lastIndex] = finalTemplate;
                        if (!subWord.matchedTemplates.contains(finalTemplate))
                            subWord.matchedTemplates.append(finalTemplate);
                        subWord.templateTrace.append(QString("%1 → %2 · %3 · %4")
                            .arg(baseText.mid(letter, 1), baseText.mid(letter + 1, 1), finalTemplate,
                                sharedExit ? "shared ligature cut" : "paired body boundary matched"));
                    }
                    if (sharedExit) {
                        outgoing.topRight = outgoing.topLeft = left.topBaseLine;
                        outgoing.bottomRight = outgoing.bottomLeft = left.bottomBaseLine;
                        outgoing.fixedLeftBoundary = false;
                        outgoing.kashidaOverrides.insert("enabled", false);
                    } else if (finalBoundary) {
                        outgoing.topLeft = finalBoundary->topBaseLine;
                        outgoing.bottomLeft = finalBoundary->bottomBaseLine;
                        outgoing.fixedLeftBoundary = true;
                    }
                    const auto id = form.value("id").toString();
                    subWord.matchedTemplates.append(id);
                    bodyTemplateIds[letter] = id;
                    if (medial.tooth) ownedTeeth.insert(letter, *medial.tooth);
                    if (lamCrest) ownedLamCrests.insert(letter, *lamCrest);
                    if (lamFork) matchedLamForks.insert(letter, *lamFork);
                    if (!openHead.isEmpty()) {
                        const auto entrySide = getJoinTime(right, context.orientation);
                        const auto exitSide = getJoinTime(left, context.orientation);
                        const PathExtrema* crest = nullptr;
                        for (const auto& point : context.timeSorted) {
                            if (entrySide.isStrictLeft(point.time) && exitSide.isStrictRight(point.time) &&
                                (!crest || point.point.y() < crest->point.y())) crest = &point;
                        }
                        if (crest) ownedOpenHeads.insert(letter, *crest);
                    }
                    if (form.value("sharedEntry").toBool() && sharedJoins) {
                        sharedEntries.append(letter);
                        subWord.templateTrace.append(id + " · shared incoming ligature cut");
                    }
                    subWord.templateTrace.append(id + (lamFork ? " · medial fork boundaries matched" :
                        " · medial body boundaries matched"));
                    if (deferred) resolvedFinalJoin = true;
                    if (open) pendingMedialJoin = false;
                    matched = true;
                    break;
                }
                }
                if (matched) break;
            }
            if (open && !pendingMedialJoin && canProposeMedialBoundary(letter + 1)) {
                cuts.emplace_back();
                pendingMedialJoin = true;
            }
        }
        if (pendingMedialJoin || (deferredFinalJoin && !resolvedFinalJoin)) cuts.pop_back();
        storeJoins(cuts);
    }

    // A final form can find its entry independently after the preceding body
    // has supplied its exit. Keep both cuts when they bound an actual ribbon;
    // overlapping proposals at the same attachment share the final's seam.
    if (preserveReadingOrder && subWord.joins.size() == lastIndex && lastIndex > 0 &&
        bodyTemplateIds.back().isEmpty() &&
        std::none_of(bodyTemplateIds.cbegin(), bodyTemplateIds.cend() - 1,
                     [](const QString &id) { return id.isEmpty(); })) {
        const auto original = subWord;
        const auto exit = *original.joins.rbegin();
        const QLineF outside(exit.topBaseLine.point, exit.bottomBaseLine.point);
        for (const auto &value : finalRules) {
            const auto rule = value.toObject();
            if (!rule.value("terminalAttachment").toBool() || !applies(rule, lastIndex - 1) ||
                !attempt(rule, lastIndex - 1, false))
                continue;
            const auto entry = context.res;
            const QLineF inside(entry.topBaseLine.point, entry.bottomBaseLine.point);
            const auto side = getJoinTime(exit, context.orientation);
            const bool ordered =
                side.isStrictLeft(entry.topBaseLine.time) && side.isStrictLeft(entry.bottomBaseLine.time);
            const bool close =
                QLineF(outside.center(), inside.center()).length() <= 1.5 * std::min(outside.length(), inside.length());
            bool touchingCounter = false;
            if (rule.value("stemFoot").toBool() ||
                formDescription(config, rule, "body").value("shape") == "terminal-fork")
                for (size_t i = 1; i < context.subPaths.size(); ++i) {
                    const auto box = context.subPaths[i].boundingRect();
                    if (box.left() >= inside.center().x() - inside.length() * .3 &&
                        QLineF(QPointF(box.left(), std::clamp(inside.center().y(), box.top(), box.bottom())),
                               inside.center())
                                .length() <= inside.length() * 1.5)
                        touchingCounter = true;
                }
            for (bool shared : {false, true}) {
                if (shared ? !(close || touchingCounter) : !ordered)
                    continue;
                subWord = original;
                auto joined = shared ? entry : exit;
                joined.length = 0;
                joined.readingOrder = lastIndex - 1;
                joined.topLeft = entry.topBaseLine;
                joined.bottomLeft = entry.bottomBaseLine;
                joined.fixedRightBoundary = true;
                joined.fixedLeftBoundary = !shared;
                joined.kashidaOverrides.insert("enabled", !shared);
                subWord.joins.erase(std::prev(subWord.joins.end()));
                subWord.joins.insert(joined);
                if (!cutSubword(lineIndex, wordIndex, subWordIndex, config) ||
                    subWord.baseGlyphs.size() != baseText.size() || subWord.kashidaPaths.back().isEmpty() != shared)
                    continue;
                const auto expected = cutRegions(shape.path, {}, {entry}, context.orientation, true);
                if (expected.size() != 2 || subWord.baseGlyphs.back().path != expected[1].path)
                    continue;
                const auto id = rule.value("id").toString();
                bodyTemplateIds.back() = id;
                subWord.matchedTemplates.append(id);
                subWord.templateTrace.append(id +
                                             (shared ? " · shared body attachment" : " · separate body attachment"));
                break;
            }
            if (!bodyTemplateIds.back().isEmpty())
                break;
            subWord = original;
        }
    }

    // No first connecting span is justified until the initial body has a
    // matched template. Keep that ink unresolved when only later bodies match.
    if (!subWord.joins.empty() && bodyTemplateIds[0].isEmpty()) {
        auto shared = *subWord.joins.begin();
        shared.length = 0;
        shared.kashidaOverrides.insert("enabled", false);
        subWord.joins.erase(subWord.joins.begin());
        subWord.joins.insert(shared);
    }
	const bool cut = (!deferredFinalJoin || resolvedFinalJoin) && cutSubword(lineIndex, wordIndex, subWordIndex, config);
    if (cut && !subWord.joins.empty() && !bodyTemplateIds[lastIndex].isEmpty() &&
        intrinsicFinalCuts.contains(bodyTemplateIds[lastIndex])) {
        auto actual = *subWord.joins.rbegin();
        if (!subWord.kashidaPaths.back().isEmpty()) {
            actual.topBaseLine = actual.topLeft;
            actual.bottomBaseLine = actual.bottomLeft;
        }
        if (!preservesBody(bodyTemplateIds[lastIndex], actual, false)) {
            subWord.templateTrace.append(bodyTemplateIds[lastIndex] + " · final mask lost its intrinsic body features");
            bodyTemplateIds[lastIndex].clear();
        }
    }
    // A shared cut can leave a nonempty sliver carrying the final template's
    // name while the preceding body takes the entire descending tail.
    if (cut && subWord.baseGlyphs.size() == baseText.size() && !bodyTemplateIds[lastIndex].isEmpty()) {
        for (const auto& value : finalRules) {
            const auto rule = value.toObject();
            if (rule.value("id") != bodyTemplateIds[lastIndex] || !rule.value("ownsLeftTip").toBool()) continue;
            auto finalInk = shape.transform.map(subWord.baseGlyphs.back().path);
            finalInk.translate(shape.pos);
            const double roundoff = 1e-6 * std::max(1., context.bbox.width());
            if (finalInk.boundingRect().left() > context.bbox.left() + roundoff) {
                subWord.templateTrace.append(bodyTemplateIds[lastIndex] + " · final body lost its left tail tip");
                subWord.matchedTemplates.removeAll(bodyTemplateIds[lastIndex]);
                bodyTemplateIds[lastIndex].clear();
            }
            break;
        }
    }
    // Verify the masks as delivered, after coincident joins have been
    // reconciled and connectors have been cut out.
    const auto visibleFinal = visibleFinalCuts.constFind(bodyTemplateIds[lastIndex]);
    if (cut && subWord.joins.size() == size_t(lastIndex) &&
        (!matchedLamForks.isEmpty() || visibleFinal != visibleFinalCuts.cend())) {
        const auto joins = std::vector<Stretching>(subWord.joins.begin(), subWord.joins.end());
        std::vector<std::vector<QBezier>> sourceCurves;
        const auto boundary = [&](int join, bool receiving) -> std::optional<Stretching> {
            auto seam = joins[join];
            if (subWord.kashidaPaths[join].isEmpty()) return seam;
            if (receiving) {
                seam.topBaseLine = seam.topLeft;
                seam.bottomBaseLine = seam.bottomLeft;
            }
            const auto& caps = subWord.kashidaCuts[join];
            const auto& actual = receiving ? caps.second : caps.first;
            if (seam.topBaseLine.point != actual.p1() || seam.bottomBaseLine.point != actual.p2()) {
                if (sourceCurves.empty())
                    for (const auto& contour : context.subPaths) sourceCurves.push_back(baseContourCurves(contour));
                seam.topBaseLine.point = actual.p1();
                seam.bottomBaseLine.point = actual.p2();
                if (!sourceLandmark(seam.topBaseLine, sourceCurves) || !sourceLandmark(seam.bottomBaseLine, sourceCurves))
                    return {};
            }
            return seam;
        };
        const auto retains = [&](const Stretching& entry, const Stretching* exit, const PathExtrema& point) {
            return getJoinTime(entry, context.orientation).isLeft(point.time) &&
                (!exit || getJoinTime(*exit, context.orientation).isRight(point.time));
        };
        for (auto fork = matchedLamForks.cbegin(); fork != matchedLamForks.cend(); ++fork) {
            const int slot = fork.key();
            const auto entry = boundary(slot - 1, true), exit = boundary(slot, false);
            if (entry && exit && retains(*entry, &*exit, *fork) && retains(*entry, &*exit, ownedLamCrests[slot])) continue;
            subWord.templateTrace.append(bodyTemplateIds[slot] + " · cut separates the stem or fork");
            bodyTemplateIds[slot].clear();
        }
        if (visibleFinal != visibleFinalCuts.cend()) {
            const auto seam = boundary(lastIndex - 1, true);
            if (!seam || !interiorChord(context.transformedPath, seam->topBaseLine, seam->bottomBaseLine, seam->topEndpointTolerance) ||
                !retains(*seam, nullptr, visibleFinal->topBaseLine) || !retains(*seam, nullptr, visibleFinal->bottomBaseLine)) {
                subWord.templateTrace.append(bodyTemplateIds[lastIndex] + " · cut separates the visible entry stroke");
                bodyTemplateIds[lastIndex].clear();
            }
        }
        if (std::none_of(bodyTemplateIds.cbegin(), bodyTemplateIds.cend(), [](const auto& id) { return id.isEmpty(); }) &&
            !coversOriginalInk(word, subWord)) {
            for (auto fork = matchedLamForks.cbegin(); fork != matchedLamForks.cend(); ++fork)
                bodyTemplateIds[fork.key()].clear();
            if (visibleFinal != visibleFinalCuts.cend()) bodyTemplateIds[lastIndex].clear();
            subWord.templateTrace.append("Contour features failed the ink ownership check");
        }
    }
    if (cut && subWord.baseGlyphs.size() == baseText.size()) {
        const auto mapped = [&](const QPainterPath& p) { return shape.transform.map(p).translated(shape.pos); };
        if (config.contains("_observedHeadInk")) {
            for (const auto& [slot, _] : missingHeadInk(word, subWord, baseText, config)) {
                subWord.templateTrace.append(bodyTemplateIds[slot] + (slot == 0 ?
                    " · final mask lost the opening of the initial head" : " · connecting ink contains a letter head"));
                bodyTemplateIds[slot].clear();
            }
            for (const auto& [slot, lips] : outlineCache.misplacedHeads) {
                const auto body = mapped(subWord.baseGlyphs[slot].path);
                if (body.contains(lips.p1()) && body.contains(lips.p2())) continue;
                subWord.templateTrace.append(bodyTemplateIds[slot] + " · cut leaves the observed lips outside the body");
                bodyTemplateIds[slot].clear();
            }
        }
        for (auto it = ownedTeeth.cbegin(); it != ownedTeeth.cend(); ++it) {
            auto mask = shape.transform.map(subWord.baseGlyphs[it.key()].path);
            mask.translate(shape.pos);
            if (!it->retainedBy(mask)) {
                subWord.templateTrace.append(bodyTemplateIds[it.key()] + " · final mask lost its tooth");
                bodyTemplateIds[it.key()].clear();
            }
        }
    }
    // A shared ligature entry can reorder both sides of a folded outline.
    // Show that slot as unresolved if the assembled ink has multiple owners.
    if (cut && !sharedEntries.isEmpty() &&
        std::all_of(bodyTemplateIds.cbegin(), bodyTemplateIds.cend(), [](const auto& id) { return !id.isEmpty(); }) &&
        !coversOriginalInk(word, subWord)) {
        for (int slot : sharedEntries) {
            subWord.templateTrace.append(bodyTemplateIds[slot] + " · shared cut failed ink ownership");
            bodyTemplateIds[slot].clear();
        }
    }
    for (int letter = 0; letter <= lastIndex; ++letter) {
        const auto position = letter == 0 ? QStringLiteral("initial") :
            letter == lastIndex ? QStringLiteral("final") : QStringLiteral("medial");
        QJsonArray candidates;
        for (const auto& value : config.value(position).toArray()) {
            const auto rule = value.toObject();
            if (matchesBody(rule, letter)) candidates.append(rule.value("id"));
        }
        const auto status = !bodyTemplateIds[letter].isEmpty() ? QStringLiteral("matched") :
            candidates.isEmpty() ? QStringLiteral("no-template") :
            bodyAttempted[letter] ? QStringLiteral("no-match") : QStringLiteral("blocked");
        QJsonObject diagnostic{{"letterIndex", letter + 1},
            {"character", baseText.mid(letter, 1)}, {"position", position}, {"status", status},
            {"template", bodyTemplateIds[letter]}, {"candidates", candidates}};
        if (matchedLamForks.contains(letter)) {
            diagnostic.insert("bodyForm", "lam-fork");
            diagnostic.insert("cutPolicy", "lam-fork-flanks");
            diagnostic.insert("boundaryKind", "lam-fork");
        }
        if (status != "matched") subWord.templateTrace.append(QString("%1 %2 · %3")
            .arg(position, baseText.mid(letter, 1), status));
        subWord.letterDiagnostics.append(diagnostic);
    }
    subWord.correct = cut && subWord.joins.size() == lastIndex &&
        std::all_of(bodyTemplateIds.begin(), bodyTemplateIds.end(), [](const auto& id) { return !id.isEmpty(); });

    if (subWord.correct && lastIndex >= 2) for (const auto& value : medialForms) {
        const auto rule = value.toObject();
        const auto attachment = rule.value("upperAttachment").toObject();
        if (attachment.isEmpty()) continue;
        for (int i = 1; i < lastIndex; ++i) {
            if (!rule.value("current").toString().contains(baseText[i]) ||
                !attachment.value("letters").toString().contains(baseText[i-1])) continue;
            const auto& glyphs = subWord.baseGlyphs;
            const auto mapped = [&](int index) { return shape.transform.map(glyphs[index].path).translated(shape.pos); };
            auto original = std::next(subWord.joins.begin(), i-1);
            const auto shared = stackedAttachment(context, attachment, mapped(i-1).boundingRect(),
                                                 mapped(i), *original, *std::next(original));
            if (!shared) continue;
            auto previous = subWord;
            subWord.joins.erase(original);
            const auto inserted = subWord.joins.insert(*shared);
            bool accepted = inserted.second && std::distance(subWord.joins.begin(), inserted.first) == i-1 &&
                cutSubword(lineIndex, wordIndex, subWordIndex, config) && glyphs.size() == previous.baseGlyphs.size() &&
                subWord.kashidaPaths[i-1].isEmpty() &&
                std::abs(mapped(i-1).boundingRect().bottom() - shared->bottomBaseLine.point.y()) < 1e-5;
            // Only the two touching bodies and their shared join may change.
            for (int j = 0; accepted && j <= lastIndex; ++j) {
                if (j != i-1 && j != i) accepted = glyphs[j].path == previous.baseGlyphs[j].path;
                if (j < lastIndex && j != i-1)
                    accepted &= subWord.kashidaPaths[j] == previous.kashidaPaths[j] &&
                                subWord.kashidaCuts[j] == previous.kashidaCuts[j];
            }
            if (!accepted) { subWord = std::move(previous); continue; }
            const auto id = rule.value("id").toString();
            auto slot = subWord.letterDiagnostics[i].toObject();
            slot.insert("entryTemplate", id);
            subWord.letterDiagnostics[i] = slot;
            if (!subWord.matchedTemplates.contains(id)) subWord.matchedTemplates.append(id);
            subWord.templateTrace.append(id + " · shared horizontal cut at the upper-stroke attachment");
        }
    }

    if (subWord.correct && config.value("_checkRecoveredCuts").toBool() &&
        (!plausibleRecoveredBodies(word, subWord, config.value("recoveryChecks").toObject()) ||
         !coversOriginalInk(word, subWord))) {
        subWord.correct = false;
        subWord.templateTrace.append("Recovered cuts failed the body or ink ownership check");
    }
    if (!subWord.correct && !enumerate && !retryRule.isEmpty()) {
        auto alternative = config;
        for (const auto* phase : {"initial", "final"}) {
            QJsonArray rules;
            for (const auto& rule : config.value(phase).toArray()) {
                if (rule.toObject().value("id").toString() != retryRule) rules.append(rule);
            }
            alternative.insert(phase, rules);
        }
        auto partial = subWord;
        if (segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true, &alternative, recoverTeeth, outlineCache, plan, seenRetry)) {
            subWord.templateTrace.prepend(retryRule + " · retried after partial segmentation");
        } else {
            subWord = std::move(partial);
        }
    }

	return subWord.correct;
}
