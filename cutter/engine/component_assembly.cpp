#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

static PageAnalysisResult componentPage(Apath shape, const QPainterPath& ink, const QString& letters) {
    PageAnalysisResult page;
    page.page.lines.resize(1);
    page.page.lines.front().words.resize(1);
    auto& word = page.page.lines.front().words.front();
    shape.path = ink;
    word.paths.append(std::move(shape));
    auto& component = word.wordResultInfo.subWords.emplace_back();
    component.paths = {0};
    component.text = letters;
    return page;
}

// The prefix outline can start on a different curve after removing the arm.
// Return cut landmarks to their original contour and curve time for inspection.
bool sourceLandmark(PathExtrema& mark, const std::vector<std::vector<QBezier>>& paths) {
    constexpr double tolerance = 1e-4;
    auto best = tolerance;
    int path = -1;
    double time = 0;
    for (int p = 0; p < int(paths.size()); ++p)
        for (int k = 0; k < int(paths[p].size()); ++k) {
            const auto& c = paths[p][k];
            const auto q = mark.point;
            if (q.x() < std::min({c.pt1().x(), c.pt2().x(), c.pt3().x(), c.pt4().x()}) - tolerance ||
                q.x() > std::max({c.pt1().x(), c.pt2().x(), c.pt3().x(), c.pt4().x()}) + tolerance ||
                q.y() < std::min({c.pt1().y(), c.pt2().y(), c.pt3().y(), c.pt4().y()}) - tolerance ||
                q.y() > std::max({c.pt1().y(), c.pt2().y(), c.pt3().y(), c.pt4().y()}) + tolerance)
                continue;
            const auto distance = [&](double t) { return QLineF(q, c.pointAt(t)).length(); };
            if (distance(0) < 1e-8) {
                mark.pathIndex = p;
                mark.time = k;
                return true;
            }
            if (distance(1) < 1e-8) {
                mark.pathIndex = p;
                mark.time = (k + 1) % paths[p].size();
                return true;
            }
            int closest = 0;
            for (int i = 1; i <= 16; ++i)
                if (distance(i / 16.) < distance(closest / 16.))
                    closest = i;
            double lo = std::max(0., (closest - 1) / 16.), hi = std::min(1., (closest + 1) / 16.);
            for (int i = 0; i < 35; ++i) {
                const auto t = (hi - lo) / 3.;
                if (distance(lo + t) < distance(hi - t))
                    hi -= t;
                else
                    lo += t;
            }
            const double t = (lo + hi) / 2, error = distance(t);
            if (error < best) {
                best = error;
                path = p;
                time = k + t;
            }
        }
    if (path < 0)
        return false;
    mark.pathIndex = path;
    mark.time = time;
    return true;
}

static bool restoreSourceCut(Stretching& cut, const std::vector<std::vector<QBezier>>& curves) {
    for (auto p : {&cut.topBaseLine, &cut.bottomBaseLine, &cut.topRight, &cut.bottomRight, &cut.topLeft, &cut.bottomLeft})
        if (!p->point.isNull() && !sourceLandmark(*p, curves))
            return false;
    return cut.counterEntry.pathIndex <= 0 ||
           (sourceLandmark(cut.counterEntry, curves) && sourceLandmark(cut.counterExit, curves));
}

bool isSeparateFoldedArm(const QPainterPath& body, const QPainterPath& arm, bool allowPointContact) {
    const auto box = arm.boundingRect();
    if (!box.intersects(body.boundingRect()))
        return true;
    // Qt flattens Beziers for path intersections. Resolve the tip at a consistent
    // scale so a chord through the bend of the bowl cannot create a false bridge.
    const double scale = 1024.0 / std::max(box.width(), box.height());
    QTransform detail;
    detail.scale(scale, scale).translate(-box.center().x(), -box.center().y());
    const auto bowl = detail.map(body);
    const auto diagonal = detail.map(arm);
    return allowPointContact ? bowl.intersected(diagonal).isEmpty() : !bowl.intersects(diagonal);
}

} // namespace MedinaCutter

// Separate a terminal arm at its root or use its existing detached contour.
bool PageAnalysisResult::recoverFoldedArm(Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config,
                                          bool attached) {
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 3 || sub.paths.empty())
        return false;
    const QChar previous = letters[letters.size() - 2];
    QJsonObject arm, mode, prefixForm;
    QString sourceTemplate, variant;
    for (const auto& value : config.value("medial").toArray()) {
        const auto form = value.toObject();
        if (!form.value("current").toString().contains(previous)) continue;
        const auto recipe = form.value("terminalArmRecovery").toObject();
        mode = recipe.value(attached ? "attached" : "detached").toObject();
        if (mode.isEmpty() || (recipe.contains("letters") &&
            !recipe.value("letters").toString().contains(letters.back()))) continue;
        const auto candidate = namedLetterForm(config, mode.value("form").toString());
        prefixForm = namedLetterForm(config, recipe.value("prefixForm").toString());
        if (!candidate.value("current").toString().contains(letters.back()) ||
            !prefixForm.value("current").toString().contains(previous)) continue;
        sourceTemplate = form.value("id").toString();
        variant = mode.value("variant").toString();
        arm = resolvedBodyForm(config, candidate);
        break;
    }
    if (arm.isEmpty())
        return false;
    auto shape = word.bodyShape(sub);
    auto toPage = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path);
        p.translate(shape.pos);
        return p;
    };
    QPainterPath connected, separate;
    int separateIndex = -1;
    std::vector<ComponentArmBody> componentBodies;
    Stretching seam{};
    std::vector<std::vector<QBezier>> curves;
    if (attached) {
        WordResultInfo::SubWordInfo dummy;
        SearchContext context{.transformedPath = toPage(shape.path), .subword = dummy};
        if (!readTerminalOutline(context) || !declaredArmRoot(context, arm, seam))
            return false;
        auto halves = cutRegions(shape.path, {}, {seam}, context.orientation, true);
        if (halves.size() != 2)
            return false;
        connected = halves[0].path;
        separate = halves[1].path;
        for (const auto& path : context.subPaths)
            curves.push_back(baseContourCurves(path));
    } else {
        WordResultInfo::SubWordInfo empty;
        SearchContext source{.transformedPath = toPage(shape.path), .subword = empty};
        if (!readTerminalOutline(source))
            return false;
        componentBodies =
            recognizeComponentArms(inkOutline(source), arm.value("body").toObject().value("arm").toObject());
        const auto paths = toSubpaths(shape.path);
        if (paths.size() < 2)
            return false;
        connected = paths[0];
        for (int i = 1; i < paths.size(); ++i) {
            if (paths[0].contains(paths[i]))
                connected.addPath(paths[i]);
            else if (separate.isEmpty() &&
                     isSeparateFoldedArm(paths[0], paths[i], config.contains("reconsiderFinalAttachments"))) {
                separate = paths[i];
                separateIndex = i;
            } else
                return false;
        }
    }
    if (separate.isEmpty())
        return false;
    connected.setFillRule(shape.path.fillRule());
    const auto component = toPage(separate);
    const auto armBodySettings = arm.value("body").toObject().value("arm").toObject();
    const auto armCuts = arm.value("cuts").toObject().value("arm").toObject();
    const double minRise = mode.value("minRise").toDouble(armBodySettings.value("minRise").toDouble(3));
    const double maxTipGap = mode.value("maxTipGap").toDouble(armCuts.value("maxTipGap").toDouble(.75));
    std::optional<ComponentArmBody> armBody;
    if (attached)
        armBody = recognizeArmEnvelope(component, minRise, false);
    else {
        const auto candidate = std::find_if(componentBodies.begin(), componentBodies.end(),
                                            [&](const auto &body) { return body.index == separateIndex; });
        if (candidate != componentBodies.end())
            armBody = *candidate;
    }
    if (!armBody)
        return false;
    const auto box = armBody->bounds;
    const auto tip =
        attached ? QLineF(seam.topBaseLine.point, seam.bottomBaseLine.point).center() : armBody->bottom.point;

    // Segment the prefix with its own final form and keep the original curves.
    auto prefix = componentPage(shape, connected, letters.chopped(1));
    auto& result = prefix.page.lines.front().words.front().wordResultInfo.subWords.front();
    auto prefixForms = config;
    const auto cutOverride = mode.value("prefixCuts").toObject();
    if (!cutOverride.isEmpty()) {
        auto rules = prefixForms.value("final").toArray();
        for (int i = 0; i < rules.size(); ++i) {
            auto form = rules[i].toObject();
            if (form.value("id") != prefixForm.value("id")) continue;
            form.insert("cuts", mergeShapeSettings(formDescription(prefixForms, form, "cuts"), cutOverride));
            rules[i] = form;
            break;
        }
        prefixForms.insert("final", rules);
    }
    bool prefixComplete;
    if (attached) {
        prefixForms["reconsiderForms"] =
            QJsonObject{{"initial", result.text}, {"medial", result.text}, {"final", result.text}};
        prefixComplete = prefix.segmentSubword(-2, 0, 0, 0, nullptr, true, &prefixForms);
    } else {
        OutlineSearchCache outlineCache;
        prefixComplete = prefix.segmentWithLetterForms(-2, 0, 0, 0, nullptr, prefixForms, outlineCache, true);
    }
    if (!prefixComplete || result.baseGlyphs.size() + 1 != size_t(letters.size()))
        return false;
    const auto prefixBody = toPage(result.baseGlyphs.back().path);
    QPainterPathStroker envelope;
    envelope.setWidth(2 * maxTipGap);
    if (!envelope.createStroke(prefixBody).contains(tip) || prefixBody.boundingRect().top() >= box.center().y() ||
        prefixBody.boundingRect().bottom() <= tip.y())
        return false;
    if (attached) {
        if (result.joins.empty())
            return false;
        const auto& lastJoin = *result.joins.rbegin();
        const auto entry = !result.kashidaPaths.empty() && !result.kashidaPaths.back().isEmpty()
                               ? result.kashidaCuts.back().second
                               : QLineF(lastJoin.topBaseLine.point, lastJoin.bottomBaseLine.point);
        // A connector entering above the folded arm would take part of the upright stem.
        if (entry.isNull() || entry.center().y() + entry.length() < tip.y())
            return false;
        Joins original;
        for (auto cut : result.joins) {
            if (!restoreSourceCut(cut, curves))
                return false;
            cut.readingOrder = int(original.size());
            original.insert(cut);
        }
        seam.readingOrder = int(original.size());
        original.insert(seam);
        result.joins = std::move(original);
        result.kashidaPaths.emplace_back();
        result.kashidaCuts.emplace_back();
    }
    sub.baseGlyphs = std::move(result.baseGlyphs);
    sub.baseGlyphs.push_back({letters.right(1), separate});
    sub.joins = std::move(result.joins);
    sub.kashidaCuts = std::move(result.kashidaCuts);
    sub.kashidaPaths = std::move(result.kashidaPaths);
    sub.letterDiagnostics = std::move(result.letterDiagnostics);
    auto stem = sub.letterDiagnostics.last().toObject();
    stem.insert("position", "medial");
    stem.insert("prefixTemplate", stem.value("template"));
    stem.insert("template", sourceTemplate);
    if (!variant.isEmpty()) stem.insert("variant", variant);
    stem.insert("candidates", QJsonArray{sourceTemplate});
    sub.letterDiagnostics[sub.letterDiagnostics.size() - 1] = stem;
    sub.letterDiagnostics.append(QJsonObject{{"letterIndex", letters.size()},
                                             {"position", "final"},
                                             {"character", letters.right(1)},
                                             {"status", "matched"},
                                             {"template", arm.value("id")},
                                             {"boundaryKind", attached ? "shared-folded-arm" : "separate-contours"},
                                             {"candidates", QJsonArray{arm.value("id")}}});
    if (attached)
        recordBoundaryTemplate(sub, sub.letterDiagnostics.size() - 2, false, arm.value("id").toString());
    sub.templateTrace = result.templateTrace;
    sub.templateTrace.append(sourceTemplate + " · prefix body recovered with its terminal arm");
    sub.templateTrace.append(arm.value("id").toString() +
                             (attached ? " · sloping arm cut at its root" : " · separate sloping arm"));
    sub.correct = true;
    return true;
}

namespace MedinaCutter {

// Isolate an established entry seam, then recognize the exposed letter by its
// own outline. Accept the reconstruction only if every suffix letter fits.
bool assembleSplitEntry(const Word& word, WordResultInfo::SubWordInfo& sub,
                               const QJsonObject& config, const QJsonObject& checks) {
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 3 || sub.paths.empty() || sub.letterDiagnostics.size() != letters.size())
        return false;
    const auto initial = sub.letterDiagnostics.first().toObject();
    const bool knownEntry = initial.value("status") == "matched" && !sub.joins.empty() &&
                            sub.joins.begin()->cutSegments().size() == 1 &&
                            (sub.kashidaPaths.empty() || sub.kashidaPaths[0].isEmpty()) &&
                            sub.letterDiagnostics[1].toObject().value("status") != "matched";
    QJsonObject head, initialForm;
    for (const auto& value : config.value("medial").toArray()) {
        const auto rule = value.toObject();
        if (!rule.value("current").toString().contains(letters[1]))
            continue;
        const auto assembly = rule.value("assembly").toObject();
        if (assembly.value("operation") != "exposed-head")
            continue;
        const auto entry = assembly.value("entry").toObject();
        if (knownEntry && entry.value("source") == "matched-boundary") {
            head = rule;
            break;
        }
        if (entry.value("source") != "form")
            continue;
        for (const auto& item : config.value("initial").toArray()) {
            const auto form = item.toObject();
            if (form.value("id") == entry.value("form") && form.value("current").toString().contains(letters[0]) &&
                (!form.contains("ligatureWith") || hasFollowingLetter(letters, 0, form.value("ligatureWith").toString()))) {
                initialForm = bodyConstraintForm(config, form);
                head = rule;
                break;
            }
        }
        if (!head.isEmpty()) break;
    }
    if (head.isEmpty()) return false;
    const auto shape = word.bodyShape(sub);
    WordResultInfo::SubWordInfo dummy{};
    OutlineSearchCache cache;
    SearchContext ctx{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                      .subword = dummy, .baseText = letters, .outlineCache = &cache};
    if (!readTerminalOutline(ctx)) return false;
    ctx.nextIter = std::next(ctx.xSorted.begin());
    Stretching cut{};
    if (initialForm.isEmpty())
        cut = *sub.joins.begin();
    else {
        if (!proposeTemplateBoundary(ctx, config, initialForm, false, false, nullptr) ||
            !matchesBoundaryBody(ctx, initialForm, ctx.res, true)) return false;
        cut = ctx.res;
    }
    const auto parts = cutRegions(shape.path, {}, {cut}, ctx.orientation, true);
    if (parts.size() != 2) return false;

    auto suffix = componentPage(shape, parts.back().path, letters.mid(1));
    auto& result = suffix.page.lines.front().words.front().wordResultInfo.subWords.front();
    if (!suffix.segmentSubword(-2, 0, 0, 0, nullptr, true, &config) ||
        result.baseGlyphs.size() + 1 != size_t(letters.size()) ||
        !head.value("assembly")
             .toObject()
             .value("bodyForms")
             .toArray()
             .contains(result.letterDiagnostics.first().toObject().value("template")))
        return false;
    auto candidate = sub;
    candidate.baseGlyphs = std::move(result.baseGlyphs);
    candidate.baseGlyphs.insert(candidate.baseGlyphs.begin(), {letters.left(1), parts.front().path});
    candidate.kashidaPaths = std::move(result.kashidaPaths);
    candidate.kashidaPaths.emplace(candidate.kashidaPaths.begin());
    candidate.kashidaCuts = std::move(result.kashidaCuts);
    candidate.kashidaCuts.emplace(candidate.kashidaCuts.begin());
    candidate.joins.clear();
    cut.readingOrder = 0; cut.fixedLeftBoundary = cut.fixedRightBoundary = true;
    candidate.joins.insert(cut);
    std::vector<std::vector<QBezier>> curves;
    for (const auto& outline : ctx.subPaths) curves.push_back(baseContourCurves(outline));
    for (auto j : result.joins) {
        if (!restoreSourceCut(j, curves)) return false;
        j.readingOrder = int(candidate.joins.size());
        candidate.joins.insert(j);
    }
    const auto initialId = initialForm.isEmpty() ? initial.value("template").toString() : initialForm.value("id").toString();
    const auto headId = head.value("id").toString();
    candidate.letterDiagnostics = {};
    auto first = initial;
    first.insert("status", "matched");
    first.insert("template", initialId);
    auto candidates = first.value("candidates").toArray();
    if (!candidates.contains(initialId)) candidates.append(initialId);
    first.insert("candidates", candidates);
    candidate.letterDiagnostics.append(first);
    for (int i = 0; i < result.letterDiagnostics.size(); ++i) {
        auto d = result.letterDiagnostics[i].toObject();
        d.insert("letterIndex",i+2);
        if (i == 0) {
            d.insert("position","medial"); d.insert("exitTemplate",d.value("template"));
            d.insert("template",headId); d.insert("candidates",QJsonArray{headId});
        }
        candidate.letterDiagnostics.append(d);
    }
    const auto exitSource = initialForm.isEmpty() ? first.value("exitTemplate").toString(initialId) : initialId;
    recordBoundaryTemplate(candidate, 0, false, exitSource);
    recordBoundaryTemplate(candidate, 1, true, exitSource);
    candidate.templateTrace = result.templateTrace;
    candidate.templateTrace.prepend(headId + " · exposed head at shared cut");
    candidate.templateTrace.prepend(initialId + (initialForm.isEmpty() ? " · existing exit retained" :
                                                                       " · declared exit at shared cut"));
    if (!plausibleRecoveredBodies(word,candidate,checks) || !coversOriginalInk(word,candidate)) return false;
    candidate.correct = true;
    sub = std::move(candidate);
    return true;
}

// In some overhanging heads the pen lifts between the incoming attachment
// and the head, even though the transcript remains joined. Recover the
// attachment from its own contour and the head from the other contour.
bool detachedHeadEntry(Word& word, WordResultInfo::SubWordInfo& result, const QJsonObject& config) {
    const auto letters = PageAnalysisResult::bodyLetters(result.text);
    if (letters.size() < 4 || result.paths.empty())
        return false;
    QJsonObject form;
    QJsonArray lifted;
    for (const auto& value : config.value("medial").toArray()) {
        const auto rule = bodyConstraintForm(config, value.toObject());
        if (rule.contains("overhangingEntry") && form.isEmpty()) form = rule;
        if (rule.contains("penLift")) lifted.append(rule);
    }
    if (form.isEmpty() && lifted.isEmpty()) return false;
    const auto liftFor = [&](QChar letter, const char* side) {
        for (const auto& value : lifted) {
            const auto rule = value.toObject();
            if (rule.value("current").toString().contains(letter) &&
                rule.value("penLift").toObject().value("side") == side) return rule;
        }
        return QJsonObject{};
    };
    const auto shape = word.bodyShape(result);
    using Component = InkComponentBody;
    auto pieces = recognizeSourceComponents(shape.path);
    if (pieces.empty())
        return false;
    auto world = [&](const QPainterPath& p) {
        auto q = shape.transform.map(p);
        q.translate(shape.pos);
        return q;
    };
    if (world(pieces[0].ink).boundingRect().center().x() < world(pieces[1].ink).boundingRect().center().x())
        std::swap(pieces[0], pieces[1]);
    const auto settings = form.value("overhangingEntry").toObject();
    QPainterPathStroker contact;
    contact.setWidth(2 * settings.value("maxGap").toDouble(.8));
    if (!contact.createStroke(world(pieces[0].ink)).intersects(world(pieces[1].ink)))
        return false;
    auto fit = [&](const Component& component, const QString& text, const QJsonObject& forms,
                   bool penLiftAtEnd = false) {
        auto analysis = componentPage(shape, component.ink, text);
        auto& localWord = analysis.page.lines.front().words.front();
        auto& local = localWord.wordResultInfo.subWords.front();
        analysis.segmentSubword(-2, 0, 0, 0, nullptr, true, &forms);
        if (penLiftAtEnd && local.correct) {
            auto refined = local;
            if (verifyBodyLandmarkOwnership(localWord, refined, forms, true)) local = std::move(refined);
        }
        return std::move(local);
    };
    std::optional<WordResultInfo::SubWordInfo> accepted;
    for (int slot = 2; slot + 1 < letters.size(); ++slot) {
        const auto exit = liftFor(letters[slot - 1], "exit"), entry = liftFor(letters[slot], "entry");
        const bool separate = !exit.isEmpty() && !entry.isEmpty();
        if (!separate && !form.value("current").toString().contains(letters[slot])) continue;
        const int prefixSize = separate ? slot : slot + 1;
        auto prefixForms = config;
        if (separate) {
            // This prefix ends at a physical pen lift within the word.
            const auto allowed = exit.value("penLift").toObject().value("forms").toArray();
            auto finals = config.value("final").toArray();
            for (qsizetype i = 0; i < finals.size(); ++i) {
                auto rule = finals[i].toObject();
                if (!allowed.contains(rule.value("id")) ||
                    formDescription(config, rule, "body").value("shape") != "tooth-cluster") continue;
                auto cuts = formDescription(config, rule, "cuts");
                auto cluster = cuts.value("cluster").toObject();
                cluster.insert("_penLiftExit", true);
                cuts.insert("cluster", cluster);
                rule.insert("cuts", cuts);
                finals[i] = rule;
            }
            prefixForms.insert("final", finals);
        }
        auto prefix = fit(pieces[0], letters.left(prefixSize), prefixForms, separate);
        auto suffix = fit(pieces[1], letters.mid(slot), config);
        if (!suffix.correct ||
            prefix.baseGlyphs.size() != size_t(prefixSize) || suffix.baseGlyphs.size() != size_t(letters.size() - slot) ||
            prefix.joins.size() != size_t(prefixSize - 1) || suffix.joins.size() + 1 != suffix.baseGlyphs.size() ||
            prefix.letterDiagnostics.size() != prefixSize || suffix.letterDiagnostics.size() != letters.size() - slot)
            continue;
        if (std::any_of(prefix.letterDiagnostics.constBegin(), prefix.letterDiagnostics.constEnd() - 1,
                        [](const auto& v) { return v.toObject().value("status") != "matched"; })) continue;
        const auto attachment = world(prefix.baseGlyphs.back().path), head = world(suffix.baseGlyphs.front().path);
        if (!contact.createStroke(attachment).intersects(head)) continue;
        if (separate) {
            const auto accepts = [](const QJsonObject& rule, const QJsonValue& letter) {
                return rule.value("penLift").toObject().value("forms").toArray().contains(letter.toObject().value("template"));
            };
            if (!prefix.correct || !accepts(exit, prefix.letterDiagnostics.last()) ||
                !accepts(entry, suffix.letterDiagnostics.first())) continue;
        } else {
            const auto box = attachment.boundingRect();
            if (toSubpaths(attachment).size() != 1 || box.width() <= box.height() ||
                box.width() > settings.value("maxEntryToHead").toDouble() * head.boundingRect().width()) continue;
        }
        auto assembled = result;
        assembled.baseGlyphs = prefix.baseGlyphs;
        assembled.letterDiagnostics = prefix.letterDiagnostics;
        if (separate) {
            auto d = assembled.letterDiagnostics.last().toObject();
            d.insert("componentTemplate", d.value("template"));
            d.insert("template", exit.value("id"));
            d.insert("candidates", QJsonArray{exit.value("id")});
            d.insert("position", "medial");
            assembled.letterDiagnostics[prefixSize - 1] = d;
            assembled.baseGlyphs.insert(assembled.baseGlyphs.end(), suffix.baseGlyphs.begin(), suffix.baseGlyphs.end());
        } else {
            assembled.baseGlyphs.back().path.addPath(suffix.baseGlyphs.front().path);
            assembled.baseGlyphs.back().path.setFillRule(shape.path.fillRule());
            assembled.baseGlyphs.insert(assembled.baseGlyphs.end(), suffix.baseGlyphs.begin() + 1, suffix.baseGlyphs.end());
            assembled.letterDiagnostics.removeLast();
        }
        for (int j = 0; j < suffix.letterDiagnostics.size(); ++j) {
            auto letter = suffix.letterDiagnostics[j].toObject();
            letter.insert("letterIndex", slot + j + 1);
            if (j == 0) {
                const auto id = (separate ? entry : form).value("id");
                letter.insert("headTemplate", letter.value("template"));
                letter.insert("template", id);
                letter.insert("position", "medial");
                letter.insert("candidates", QJsonArray{id});
                if (separate) letter.insert("boundaryKind", "separate-contours");
            }
            assembled.letterDiagnostics.append(letter);
        }
        assembled.joins.clear();
        assembled.kashidaPaths.clear();
        assembled.kashidaCuts.clear();
        const auto append = [&](const auto& run, const Component& piece) {
            for (auto cut : run.joins) {
                const auto mapIndex = [&](PathExtrema& p) {
                    if (p.pathIndex >= 0 && p.pathIndex < int(piece.indices.size()))
                        p.pathIndex = piece.indices[p.pathIndex];
                };
                for (auto p : {&cut.topBaseLine, &cut.topRight, &cut.topLeft, &cut.bottomBaseLine, &cut.bottomLeft,
                               &cut.bottomRight})
                    mapIndex(*p);
                if (cut.counterEntry.pathIndex > 0) {
                    mapIndex(cut.counterEntry);
                    mapIndex(cut.counterExit);
                }
                cut.readingOrder = int(assembled.joins.size());
                assembled.joins.insert(cut);
            }
            assembled.kashidaPaths.insert(assembled.kashidaPaths.end(), run.kashidaPaths.begin(),
                                          run.kashidaPaths.end());
            assembled.kashidaCuts.insert(assembled.kashidaCuts.end(), run.kashidaCuts.begin(),
                                         run.kashidaCuts.end());
        };
        append(prefix, pieces[0]);
        append(suffix, pieces[1]);
        assembled.templateTrace.clear();
        for (const auto& d : assembled.letterDiagnostics) {
            const auto v = d.toObject();
            assembled.templateTrace << v.value("character").toString() + " · " + v.value("template").toString() +
                                           " · matched";
        }
        assembled.templateTrace << (separate ? QStringLiteral("Pen lift between two independently matched bodies") :
                                     form.value("id").toString() + " · incoming stroke and overhanging head share one letter");
        assembled.correct = true;
        if (!plausibleRecoveredBodies(word, assembled, config.value("recoveryChecks").toObject()) ||
            !coversOriginalInk(word, assembled))
            continue;
        if (accepted)
            return false;
        accepted = std::move(assembled);
    }
    if (!accepted)
        return false;
    result = std::move(*accepted);
    return true;
}

} // namespace MedinaCutter
