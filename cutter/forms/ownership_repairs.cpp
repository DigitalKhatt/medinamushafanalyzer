#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

// At an upturned entry the two sides of the rising pen stroke meet the
// underside of the nose at two facing corners. A stacked descending arm
// instead meets the upper lip at a re-entrant corner, with ink above it.
static std::optional<Stretching> lipEntry(SearchContext& ctx, const MouthLips& lips,
                                          double maxThroat, bool stackedArm, const Stretching& preceding) {
    const auto pivot = lips.notch.point;
    const double span = pivot.x() - lips.upper.point.x();
    std::optional<Stretching> best;
    double shortest = std::numeric_limits<double>::infinity();
    const auto separates = [&](const Stretching& cut) {
        const auto side = getJoinTime(cut, ctx.orientation);
        return side.isStrictLeft(lips.upper.time) && side.isRight(preceding.topBaseLine.time) &&
               side.isRight(preceding.bottomBaseLine.time);
    };
    for (auto at = ctx.timeSorted.begin(); at != ctx.timeSorted.end(); ++at) {
        if (!at->isSharp) continue;
        const auto d = at->point - pivot;
        if (stackedArm) {
            if (d.x() >= 0 || d.x() <= -span || d.y() >= -.25 || d.y() < -span) continue;
            for (bool forward : {false, true}) {
                auto arm = (forward ? ctx.next(at) : ctx.prev(at))->point - at->point;
                auto head = (forward ? ctx.prev(at) : ctx.next(at))->point - at->point;
                if (arm.x() < .3 * span || arm.y() > -.25 || head.x() < .3 * span || head.y() <= 0) continue;
                Stretching cut{};
                if (!contourNormalCut(ctx, *at, forward, false, cut, true)) continue;
                const double width = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
                if (width >= shortest || width > .6 * span) continue;
                orientSection(ctx, cut, lips.notch.time, true);
                if (!separates(cut)) continue;
                best = cut; shortest = width;
            }
        } else {
            if (d.x() <= 0 || d.x() > 1.5 * span || d.y() < -.05 || d.y() > .5 * span) continue;
            for (const auto& other : ctx.timeSorted) {
                const auto delta = other.point - at->point;
                if (!other.isSharp || delta.x() <= .04 || delta.x() > maxThroat ||
                    std::abs(delta.y()) > .35 * delta.x()) continue;
                const double width = std::hypot(delta.x(), delta.y());
                if (width >= shortest || !interiorChord(ctx.transformedPath, *at, other)) continue;
                Stretching cut{};
                cut.topBaseLine = *at; cut.bottomBaseLine = other;
                orientSection(ctx, cut, lips.notch.time, true);
                if (!separates(cut)) continue;
                best = cut; shortest = width;
            }
        }
    }
    return best;
}

// Match the sending head by its already established right cut. Another crest
// in this outline can satisfy the same shape template. Only an unambiguous
// left cut of this particular head can start its outgoing kashida.
static std::optional<Stretching> matchedOpenHeadExit(SearchContext &ctx, const QJsonObject &config, const QString &id,
                                                     const Stretching &entry, const Stretching &nextEntry) {
    const auto sameSeam = [](const Stretching& a, const Stretching& b) {
        return std::abs(a.topBaseLine.time - b.topBaseLine.time) < 1e-5 &&
               std::abs(a.bottomBaseLine.time - b.bottomBaseLine.time) < 1e-5;
    };
    for (const auto &value : config.value("medial").toArray()) {
        const auto rule = resolvedBodyForm(config, value.toObject());
        if (rule.value("id") != id || openHeadSettings(rule).isEmpty())
            continue;
        if (ctx.subPaths.empty()) {
            const auto paths = toSubpaths(ctx.transformedPath);
            ctx.subPaths.assign(paths.cbegin(), paths.cend());
        }
        std::optional<Stretching> exit;
        for (const auto &[right, left] : declaredOpenHeadCuts(ctx, rule, ctx.bbox.left(), ctx.bbox.right())) {
            if (!sameSeam(right, entry)) continue;
            const auto side = getJoinTime(left, ctx.orientation);
            if (!side.isStrictLeft(nextEntry.topBaseLine.time) || !side.isStrictLeft(nextEntry.bottomBaseLine.time))
                continue;
            if (exit && !sameSeam(*exit, left)) return {};
            exit = left;
        }
        return exit;
    }
    return {};
}

} // namespace MedinaCutter

// Refine an already identified open mouth on the original contour. Work only
// between its actual neighbouring bodies; an ambiguous mouth keeps its cuts.
void PageAnalysisResult::refineLipCuts(int line, int wordIndex, int subIndex, const QJsonObject& config) {
    auto& word = page.lines[line].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subIndex];
    const auto letters = bodyLetters(sub.text);
    if (letters.size() < 3 || sub.letterDiagnostics.size() != letters.size() ||
        sub.kashidaPaths.size() != sub.joins.size() || sub.kashidaCuts.size() != sub.joins.size() ||
        sub.baseGlyphs.size() != size_t(letters.size()) || int(sub.joins.size()) + 1 != letters.size()) return;
    QJsonObject form;
    for (const auto& v : config.value("medial").toArray()) {
        const auto rule = bodyConstraintForm(config, v.toObject());
        if (rule.value("mouth").toObject().contains("exitMargin") &&
            std::any_of(letters.cbegin() + 1, letters.cend() - 1,
                        [&](QChar ch) { return rule.value("current").toString().contains(ch); })) {
            form = rule; break;
        }
    }
    if (form.isEmpty()) return;
    const auto mouth = form.value("mouth").toObject();
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    auto ink = mapped(shape.path); ink.setFillRule(Qt::WindingFill);
    SearchContext ctx{.transformedPath = ink, .bbox = ink.boundingRect(), .subword = sub};
    ctx.orientation = sub.basePathOrientation;
    for (const auto& mark : sub.extrema) if (!mark.pathIndex) ctx.timeSorted.insert(mark);
    const auto mouths = mouthLips(ctx, mouth);
    const auto before = [&](const Stretching& first, const Stretching& next) {
        const auto side = getJoinTime(first, ctx.orientation);
        return side.isStrictLeft(next.topBaseLine.time) && side.isStrictLeft(next.bottomBaseLine.time);
    };
    const auto chord = [](const Stretching& s) { return QLineF(s.topBaseLine.point, s.bottomBaseLine.point); };
    const auto receiver = [&](Stretching cut, int index) {
        if (!sub.kashidaPaths[index].isEmpty()) {
            cut.topBaseLine = cut.topLeft; cut.bottomBaseLine = cut.bottomLeft;
        }
        return cut;
    };
    const auto join = [](Stretching sending, const Stretching& receiving, int order, bool shared) {
        sending.topRight = sending.topBaseLine; sending.bottomRight = sending.bottomBaseLine;
        sending.topLeft = receiving.topBaseLine; sending.bottomLeft = receiving.bottomBaseLine;
        sending.fixedLeftBoundary = sending.fixedRightBoundary = true;
        sending.readingOrder = order; sending.length = 0;
        sending.kashidaOverrides.insert("enabled", !shared);
        return sending;
    };
    for (int i = 1; i < letters.size() - 1; ++i) {
        if (!form.value("current").toString().contains(letters[i])) continue;
        const auto slot = sub.letterDiagnostics[i].toObject();
        const bool ain = QStringLiteral("عغ").contains(letters[i-1]);
        const bool seen = QStringLiteral("سش").contains(letters[i-1]);
        if (!ain && !seen && slot.value("template") != form.value("id")) continue;
        auto cuts = std::vector<Stretching>(sub.joins.begin(), sub.joins.end());
        const auto entry = cuts[i-1]; const auto exit = cuts[i];
        if (entry.counterEntry.pathIndex > 0 || exit.counterEntry.pathIndex > 0 ||
            toSubpaths(sub.baseGlyphs[i].path).size() != 1) continue;
        const auto oldHeadStart = receiver(entry, i-1);
        const auto nextBody = receiver(exit, i);
        const auto rightSide = getJoinTime(oldHeadStart, ctx.orientation);
        const auto leftSide = getJoinTime(exit, ctx.orientation);
        const MouthLips* found = nullptr;
        for (const auto& m : mouths) {
            if (!rightSide.isStrictLeft(m.notch.time) || !rightSide.isStrictLeft(m.upper.time) ||
                !leftSide.isStrictRight(m.notch.time) || !leftSide.isStrictRight(m.upper.time)) continue;
            if (found) { found = nullptr; break; }
            found = &m;
        }
        if (!found) continue;
        // At the free tip's projection there is only the departing stroke.
        // Keep the last fraction of the nib with the lip before starting the join.
        Stretching newExit = exit, projected{};
        const double x = found->upper.point.x() - mouth.value("exitMargin").toDouble();
        if (medialChordAt(ctx, x, found->lower.point.y(), 2.5, projected)) {
            orientSection(ctx, projected, found->notch.time, false);
            if (before(oldHeadStart, projected) && before(projected, nextBody) &&
                chord(projected).length() <= 2.5 && std::abs(chord(projected).center().x() - chord(exit).center().x()) > .05)
                newExit = projected;
        }
        auto headStart = oldHeadStart, sender = entry;
        const bool wasShared = sub.kashidaPaths[i-1].isEmpty();
        bool shared = wasShared;
        bool senderFromTooth = false;
        std::optional<Stretching> ainExit;
        const bool refineSharedEntry = wasShared && mouth.value("separateSharedEntry").toBool();
        if (ain || seen || refineSharedEntry) {
            auto start = lipEntry(ctx, *found, mouth.value("maxThroat").toDouble(), false, oldHeadStart);
            if (!start && ain) start = lipEntry(ctx, *found, 0, true, oldHeadStart);
            if (start && before(oldHeadStart, *start) && before(*start, newExit)) {
                if (ain && shared) { sender = headStart = *start; }
                else {
                    headStart = *start; shared = false;
                    if (seen && i == 1) {
                        for (const auto& v : config.value("initial").toArray()) {
                            auto rule = v.toObject();
                            if (rule.value("body").toObject().value("shape") != "tooth-cluster" ||
                                rule.value("id") != sub.letterDiagnostics[0].toObject().value("template"))
                                continue;
                            rule = toothClusterSettings(rule);
                            rule.insert("exitBendFirst", true);
                            rule.insert("exitOffset", .25);
                            Stretching tooth{};
                            if (seenTeethBoundaries(ctx, rule, ctx.bbox.left(), ctx.bbox.right()+1e-6, nullptr, &tooth) &&
                                before(entry, tooth) && before(tooth, headStart)) {
                                sender = tooth;
                                senderFromTooth = true;
                            }
                            break;
                        }
                    }
                }
            }
            if (ain && i > 1 && shared && start && chord(headStart) == chord(*start)) {
                ainExit = matchedOpenHeadExit(ctx, config,
                                              sub.letterDiagnostics[i - 1].toObject().value("template").toString(),
                                              receiver(cuts[i - 2], i - 2), headStart);
            }
        }
        const bool exitChanged = chord(newExit) != chord(exit);
        const auto apply = [&](const Stretching& sending, bool sharedEntry) {
            const bool entryChanged = sharedEntry != wasShared || chord(sending) != chord(entry) ||
                                      chord(headStart) != chord(oldHeadStart);
            if (!entryChanged && !exitChanged) return false;
            cuts[i-1] = entryChanged ? join(sending, headStart, entry.readingOrder, sharedEntry) : entry;
            if (exitChanged) cuts[i] = join(newExit, nextBody, exit.readingOrder, false);
            auto previous = sub;
            sub.joins.clear();
            for (const auto& c : cuts) sub.joins.insert(c);
            bool ok = sub.joins.size() == cuts.size() && cutSubword(line, wordIndex, subIndex, config) &&
                      sub.baseGlyphs.size() == previous.baseGlyphs.size();
            // A distinct exit is useful only if the two specified seams bound
            // one connecting stroke and the other letter masks retain their ink.
            for (int j = 0; ok && j < letters.size(); ++j) {
                if (j != i-1 && j != i) ok = sub.baseGlyphs[j].path == previous.baseGlyphs[j].path;
                if (j >= int(cuts.size())) break;
                if (j != i-1 && j != i) ok &= previous.kashidaPaths[j] == sub.kashidaPaths[j];
                else if ((j == i-1 && entryChanged && !sharedEntry) || (j == i && exitChanged)) {
                    const auto& [r,l] = sub.kashidaCuts[j];
                    ok &= r == chord(j == i ? newExit : sending) && l == chord(j == i ? nextBody : headStart) &&
                          followsOneStroke(mapped(sub.kashidaPaths[j]), r, l);
                } else if (j == i-1 && sharedEntry) ok &= sub.kashidaPaths[j].isEmpty();
            }
            if (ok && coversOriginalInk(word, sub)) return true;
            sub = std::move(previous);
            return false;
        };
        const bool separateAin = ainExit && apply(*ainExit, false);
        if (separateAin) {
            sub.templateTrace.append(sub.letterDiagnostics[i-1].toObject().value("template").toString() +
                                     " · own exit starts kashida before the open lip");
        } else if (!apply(sender, shared)) {
            continue;
        }
        const auto id = form.value("id").toString();
        const bool entryChanged = sub.kashidaPaths[i-1].isEmpty() != wasShared || chord(headStart) != chord(oldHeadStart);
        if (entryChanged) {
            recordBoundaryTemplate(sub, i, true, id);
            if (sub.kashidaPaths[i-1].isEmpty()) recordBoundaryTemplate(sub, i - 1, false, id);
            else if (separateAin || senderFromTooth)
                recordBoundaryTemplate(sub, i - 1, false, sub.letterDiagnostics[i-1].toObject().value("template").toString());
        }
        if (exitChanged) recordBoundaryTemplate(sub, i, false, id);
        sub.templateTrace.append(id + " · open lip boundaries refined");
    }
}

void PageAnalysisResult::refineBodyOwnership(int line, int wordIndex, int subIndex, const QJsonObject& config) {
    auto& word = page.lines[line].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subIndex];
    const auto letters = bodyLetters(sub.text);
    const int last = letters.size() - 1;
    if (last < 1 || sub.joins.size() != size_t(last) ||
        sub.kashidaPaths.size() != size_t(last) || sub.kashidaCuts.size() != size_t(last) ||
        sub.baseGlyphs.size() != size_t(last + 1) ||
        sub.letterDiagnostics.size() != last + 1)
        return;
    const auto templateId = [&](int slot) { return sub.letterDiagnostics[slot].toObject().value("template").toString(); };
    const auto initial = templateId(0), terminal = templateId(last);
    const auto shape = word.bodyShape(sub);
    const auto world = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    const auto first = world(sub.baseGlyphs.front().path).boundingRect();
    const auto tail = world(sub.baseGlyphs.back().path).boundingRect();
    const double penWidth = config.value("strokeWidth").toDouble(1);
    const auto& program = bodyCutProgram(config);
    QString stackedExit;
    double shaftAspect = 0, minHeightGain = 0;
    bool overwideStem = false, clippedStem = false;
    for (const auto& id : program.formsByPosition[0].value(letters.front())) {
        const auto& form = program.forms.constFind(id).value();
        const auto policy = form.cuts.value("restoreStackedExit").toObject();
        if (policy.isEmpty() || !hasFollowingLetter(letters, 0, form.nextLetters)) continue;
        shaftAspect = form.body.value("features").toObject().value("maxWidthToHeight").toDouble();
        overwideStem = first.width() > shaftAspect * first.height();
        clippedStem = initial == policy.value("fromTemplate").toString() &&
            templateId(1) == policy.value("nextTemplate").toString() &&
            first.height() < policy.value("maxClippedHeight").toDouble();
        if (!overwideStem && !clippedStem) continue;
        stackedExit = id;
        minHeightGain = policy.value("minHeightGain").toDouble();
        break;
    }
    const bool clippedSeen = initial == "seen-init" && first.width() < 1.5 && first.height() < 2;
    const bool clippedHah = initial == "hah-open-init" && first.width() < 1 && first.height() < 1;
    const bool clippedTooth = initial == "beh-shallow-step-init" && (first.width() < .25 || first.height() < .6);
    const auto& entryJoin = *sub.joins.begin();
    const bool clippedStackedTooth = last >= 2 && initial.startsWith("beh-") &&
        QStringLiteral("جحخ").contains(letters[1]) && first.width() <=
            1.05 * QLineF(entryJoin.topBaseLine.point, entryJoin.bottomBaseLine.point).length();
    const bool clippedKafDescent = last >= 2 && initial.startsWith("kaf-") && templateId(1) == "hah-stacked-medi" &&
        QLineF(entryJoin.topBaseLine.point, entryJoin.bottomBaseLine.point).length() < .6 * penWidth;
    const auto terminalForm = program.forms.constFind(terminal);
    const auto terminalCuts = terminalForm == program.forms.cend() ? QJsonObject{} : terminalForm->cuts;
    const auto &lastJoin = *sub.joins.rbegin();
    const QLineF terminalChord(lastJoin.topBaseLine.point, lastJoin.bottomBaseLine.point);
    const bool broadShoulder = sub.kashidaPaths.back().isEmpty() && terminalChord.length() > 2.8 * penWidth &&
        terminalCuts.value("route").toObject().contains("range");
    struct TurnCut { int join; bool incoming; QString policy; };
    std::vector<TurnCut> turnCuts;
    const auto queueTurn = [&](int join, bool incoming, const QLineF& cap, bool connector) {
        if (letters[join] == QChar(0x0640) || letters[join + 1] == QChar(0x0640)) return;
        const int owner = join + incoming;
        const auto form = program.forms.constFind(templateId(owner));
        if (form == program.forms.cend()) return;
        const auto policy = form->cuts.value("turnBoundaries").toObject().value(incoming ? "right" : "left").toString();
        if (policy.isEmpty()) return;
        const double width = cap.length();
        if (width < .25 * penWidth || width > 2.7 * penWidth) return;
        const double vertical = std::abs(cap.dy()) / width;
        if (policy == "descender" && (!connector || vertical < .4 || vertical > .91)) return;
        if (policy == "stem" && (!connector || vertical > .05 || width > .83 * penWidth)) return;
        if (incoming && form->cuts.value("skipEntryTurnAfter").toString().contains(letters[owner - 1])) return;
        if ((policy == "trim" || policy == "stem-foot") &&
            (vertical < .72 || world(sub.baseGlyphs[owner].path).boundingRect().width() < 2.7 * width))
            return;
        turnCuts.push_back({join, incoming, policy});
    };
    std::vector<int> clippedCounterEntries;
    int slot = 0;
    for (const auto &join : sub.joins) {
        const bool connector = !sub.kashidaPaths[slot].isEmpty();
        const QLineF seam(join.topBaseLine.point, join.bottomBaseLine.point);
        const QLineF cap = connector ? sub.kashidaCuts[slot].second : seam;
        if (join.counterEntry.pathIndex <= 0) {
            queueTurn(slot, false, connector ? sub.kashidaCuts[slot].first : seam, connector);
            queueTurn(slot, true, cap, connector);
        }
        ++slot;
        if (join.counterEntry.pathIndex > 0) continue;
        const auto form = program.forms.constFind(templateId(slot));
        const auto rim = form == program.forms.cend() ? QJsonObject{} : form->cuts.value("entryRim").toObject();
        if (!rim.isEmpty() && std::abs(cap.dy()) >= rim.value("minRiseWidths").toDouble() * penWidth &&
            std::abs(cap.dx()) <= rim.value("maxSlope").toDouble() * std::abs(cap.dy()))
            clippedCounterEntries.push_back(slot);
    }
    const auto reorient = terminalCuts.value("reorientEntry").toObject();
    const auto after = reorient.value("after").toString(), afterTemplate = reorient.value("afterTemplate").toString();
    const bool clippedEntry = !reorient.isEmpty() &&
        (after.isEmpty() || after.contains(letters[last - 1])) &&
        (afterTemplate.isEmpty() || afterTemplate == templateId(last - 1)) &&
        tail.width() < reorient.value("maxWidth").toDouble(INFINITY) &&
        tail.height() < reorient.value("maxHeight").toDouble(INFINITY) &&
        std::min(tail.width(), tail.height()) < reorient.value("maxShortSide").toDouble(INFINITY);
    const bool risingTail = terminal == "reh-rising-tail-fina" && initial == "ain-round-init";
    const bool clippedReh = !risingTail && (terminal == "reh-rising-tail-fina" || terminal == "reh-fina" ||
        terminal == "reh-shouldered-fina" || terminal == "reh-hook-fina");
    const bool clippedKaf = [&] {
        if (initial != "kaf-hook-init" || last != 1 || world(sub.kashidaPaths.front()).boundingRect().width() <= 5)
            return false;
        const auto form = namedLetterForm(config, terminal);
        return form.value("terminalAttachment").toBool() &&
            formDescription(config, form, "body").value("shape") == "counter-bridge";
    }();
    const bool connectedNoon = letters[last] == QChar(0x0646) && !sub.kashidaPaths.back().isEmpty();
    const auto detach = terminalCuts.value("detachedEntry").toObject();
    const bool detachedStroke = detach.value("after").toString().contains(letters[last - 1]) &&
        tail.width() < detach.value("maxMatchedWidth").toDouble() && sub.kashidaPaths.back().isEmpty();
    if (!overwideStem && !clippedStem && !clippedSeen && !clippedHah && !clippedTooth && !clippedStackedTooth &&
        !clippedKafDescent && clippedCounterEntries.empty() && !broadShoulder && !clippedEntry &&
        !risingTail && !clippedReh && !clippedKaf && !detachedStroke && !connectedNoon &&
        turnCuts.empty())
        return;

    WordResultInfo::SubWordInfo trial{};
    SearchContext outline{.transformedPath = world(shape.path), .subword = trial};
    if (!readTerminalOutline(outline)) return;
    outline.baseText = letters;
    outline.joinDefaults = config.value("joinDefaults").toObject();
    const auto form = [&](const char* position, const QString& id) {
        for (const auto& value : config.value(position).toArray())
            if (value.toObject().value("id") == id) return value.toObject();
        return QJsonObject{};
    };
    const auto walk = [&](const char* position, const QString& id,
                          ContourRouteSearch* routeSearch = nullptr) -> std::optional<Stretching> {
        outline.form = form(position, id);
        outline.nextIter = std::next(outline.xSorted.begin());
        outline.res = {};
        if (outline.form.isEmpty() ||
            !declaredSharedBoundary(outline, resolvedBodyForm(config, outline.form), nullptr, false, nullptr, routeSearch))
            return {};
        return outline.res;
    };
    const auto entry = [](Stretching cut) {
        cut.topBaseLine = cut.topLeft;
        cut.bottomBaseLine = cut.bottomLeft;
        return cut;
    };
    const auto replace = [&](int slot, const Stretching& right, const std::optional<Stretching>& left,
                             const QString& trace, const auto& accepts,
                             const std::optional<Stretching>& incoming = std::nullopt) {
        return replaceLocalJoin(word, sub, config, outline.transformedPath, slot, right, left, trace, accepts, incoming);
    };
    std::vector<std::vector<QBezier>> outer;
    const auto assembledCaps = [&](int index) { return fittedJoinCaps(sub, index, outline, outer); };

    if (!turnCuts.empty()) {
        const auto chord = [](const Stretching& cap) {
            return QLineF(cap.topBaseLine.point, cap.bottomBaseLine.point);
        };
        for (const auto& request : turnCuts) {
            const auto caps = assembledCaps(request.join);
            if (!caps) continue;
            const auto& [sending, receiving] = *caps;
            const auto& current = request.incoming ? receiving : sending;
            const int owner = request.join + request.incoming;
            const bool descender = request.policy == "descender", stem = request.policy == "stem";
            const bool stemFoot = request.policy == "stem-foot";
            const bool grow = descender || stem, lowerFlank = stem || stemFoot;
            const double width = chord(current).length();
            const double minTravel = stem ? 2.6 : descender ? 1.1 : stemFoot ? 1.0 : 2.0;
            std::optional<ToothClusterBody> cluster;
            const auto id = templateId(owner);
            const auto declaration = program.forms.constFind(id);
            if (declaration != program.forms.cend() && declaration->body.value("shape") == "tooth-cluster") {
                // A long run near the last tooth can look like a connector.
                // Find this body's three tips before considering the new cut.
                if (request.incoming || owner == 0) continue;
                const auto entrance = assembledCaps(request.join - 1);
                if (!entrance) continue;
                const auto before = getJoinTime(entrance->second, outline.orientation);
                const auto after = getJoinTime(current, outline.orientation);
                const auto settings = declaration->body.value("cluster").toObject();
                for (const auto& candidate : recognizeToothClusters(inkOutline(outline), settings)) {
                    const auto& tips = candidate.turns;
                    if (!candidate.ordinary && !candidate.wide) continue;
                    if (cluster && tips[0].point.x() <= cluster->turns[0].point.x()) continue;
                    const auto owned = [&](int i) {
                        return before.isStrictLeft(tips[i].time) && after.isStrictRight(tips[i].time);
                    };
                    if (owned(0) && owned(2) && owned(4))
                        cluster = candidate;
                }
                if (!cluster) continue;
                const auto& tips = cluster->turns;
                const double pitch = (tips[0].point.x() - tips[4].point.x()) / 2;
                if (tips[4].point.x() - chord(current).center().x() < 1.65 * pitch) continue;
            }
            const auto body = world(sub.baseGlyphs[owner].path);
            const auto bounds = body.boundingRect();
            const auto contourCount = toSubpaths(body).size();
            const std::array<std::pair<bool, double>, 3> walks{{
                {lowerFlank, descender ? 13. : 43.}, {false, 25.}, {true, 43.}}};
            for (int i = 0, count = request.policy == "trim" ? walks.size() : 1; i < count; ++i) {
                const auto [lower, angle] = walks[i];
                const auto& start = lower ? current.bottomBaseLine : current.topBaseLine;
                auto boundary = strokeFlankTurn(outline, start, width, grow || !request.incoming ? 1 : -1,
                                                grow, angle, minTravel);
                if (!boundary || (!grow && boundary->referenceWidth > 1.65 * width)) continue;
                if (lower) std::swap(boundary->topBaseLine, boundary->bottomBaseLine);
                const auto shift = chord(*boundary).center() - chord(current).center();
                if (descender && shift.x() < 1.6 * width) continue;
                if (cluster) {
                    const auto side = getJoinTime(*boundary, outline.orientation);
                    const auto& tips = cluster->turns;
                    if (!side.isStrictRight(tips[0].time) || !side.isStrictRight(tips[2].time) ||
                        !side.isStrictRight(tips[4].time)) continue;
                }
                const auto& right = request.incoming ? sending : *boundary;
                const auto& left = request.incoming ? *boundary : receiving;
                const bool movesEarlier = grow || !request.incoming;
                if (!cutPrecedes(right, left, outline.orientation) ||
                    !cutPrecedes(movesEarlier ? *boundary : current,
                                 movesEarlier ? current : *boundary, outline.orientation)) continue;
                if (replace(request.join, right, left, id + " · cut at the stroke turn", [&](const auto& trial) {
                        const auto updated = world(trial.baseGlyphs[owner].path);
                        return toSubpaths(updated).size() == contourCount &&
                            std::abs(updated.boundingRect().top() - bounds.top()) < 1e-5;
                    })) {
                    recordBoundaryTemplate(sub, owner, request.incoming, id);
                    break;
                }
            }
        }
    }

    placeCounterRimEntries(word, sub, config, outline, clippedCounterEntries);

    if (clippedKafDescent) {
        // The upper elbow and the start of the head both make narrow
        // sections. Locate the latter from the complete descending route.
        if (auto boundary = walk("initial", "kaf-descending-hook-init")) {
            const auto anchor = boundary->topBaseLine;
            const double width = QLineF(anchor.point, boundary->bottomBaseLine.point).length();
            double nearest = 3.1 * width;
            for (const auto &other : outline.timeSorted) {
                const auto gap = other.point - anchor.point;
                const double length = QLineF(anchor.point, other.point).length();
                if (!other.isSharp || std::abs(gap.x()) > .75 * width ||
                    gap.y() < .6 * width || length >= nearest ||
                    !interiorChord(outline.transformedPath, anchor, other)) continue;
                boundary->bottomBaseLine = other;
                nearest = length;
            }
            orientSection(outline, *boundary, outline.ySorted.rbegin()->time, false);
            const auto id = outline.form.value("id").toString();
            if (replace(0, *boundary, {}, id + " · descending stroke retained in kaf", [&](const auto& trial) {
                const auto repaired = world(trial.baseGlyphs.front().path).boundingRect();
                return repaired.height() > 1.45 * first.height() &&
                    std::abs(repaired.top() - first.top()) < 1e-5;
            })) {
                recordBoundaryTemplate(sub, 0, false, id);
                recordBoundaryTemplate(sub, 1, true, id);
            }
        }
    }

    if (broadShoulder) {
        // A short overlap with the sending cut can make assembly fall
        // back to a broad chord through the previous body. Recheck the
        // intrinsic shoulder and keep the join shared at the narrow seam.
        ContourRouteSearch search;
        for (; search.position < search.count; ++search.position) {
            auto cut = walk("final", terminal, &search);
            if (!cut || QLineF(cut->topBaseLine.point, cut->bottomBaseLine.point).length() > .6 * terminalChord.length())
                continue;
            orientSection(outline, *cut, outline.xSorted.rbegin()->time, true);
            if (replace(last - 1, *cut, {}, terminal + " · narrow shoulder retained", [&](const auto& trial) {
                    const auto repaired = world(trial.baseGlyphs.back().path).boundingRect();
                    return repaired.right() < tail.right() && std::abs(repaired.left() - tail.left()) < 1e-5 &&
                        std::abs(repaired.top() - tail.top()) < 1e-5;
                })) {
                recordBoundaryTemplate(sub, last, true, terminal);
                recordBoundaryTemplate(sub, last - 1, false, terminal);
                break;
            }
        }
    }

    // The generic contour route can reach around the head under a stacked
    // shaft. Require actual ink just inside the projecting lip to change owner.
    const auto overwideBoundary = overwideStem ? walk("initial", stackedExit) : std::nullopt;
    if (overwideBoundary) {
        const auto oldShaft = world(sub.baseGlyphs.front().path);
        const auto lip = outline.xSorted.begin()->point;
        const auto stem = program.forms.constFind(stackedExit)->body.value("stem").toObject();
        const double pen = stem.value("penWidth").toDouble(.5);
        const auto shaftRestored = [&](const auto& trial) {
            const auto repaired = world(trial.baseGlyphs.front().path).boundingRect();
            if (repaired.width() > shaftAspect * repaired.height() || repaired.width() >= first.width()) return false;
            const auto head = world(trial.baseGlyphs[1].path);
            for (double depth : {.5, 1.0}) {
                const double x = lip.x() - depth * pen;
                auto crossings = FindInterections(outline.transformedPath,
                    QLineF(x, outline.bbox.top() - 1, x, outline.bbox.bottom() + 1), true);
                std::sort(crossings.begin(), crossings.end(), [](const auto& a, const auto& b) {
                    return a.point.y() < b.point.y();
                });
                std::optional<QPointF> witness;
                double nearest = pen;
                for (size_t i = 1; i < crossings.size(); ++i) {
                    const double top = crossings[i - 1].point.y(), bottom = crossings[i].point.y();
                    if (bottom - top < .01) continue;
                    const QPointF center(x, (top + bottom) / 2);
                    const double distance = std::max({top - lip.y(), lip.y() - bottom, 0.0});
                    if (distance < nearest && outline.transformedPath.contains(center)) {
                        witness = center;
                        nearest = distance;
                    }
                }
                if (!witness || !oldShaft.contains(*witness) || !head.contains(*witness)) return false;
            }
            return true;
        };
        if (replace(0, *overwideBoundary, {}, stackedExit + " · stacked head restored to its own body", shaftRestored)) {
            recordAssembledForm(sub, 0, stackedExit);
            recordBoundaryTemplate(sub, 0, false, stackedExit);
            recordBoundaryTemplate(sub, 1, true, stackedExit);
        }
    }

    if (clippedStem || clippedSeen || clippedHah || clippedTooth || clippedStackedTooth) {
        const auto current = *sub.joins.begin();
        auto connector = sub.kashidaPaths.front().isEmpty() ? std::optional<Stretching>{} : entry(current);
        std::optional<Stretching> boundary;
        QString boundaryId;
        if (clippedStem) {
            boundaryId = stackedExit;
            boundary = walk("initial", boundaryId);
        } else if ((clippedSeen || clippedTooth || clippedStackedTooth) && last >= 2 && QStringLiteral("جحخ").contains(letters[1])) {
            boundaryId = "hah-stacked-medi";
            const auto settings = form("medial", boundaryId).value("upperAttachment").toObject();
            auto attachmentInk = world(sub.baseGlyphs[1].path);
            if (connector) attachmentInk.addPath(world(sub.kashidaPaths.front()));
            boundary = stackedAttachment(outline, settings, first, attachmentInk,
                                         current, *std::next(sub.joins.begin()), true);
            if (boundary) connector.reset();
        } else {
            boundaryId = clippedTooth ? QStringLiteral("beh-foot-init") : initial;
            boundary = walk("initial", boundaryId);
        }
        if (boundary) {
            const auto restoresBody = [&](const auto& trial) {
                const auto repaired = world(trial.baseGlyphs.front().path).boundingRect();
                return clippedStem ? repaired.height() >= minHeightGain * first.height() :
                    repaired.width() >= std::max(.45, 1.5 * first.width()) && repaired.height() >= 1.2;
            };
            if (replace(0, *boundary, connector, boundaryId + " · initial stroke restored at its own boundary", restoresBody)) {
                if (clippedStem) recordAssembledForm(sub, 0, boundaryId);
                recordBoundaryTemplate(sub, 0, false, boundaryId);
                if (!connector) recordBoundaryTemplate(sub, 1, true, boundaryId);
            }
        }
    }

    if (clippedEntry) {
        auto seam = entry(*sub.joins.rbegin());
        orientSection(outline, seam, outline.xSorted.rbegin()->time, true);
        const auto restoresBody = [&](const auto& trial) {
            const auto repaired = world(trial.baseGlyphs.back().path).boundingRect();
            return repaired.left() <= outline.bbox.left() + 1e-5 &&
                repaired.width() >= reorient.value("minWidth").toDouble() &&
                repaired.height() >= reorient.value("minHeight").toDouble();
        };
        const auto source = sub.letterDiagnostics.last().toObject().value("entryTemplate").toString(terminal);
        if (replace(last - 1, seam, {}, terminal + " · terminal stroke restored at its own boundary", restoresBody)) {
            recordBoundaryTemplate(sub, last - 1, false, source);
            recordBoundaryTemplate(sub, last, true, source);
        }
    }

    if (risingTail) {
        const auto join = *sub.joins.rbegin();
        const auto seam = sub.kashidaPaths.back().isEmpty() ? join : entry(join);
        auto boundary = cutAfterEntryCrest(outline, seam);
        if (!boundary && last > 1) {
            const auto declaration = resolvedBodyForm(config, form("final", terminal));
            const auto route = declaration.value("body").toObject().value("entryRecoveryRoute").toObject();
            const auto policy = declaration.value("cuts").toObject().value("entryRecoveryRoute").toObject();
            if (!route.isEmpty()) {
                InspectFormScope inspection(declaration, outline.transformedPath);
                outline.nextIter = std::next(outline.xSorted.begin());
                outline.res = {};
                if (declaredContourRouteCut(outline, route, policy)) {
                    boundary = outline.res;
                    orientSection(outline, *boundary, outline.xSorted.rbegin()->time, true);
                    inspectCandidate("Raised entry recovery", *boundary);
                }
            }
        }
        if (boundary) {
            const auto right = last == 1 ? walk("initial", initial) : std::optional<Stretching>{join};
            if (right && replace(last - 1, *right, boundary, terminal + " · raised entry restored", [&](const auto& trial) {
                    return world(trial.baseGlyphs.back().path).boundingRect().top() < tail.top() - .3;
                })) {
                recordBoundaryTemplate(sub, last, true, terminal);
                if (last == 1) recordBoundaryTemplate(sub, 0, false, initial);
            }
        }
    }

    if (clippedReh) {
        const auto join = *sub.joins.rbegin();
        const bool connected = !sub.kashidaPaths.back().isEmpty();
        const auto seam = connected ? entry(join) : join;
        std::optional<Stretching> preceding;
        if (last > 1) {
            const auto before = *std::next(sub.joins.rbegin());
            preceding = sub.kashidaPaths[last - 2].isEmpty() ? before : entry(before);
            orientSection(outline, *preceding, outline.xSorted.rbegin()->time, true);
        }
        // With an existing connector, the recovered hook must stay within
        // that connector so its sending edge and the previous body stay put.
        auto limit = connected ? std::optional<Stretching>{join} : preceding;
        if (limit) orientSection(outline, *limit, outline.xSorted.rbegin()->time, true);
        const auto boundary = cutAfterEntryCrest(outline, seam, limit ? &*limit : nullptr, true);
        if (boundary) {
            const auto restoredHook = [&](const auto& trial) {
                return world(trial.baseGlyphs.back().path).boundingRect().top() < tail.top() - .2 * tail.height();
            };
            const auto message = terminal + " · upper hook retained in final body";
            const auto chord = [](const Stretching& cut) { return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point); };
            const auto toothId = terminalCuts.value("precedingTooth").toObject().value("refitTemplate").toString();
            const auto previousForm = program.forms.constFind(templateId(last - 1));
            const bool sliver = preceding && !connected &&
                QLineF(chord(*preceding).center(), chord(*boundary).center()).length() < .4 * chord(*boundary).length();
            if (!sliver) {
                if (replace(last - 1, connected ? join : *boundary, connected ? boundary : std::nullopt, message, restoredHook)) {
                    recordBoundaryTemplate(sub, last, true, terminal);
                    if (!connected) recordBoundaryTemplate(sub, last - 1, false, terminal);
                }
            } else if (!toothId.isEmpty() && previousForm != program.forms.cend() && toothDescription(previousForm->body)) {
                // The old tooth match can be the reh hook itself. If giving
                // that hook back leaves a sliver, look for the actual tooth
                // and recover its incoming boundary at the same time.
                const auto oldEntry = *std::next(sub.joins.rbegin());
                const bool ribbon = !sub.kashidaPaths[last - 2].isEmpty();
                const auto declaration = resolvedBodyForm(config, form("medial", toothId));
                if (!declaration.value("current").toString().contains(letters[last - 1])) return;
                auto teeth = declaredToothBoundaries(outline, declaration);
                std::stable_sort(teeth.begin(), teeth.end(), [](const auto& a, const auto& b) {
                    return a.tip.point.y() < b.tip.point.y();
                });
                for (const auto& tooth : teeth) {
                    auto start = *tooth.right;
                    orientSection(outline, start, outline.xSorted.rbegin()->time, true);
                    if (!getJoinTime(start, outline.orientation).isStrictLeft(preceding->topBaseLine.time) ||
                        !getJoinTime(start, outline.orientation).isStrictLeft(preceding->bottomBaseLine.time)) continue;
                    auto incoming = ribbon ? oldEntry : start;
                    incoming.readingOrder = last - 2;
                    incoming.topLeft = start.topBaseLine;
                    incoming.bottomLeft = start.bottomBaseLine;
                    incoming.fixedRightBoundary = true;
                    incoming.fixedLeftBoundary = ribbon;
                    incoming.kashidaOverrides.insert("enabled", ribbon);
                    const auto hookAndTooth = [&](const auto& trial) {
                        return restoredHook(trial) && tooth.retainedBy(world(trial.baseGlyphs[last - 1].path));
                    };
                    auto exit = tooth.left;
                    orientSection(outline, exit, outline.xSorted.rbegin()->time, true);
                    const auto end = chord(exit), startOfReh = chord(*boundary);
                    const bool separate = cutPrecedes(exit, *boundary, outline.orientation) &&
                        QLineF(end.center(), startOfReh.center()).length() >= .75 * std::max(end.length(), startOfReh.length());
                    const auto restored = message + " and preceding tooth restored";
                    if (separate && replace(last - 1, exit, boundary, restored, [&](const auto& trial) {
                            const auto connector = world(trial.kashidaPaths.back());
                            return hookAndTooth(trial) && toSubpaths(connector).size() == 1 &&
                                followsOneStroke(connector, end, startOfReh);
                        }, incoming)) {
                        recordAssembledForm(sub, last - 1, toothId);
                        recordBoundaryTemplate(sub, last, true, terminal);
                        recordBoundaryTemplate(sub, last - 1, true, toothId);
                        recordBoundaryTemplate(sub, last - 1, false, toothId);
                        if (!ribbon) recordBoundaryTemplate(sub, last - 2, false, toothId);
                        break;
                    }
                    if (replace(last - 1, *boundary, {}, restored, hookAndTooth, incoming)) {
                        recordAssembledForm(sub, last - 1, toothId);
                        recordBoundaryTemplate(sub, last, true, terminal);
                        recordBoundaryTemplate(sub, last - 1, true, toothId);
                        recordBoundaryTemplate(sub, last - 1, false, terminal);
                        if (!ribbon) recordBoundaryTemplate(sub, last - 2, false, toothId);
                        break;
                    }
                }
            }
        }
    }

    if (clippedKaf) {
        const double rail = .45 * world(sub.kashidaPaths.front()).boundingRect().width();
        const QJsonObject parallel{{"minRun", rail}, {"minGap", .25}};
        const QString id = "kaf-elbow-init";
        if (const auto cut = walk("initial", id))
            if (replace(0, *cut, entry(*sub.joins.begin()), id + " · lower return retained in kaf", [&](const auto& trial) {
                return containsHeadLips(world(trial.baseGlyphs.front().path), parallel);
            })) recordBoundaryTemplate(sub, 0, false, id);
    }

    if (connectedNoon && sub.kashidaCuts.size() == sub.kashidaPaths.size()) {
        const auto [rightCap, leftCap] = sub.kashidaCuts.back();
        const std::vector<std::vector<QBezier>> outer{baseContourCurves(outline.subPaths.front())};
        const auto readCap = [&](const QLineF& cap) -> std::optional<Stretching> {
            Stretching cut{};
            cut.topBaseLine.point = cap.p1();
            cut.bottomBaseLine.point = cap.p2();
            if (!sourceLandmark(cut.topBaseLine, outer) || !sourceLandmark(cut.bottomBaseLine, outer)) return {};
            orientSection(outline, cut, outline.xSorted.rbegin()->time, true);
            return cut;
        };
        const auto sending = readCap(rightCap), receiving = readCap(leftCap);
        const auto boundary = sending && receiving ? restoreNoonBowlEntry(outline, *receiving, *sending) : std::nullopt;
        if (boundary && replace(last - 1, *sending, boundary, terminal + " · outer bowl retained", [&](const auto& trial) {
                const auto repaired = world(trial.baseGlyphs.back().path).boundingRect();
                return repaired.right() > tail.right() && repaired.left() <= tail.left() + 1e-5 &&
                    repaired.bottom() >= tail.bottom() - 1e-5;
            })) recordBoundaryTemplate(sub, last, true, terminal);
    }

    if (detachedStroke) {
        const auto id = detach.value("template").toString();
        const auto detached = program.forms.constFind(id);
        if (detached == program.forms.cend()) return;
        const auto arm = detached->body.value("arm").toObject();
        const auto stroke = arm.value("singleStroke").toObject();
        const auto contours = toSubpaths(shape.path);
        if (contours.size() != 2) return;
        const auto branch = world(contours[1]);
        const auto main = world(contours[0]);
        const auto box = branch.boundingRect();
        if (box.height() < arm.value("minRise").toDouble() || box.width() < stroke.value("minWidth").toDouble() ||
            box.top() > main.boundingRect().top() + stroke.value("maxTopOffset").toDouble() ||
            box.left() >= tail.left() || branch.intersects(main))
            return;
        const auto top = singleHorizontalStroke(branch, box, box.top() + .25 * box.height());
        const auto bottom = singleHorizontalStroke(branch, box, box.top() + .75 * box.height());
        if (!top || !bottom ||
            bottom->center().x() - top->center().x() < stroke.value("minLeanToHeight").toDouble() * box.height() ||
            std::max(top->length(), bottom->length()) > stroke.value("maxWidthToHeight").toDouble() * box.height())
            return;
        auto previous = sub;
        sub.joins.erase(std::prev(sub.joins.end()));
        if (!cutSubword(line, wordIndex, subIndex, config) || sub.baseGlyphs.size() != size_t(last)) {
            sub = std::move(previous);
            return;
        }
        sub.baseGlyphs.push_back({letters.right(1), contours[1]});
        bool valid = sub.kashidaPaths.size() + 1 == previous.kashidaPaths.size();
        for (int i = 0; valid && i < last - 1; ++i)
            valid = sub.baseGlyphs[i].path == previous.baseGlyphs[i].path &&
                sub.kashidaPaths[i] == previous.kashidaPaths[i];
        if (!valid || !coversOriginalInk(word, sub)) {
            sub = std::move(previous);
            return;
        }
        recordAssembledForm(sub, last, id);
        auto diagnostic = sub.letterDiagnostics[last].toObject();
        diagnostic.insert("boundaryKind", "separate-contours");
        diagnostic.remove("entryTemplate");
        sub.letterDiagnostics[last] = diagnostic;
        auto previousLetter = sub.letterDiagnostics[last - 1].toObject();
        previousLetter.remove("exitTemplate");
        sub.letterDiagnostics[last - 1] = previousLetter;
        sub.templateTrace.append(id + " · detached diagonal retained by its own contour");
    }
}
