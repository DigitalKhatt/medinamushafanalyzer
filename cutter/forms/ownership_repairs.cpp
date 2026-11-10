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
                            rule = seenSettings(rule);
                            rule.insert("exitBendFirst", true);
                            rule.insert("exitOffset", .25);
                            Stretching tooth{};
                            if (seenTeethBoundaries(ctx, rule, ctx.bbox.left(), ctx.bbox.right()+1e-6, nullptr, &tooth) &&
                                before(entry, tooth) && before(tooth, headStart)) sender = tooth;
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
        if (ainExit && apply(*ainExit, false)) {
            sub.templateTrace.append(sub.letterDiagnostics[i-1].toObject().value("template").toString() +
                                     " · own exit starts kashida before the open lip");
        } else if (!apply(sender, shared)) {
            continue;
        }
        auto diagnostic = slot;
        diagnostic.insert("boundaryTemplate", form.value("id"));
        sub.letterDiagnostics[i] = diagnostic;
        if (!sub.matchedTemplates.contains(form.value("id").toString())) sub.matchedTemplates.append(form.value("id").toString());
        sub.templateTrace.append(form.value("id").toString() + " · open lip boundaries refined");
    }
}

void PageAnalysisResult::refineBodyOwnership(int line, int wordIndex, int subIndex, const QJsonObject& config) {
    auto& word = page.lines[line].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subIndex];
    const auto letters = bodyLetters(sub.text);
    const int last = letters.size() - 1;
    if (last < 1 || sub.joins.size() != size_t(last) ||
        sub.kashidaPaths.size() != size_t(last) || sub.baseGlyphs.size() != size_t(last + 1) ||
        sub.letterDiagnostics.size() != last + 1)
        return;
    const auto templateId = [&](int slot) { return sub.letterDiagnostics[slot].toObject().value("template").toString(); };
    const auto initial = templateId(0), terminal = templateId(last);
    const auto shape = word.bodyShape(sub);
    const auto world = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    const auto first = world(sub.baseGlyphs.front().path).boundingRect();
    const auto tail = world(sub.baseGlyphs.back().path).boundingRect();
    const bool clippedLam = initial == "lam-normal-init" && templateId(1) == "hah-stacked-medi" && first.height() < 3.2;
    const bool clippedSeen = initial == "seen-init" && first.width() < 1.5 && first.height() < 2;
    const bool clippedHah = initial == "hah-open-init" && first.width() < 1 && first.height() < 1;
    const bool clippedTooth = initial == "beh-shallow-step-init" && (first.width() < .25 || first.height() < .6);
    const bool lostShaft = terminal == "alef-fina" && letters[last - 1] == QChar(0x0647) &&
        std::min(tail.width(), tail.height()) < .05;
    const bool lostFold = terminal == "yeh-fina" && templateId(last - 1) == "lam-medi" &&
        tail.width() < 1.2 && tail.height() < 1.4;
    const bool risingTail = terminal == "reh-rising-tail-fina" && initial == "ain-round-init";
    const bool clippedReh = !risingTail && (terminal == "reh-rising-tail-fina" || terminal == "reh-fina" ||
        terminal == "reh-shouldered-fina" || terminal == "reh-hook-fina");
    const bool clippedKaf = initial == "kaf-hook-init" && terminal == "waw-corner-bridge-fina" &&
        last == 1 && world(sub.kashidaPaths.front()).boundingRect().width() > 5;
    const bool connectedNoon = letters[last] == QChar(0x0646) && !sub.kashidaPaths.back().isEmpty();
    const bool liftedAlef = terminal == "alef-return-fina" && letters[last - 1] == QChar(0x0644) &&
        tail.width() < 1.5 && sub.kashidaPaths.back().isEmpty();
    if (!clippedLam && !clippedSeen && !clippedHah && !clippedTooth && !lostShaft && !lostFold &&
        !risingTail && !clippedReh && !clippedKaf && !liftedAlef && !connectedNoon)
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
    const auto walk = [&](const char* position, const QString& id, bool seen = false) -> std::optional<Stretching> {
        outline.form = form(position, id);
        outline.nextIter = std::next(outline.xSorted.begin());
        outline.res = {};
        if (outline.form.isEmpty() ||
            !(sharedShapeDescription(outline.form.value("body").toObject())
                  ? declaredSharedBoundary(outline, resolvedBodyForm(config, outline.form))
              : outline.form.contains("body") ? declaredLamBoundary(outline, config, outline.form)
                                              : searchContourWalk(outline)))
            return {};
        return outline.res;
    };
    const auto entry = [](Stretching cut) {
        cut.topBaseLine = cut.topLeft;
        cut.bottomBaseLine = cut.bottomLeft;
        return cut;
    };
    const auto replace = [&](int slot, Stretching right, const std::optional<Stretching>& left,
                             const QString& message, const auto& accepts,
                             const std::optional<Stretching>& incoming = std::nullopt) {
        if (!interiorChord(outline.transformedPath, right.topBaseLine, right.bottomBaseLine) ||
            (left && !interiorChord(outline.transformedPath, left->topBaseLine, left->bottomBaseLine))) return false;
        auto previous = sub;
        right.length = 0;
        right.fixedRightBoundary = true;
        right.fixedLeftBoundary = left.has_value();
        right.topRight = right.topBaseLine;
        right.bottomRight = right.bottomBaseLine;
        right.topLeft = left ? left->topBaseLine : right.topBaseLine;
        right.bottomLeft = left ? left->bottomBaseLine : right.bottomBaseLine;
        right.kashidaOverrides.insert("enabled", left.has_value());
        sub.joins.clear();
        int i = 0;
        for (auto cut : previous.joins) {
            if (i == slot) cut = right;
            else if (incoming && i == slot - 1) cut = *incoming;
            cut.readingOrder = i++;
            sub.joins.insert(cut);
        }
        bool valid = cutSubword(line, wordIndex, subIndex, config) &&
            sub.baseGlyphs.size() == previous.baseGlyphs.size() && sub.kashidaPaths.size() == previous.kashidaPaths.size() &&
            sub.kashidaPaths[slot].isEmpty() == !left && accepts();
        for (size_t j = 0; valid && j < sub.baseGlyphs.size(); ++j)
            if (j != size_t(slot) && j != size_t(slot + 1) && !(incoming && j + 1 == size_t(slot)))
                valid = sub.baseGlyphs[j].path == previous.baseGlyphs[j].path;
        for (size_t j = 0; valid && j < sub.kashidaPaths.size(); ++j)
            if (incoming && j + 1 == size_t(slot)) {
                valid = sub.kashidaPaths[j].isEmpty() == previous.kashidaPaths[j].isEmpty();
                if (valid && !sub.kashidaPaths[j].isEmpty())
                    valid = sub.kashidaCuts[j].first == previous.kashidaCuts[j].first &&
                        sub.baseGlyphs[j].path == previous.baseGlyphs[j].path;
            } else if (j != size_t(slot)) valid = sub.kashidaPaths[j] == previous.kashidaPaths[j];
        if (!valid || !coversOriginalInk(word, sub)) {
            sub = std::move(previous);
            return false;
        }
        sub.templateTrace.append(message);
        return true;
    };

    if (clippedLam || clippedSeen || clippedHah || clippedTooth) {
        const auto current = *sub.joins.begin();
        auto connector = sub.kashidaPaths.front().isEmpty() ? std::optional<Stretching>{} : entry(current);
        std::optional<Stretching> boundary;
        if (clippedLam) {
            const auto declaration = form("initial", "lam-hah-stacked-init");
            if (!declaration.isEmpty() && declaredLamBoundary(outline, config, declaration)) boundary = outline.res;
        } else if ((clippedSeen || clippedTooth) && last >= 2 && QStringLiteral("جحخ").contains(letters[1])) {
            const auto settings = form("medial", "hah-stacked-medi").value("upperAttachment").toObject();
            boundary = stackedAttachment(outline, settings, first, world(sub.baseGlyphs[1].path),
                                         current, *std::next(sub.joins.begin()), true);
            if (boundary) connector.reset();
        } else {
            boundary = walk("initial", clippedTooth ? "beh-foot-init" : initial, clippedSeen);
        }
        if (boundary) {
            const auto restoresBody = [&] {
                const auto repaired = world(sub.baseGlyphs.front().path).boundingRect();
                return clippedLam ? repaired.height() >= 2 * first.height() :
                    repaired.width() >= std::max(.45, 1.5 * first.width()) && repaired.height() >= 1.2;
            };
            replace(0, *boundary, connector, initial + " · initial stroke restored at its own boundary", restoresBody);
        }
    }

    if (lostShaft || lostFold) {
        auto seam = entry(*sub.joins.rbegin());
        orientSection(outline, seam, outline.xSorted.rbegin()->time, true);
        const auto restoresBody = [&] {
            const auto repaired = world(sub.baseGlyphs.back().path).boundingRect();
            return repaired.left() <= outline.bbox.left() + 1e-5 &&
                (lostShaft ? repaired.height() >= 4 : repaired.width() >= 2.5 && repaired.height() >= 1.4);
        };
        replace(last - 1, seam, {}, terminal + " · terminal stroke restored at its own boundary", restoresBody);
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
            if (right)
                replace(last - 1, *right, boundary, terminal + " · raised entry restored", [&] {
                    return world(sub.baseGlyphs.back().path).boundingRect().top() < tail.top() - .3;
                });
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
            const auto restoredHook = [&] {
                return world(sub.baseGlyphs.back().path).boundingRect().top() < tail.top() - .2 * tail.height();
            };
            const auto message = terminal + " · upper hook retained in final body";
            const auto chord = [](const Stretching& cut) { return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point); };
            const bool sliver = preceding && !connected &&
                QLineF(chord(*preceding).center(), chord(*boundary).center()).length() < .4 * chord(*boundary).length();
            if (!sliver) {
                replace(last - 1, connected ? join : *boundary, connected ? boundary : std::nullopt, message, restoredHook);
            } else if (templateId(last - 1).startsWith("beh-")) {
                // The old tooth match can be the reh hook itself. If giving
                // that hook back leaves a sliver, look for the actual tooth
                // and recover its incoming boundary at the same time.
                const auto oldEntry = *std::next(sub.joins.rbegin());
                const bool ribbon = !sub.kashidaPaths[last - 2].isEmpty();
                const auto declaration = resolvedBodyForm(config, form("medial", "beh-shoulder-medi"));
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
                    if (replace(last - 1, *boundary, {}, message + " and preceding tooth restored", [&] {
                            return restoredHook() && tooth.retainedBy(world(sub.baseGlyphs[last - 1].path));
                        }, incoming)) break;
                }
            }
        }
    }

    if (clippedKaf) {
        const double rail = .45 * world(sub.kashidaPaths.front()).boundingRect().width();
        const QJsonObject parallel{{"minRun", rail}, {"minGap", .25}};
        if (const auto cut = walk("initial", "kaf-elbow-init"))
            replace(0, *cut, entry(*sub.joins.begin()), initial + " · lower return retained in kaf", [&] {
                return containsHeadLips(world(sub.baseGlyphs.front().path), parallel);
            });
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
        if (boundary)
            replace(last - 1, *sending, boundary, terminal + " · outer bowl retained", [&] {
                const auto repaired = world(sub.baseGlyphs.back().path).boundingRect();
                return repaired.right() > tail.right() && repaired.left() <= tail.left() + 1e-5 &&
                    repaired.bottom() >= tail.bottom() - 1e-5;
            });
    }

    if (liftedAlef) {
        const auto contours = toSubpaths(shape.path);
        if (contours.size() != 2) return;
        const auto branch = world(contours[1]);
        const auto main = world(contours[0]);
        const auto box = branch.boundingRect();
        if (box.height() < 3 || box.width() < 2 || box.top() > main.boundingRect().top() + 3 ||
            box.left() >= tail.left() || branch.intersects(main))
            return;
        const auto top = singleHorizontalStroke(branch, box, box.top() + .25 * box.height());
        const auto bottom = singleHorizontalStroke(branch, box, box.top() + .75 * box.height());
        if (!top || !bottom || bottom->center().x() - top->center().x() < .3 * box.height() ||
            std::max(top->length(), bottom->length()) > .45 * box.height())
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
        auto diagnostic = sub.letterDiagnostics[last].toObject();
        diagnostic.insert("boundaryKind", "separate-contours");
        sub.letterDiagnostics[last] = diagnostic;
        sub.templateTrace.append(terminal + " · detached diagonal retained in alef");
    }
}
