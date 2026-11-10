#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

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
    const auto findRule = [&](const char* phase, const char* feature, QChar ch) {
        for (const auto& value : config.value(phase).toArray()) {
            const auto form = value.toObject();
            if (!form.value("current").toString().contains(ch)) continue;
            const auto rule = bodyConstraintForm(config, form);
            if (rule.contains(feature) || formDescription(config, rule, "body").contains(feature))
                return rule;
        }
        return QJsonObject{};
    };
    const QChar previous = letters[letters.size() - 2];
    const auto legacy = findRule("medial", attached ? "terminalArm" : "detachedExit", previous);
    QJsonObject arm, mode, prefixForm;
    QString sourceTemplate, variant;
    if (legacy.isEmpty()) {
        for (const auto& value : config.value("medial").toArray()) {
            const auto form = value.toObject();
            if (!form.value("current").toString().contains(previous)) continue;
            const auto recipe = terminalArmRecipe(config, form);
            mode = recipe.value(attached ? "attached" : "detached").toObject();
            if (mode.isEmpty() || (recipe.contains("letters") &&
                !recipe.value("letters").toString().contains(letters.back()))) continue;
            auto candidate = mode.contains("form") ? namedLetterForm(config, mode.value("form").toString())
                                                   : findRule("final", "detachedArm", letters.back());
            prefixForm = namedLetterForm(config, recipe.value("prefixForm").toString());
            if (!candidate.value("current").toString().contains(letters.back()) ||
                !prefixForm.value("current").toString().contains(previous)) continue;
            sourceTemplate = form.value("id").toString();
            variant = mode.value("variant").toString();
            arm = resolvedBodyForm(config, candidate);
            break;
        }
    } else {
        const auto body = formDescription(config, legacy, "body");
        if (attached) {
            const auto candidate = namedLetterForm(config, body.value("terminalArm").toString());
            if (candidate.value("body").toObject().value("shape") == "tapered-arm" &&
                candidate.value("current").toString().contains(letters.back()))
                arm = candidate;
        } else if (body.value("detachedExit").toString().contains(letters.back())) {
            arm = findRule("final", "detachedArm", letters.back());
        }
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
        auto ink = toPage(shape.path);
        ink.setFillRule(Qt::WindingFill);
        SearchContext context{.transformedPath = ink, .subword = dummy};
        context.bbox = ink.boundingRect();
        QVector<PathExtrema> extrema;
        ShapeItem::getExtrema(ink, context.subPaths, extrema);
        for (const auto& p : extrema)
            if (p.pathIndex == 0) {
                context.timeSorted.insert(p);
                context.xSorted.insert(p);
                context.ySorted.insert(p);
            }
        context.setOrientation();
        if (!declaredArmRoot(context, arm, seam))
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
    const auto legacySettings = arm.value("detachedArm").toObject();
    const double minRise = sourceTemplate.isEmpty() ? legacySettings.value("minRise").toDouble(3)
        : mode.value("minRise").toDouble(armBodySettings.value("minRise").toDouble(3));
    const double maxTipGap = sourceTemplate.isEmpty() ? legacySettings.value("maxTipGap").toDouble(.75)
        : mode.value("maxTipGap").toDouble(armCuts.value("maxTipGap").toDouble(.75));
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
    PageAnalysisResult prefix;
    prefix.page.lines.resize(1);
    prefix.page.lines[0].words.resize(1);
    auto& part = prefix.page.lines[0].words[0];
    shape.path = connected;
    part.paths.append(shape);
    auto& result = part.wordResultInfo.subWords.emplace_back();
    result.paths = {0};
    result.text = letters.chopped(1);
    auto prefixForms = config;
    const auto legacyBody = formDescription(config, legacy, "body");
    const auto legacyCuts = formDescription(config, legacy, "cuts");
    if (legacyBody.value("shape") == "detached-return") {
        auto rules = prefixForms.value("final").toArray();
        auto body = legacyBody;
        body.insert("shape", "terminal-return");
        body.insert("counters", false);
        auto cuts = legacyCuts;
        cuts.insert("beforeFinal", false);
        cuts.insert("boundaries", QJsonObject{{"right", "fork-normal"}, {"left", "free"}});
        rules.append(QJsonObject{{"id", legacy.value("id")}, {"current", legacy.value("current")},
            {"ownsLeftTip", true}, {"body", body}, {"cuts", cuts}});
        prefixForms.insert("final", rules);
    }
    const auto sectionOverride = mode.value("prefixSection").toObject();
    if (!sectionOverride.isEmpty() || (attached && sourceTemplate.isEmpty())) {
        auto rules = prefixForms.value("final").toArray();
        for (int i = 0; i < rules.size(); ++i) {
            auto form = rules[i].toObject();
            const bool matches = sourceTemplate.isEmpty() ?
                formDescription(prefixForms, form, "body").value("shape") == "terminal-return" &&
                    form.value("current").toString().contains(previous)
                : form.value("id") == prefixForm.value("id");
            if (!matches) continue;
            auto cuts = formDescription(prefixForms, form, "cuts");
            const auto overrides = sourceTemplate.isEmpty() ?
                QJsonObject{{"turnAngle", legacyCuts.value("entryTurnAngle")}} : sectionOverride;
            cuts.insert("section", withOverrides(cuts.value("section").toObject(), overrides));
            form.insert("cuts", cuts);
            rules[i] = form;
        }
        prefixForms.insert("final", rules);
    }
    bool prefixComplete;
    if (attached) {
        prefixForms["reconsiderForms"] =
            QJsonObject{{"initial", result.text}, {"medial", result.text}, {"final", result.text}};
        prefixComplete = prefix.segmentSubword(-2, 0, 0, 0, nullptr, true, &prefixForms);
    } else {
        prefixComplete = prefix.segmentWithLetterForms(-2, 0, 0, 0, nullptr, prefixForms, true);
    }
    if (!prefixComplete || result.provisional() || result.baseGlyphs.size() + 1 != size_t(letters.size()))
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
    const auto selected = sourceTemplate.isEmpty() ? legacy.value("id").toString() : sourceTemplate;
    stem.insert("template", selected);
    if (!variant.isEmpty()) stem.insert("variant", variant);
    stem.insert("candidates", QJsonArray{selected});
    sub.letterDiagnostics[sub.letterDiagnostics.size() - 1] = stem;
    sub.letterDiagnostics.append(QJsonObject{{"letterIndex", letters.size()},
                                             {"position", "final"},
                                             {"character", letters.right(1)},
                                             {"status", "matched"},
                                             {"template", arm.value("id")},
                                             {"boundaryKind", attached ? "shared-folded-arm" : "separate-contours"},
                                             {"candidates", QJsonArray{arm.value("id")}}});
    sub.templateTrace = result.templateTrace;
    sub.templateTrace.append(selected + " · prefix body recovered with its terminal arm");
    sub.templateTrace.append(arm.value("id").toString() +
                             (attached ? " · sloping arm cut at its root" : " · separate sloping arm"));
    sub.matchedTemplates = result.matchedTemplates;
    sub.matchedTemplates << selected << arm.value("id").toString();
    sub.matchedTemplates.removeDuplicates();
    sub.correct = true;
    return true;
}

namespace MedinaCutter {

// Isolate an established entry seam, then recognize the exposed letter by its
// own outline. Accept the reconstruction only if every suffix letter fits.
bool recoverExposedHead(const Word& word, WordResultInfo::SubWordInfo& sub,
                               const QJsonObject& config, const QJsonObject& checks) {
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 3 || sub.paths.empty() || sub.letterDiagnostics.size() != letters.size())
        return false;
    const auto initial = sub.letterDiagnostics.first().toObject();
    const bool knownEntry = initial.value("status") == "matched" && !sub.joins.empty() &&
                            sub.joins.begin()->cutSegments().size() == 1 &&
                            (sub.kashidaPaths.empty() || sub.kashidaPaths[0].isEmpty()) &&
                            sub.letterDiagnostics[1].toObject().value("status") != "matched";
    QJsonObject head, stem;
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
                form.value("ligatureWith").toString().contains(letters[1]) &&
                descendingStemDescription(config, form).contains("headLip")) {
                stem = form;
                head = rule;
                break;
            }
        }
        if (!head.isEmpty()) break;
    }
    if (head.isEmpty()) return false;
    auto shape = word.bodyShape(sub);
    auto ink = shape.transform.map(shape.path).translated(shape.pos);
    ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy{};
    SearchContext ctx{.transformedPath = ink, .subword = dummy};
    ctx.bbox = ink.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(ink, ctx.subPaths, extrema);
    for (const auto& e : extrema) {
        if (e.pathIndex != 0) continue;
        ctx.timeSorted.insert(e);
        ctx.xSorted.insert(e);
        ctx.ySorted.insert(e);
    }
    if (ctx.timeSorted.size() < 3) return false;
    ctx.setOrientation();
    Stretching cut{};
    if (stem.isEmpty())
        cut = *sub.joins.begin();
    else {
        if (!declaredLamBoundary(ctx, config, stem)) return false;
        cut = ctx.res;
    }
    const auto parts = cutRegions(shape.path, {}, {cut}, ctx.orientation, true);
    if (parts.size() != 2) return false;

    PageAnalysisResult suffix;
    suffix.page.lines.resize(1); suffix.page.lines[0].words.resize(1);
    auto& tail = suffix.page.lines[0].words[0];
    shape.path = parts.back().path;
    tail.paths.append(shape);
    auto& result = tail.wordResultInfo.subWords.emplace_back();
    result.paths = {0}; result.text = letters.mid(1);
    if (!suffix.segmentSubword(-2, 0, 0, 0, nullptr, true, &config) || result.provisional() ||
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
    const auto initialId = stem.isEmpty() ? initial.value("template").toString() : stem.value("id").toString();
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
    candidate.matchedTemplates = result.matchedTemplates;
    candidate.matchedTemplates.prepend(headId); candidate.matchedTemplates.prepend(initialId);
    candidate.templateTrace = result.templateTrace;
    candidate.templateTrace.prepend(headId + " · exposed head at shared cut");
    candidate.templateTrace.prepend(initialId + (stem.isEmpty() ? " · existing exit retained" :
                                                                " · shaft ends at projecting head"));
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
    auto fit = [&](const Component& component, const QString& text) {
        PageAnalysisResult analysis;
        analysis.page.lines.resize(1);
        analysis.page.lines.front().words.resize(1);
        auto& localWord = analysis.page.lines.front().words.front();
        auto localShape = shape;
        localShape.path = component.ink;
        localWord.paths.append(localShape);
        auto& local = localWord.wordResultInfo.subWords.emplace_back();
        local.paths = {0};
        local.text = text;
        analysis.segmentSubword(-2, 0, 0, 0, nullptr, true, &config);
        return std::move(local);
    };
    std::optional<WordResultInfo::SubWordInfo> accepted;
    for (int slot = 2; slot + 1 < letters.size(); ++slot) {
        const auto exit = liftFor(letters[slot - 1], "exit"), entry = liftFor(letters[slot], "entry");
        const bool separate = !exit.isEmpty() && !entry.isEmpty();
        if (!separate && !form.value("current").toString().contains(letters[slot])) continue;
        const int prefixSize = separate ? slot : slot + 1;
        auto prefix = fit(pieces[0], letters.left(prefixSize)), suffix = fit(pieces[1], letters.mid(slot));
        if (!suffix.correct || prefix.provisional() || suffix.provisional() ||
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
        assembled.matchedTemplates.clear();
        for (const auto& d : assembled.letterDiagnostics)
            assembled.matchedTemplates << d.toObject().value("template").toString();
        assembled.matchedTemplates.removeDuplicates();
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

// Recover a contour match that is sensitive to pen scale only when the same templates
// find the same ink boundaries on both sides of the native scale.
bool stableTemplateCuts(const Word& word, WordResultInfo::SubWordInfo& sub, QJsonObject config,
                               const QJsonArray& shapeFrames) {
    const auto settings = config.value("reconsiderScale").toObject();
    if (settings.isEmpty())
        return false;
    config.remove("reconsiderScale"); // Recheck each scale once.
    const auto shape = word.bodyShape(sub);
    double width = 1, slant = 0, frameAngle = 0;
    auto trial = [&](double scale, double angle, WordResultInfo::SubWordInfo& candidate) {
        PageAnalysisResult page;
        page.page.lines.resize(1);
        page.page.lines.front().words.resize(1);
        auto& item = page.page.lines.front().words.front();
        auto scaled = shape;
        QTransform affine = QTransform::fromScale(scale * width, scale);
        affine.shear(slant, 0);
        affine.rotate(angle + frameAngle);
        scaled.transform *= affine;
        item.paths.append(scaled);
        auto& result = item.wordResultInfo.subWords.emplace_back();
        result.paths = {0};
        result.text = sub.text;
        if (!page.segmentSubword(-2, 0, 0, 0, nullptr, true, &config) || result.provisional())
            return false;
        // Masks are stored in the original path coordinates. Only inspection
        // landmarks, measured distances, and connector lines use page units.
        const auto inverse = affine.inverted();
        auto point = [&](QPointF p) {
            const auto delta = p - shape.pos;
            return (width == 1 && slant == 0 && angle + frameAngle == 0 ? delta / scale : inverse.map(delta)) + shape.pos;
        };
        auto line = [&](QLineF l) { return QLineF(point(l.p1()), point(l.p2())); };
        Joins restored;
        for (auto cut : result.joins) {
            const auto normal = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
            const double unit = normal.length() > 0 && (width != 1 || slant != 0) ?
                line(normal).length() / normal.length() : 1 / scale;
            for (auto* p : {&cut.topBaseLine, &cut.bottomBaseLine, &cut.topLeft, &cut.bottomLeft, &cut.topRight,
                            &cut.bottomRight}) {
                if (!p->point.isNull())
                    p->point = point(p->point);
            }
            if (cut.counterEntry.pathIndex > 0) {
                cut.counterEntry.point = point(cut.counterEntry.point);
                cut.counterExit.point = point(cut.counterExit.point);
            }
            cut.length *= unit;
            cut.referenceWidth *= unit;
            cut.topEndpointTolerance *= unit;
            restored.insert(cut);
        }
        result.joins = std::move(restored);
        for (auto& [a, b] : result.kashidaCuts) {
            if (!a.isNull())
                a = line(a);
            if (!b.isNull())
                b = line(b);
        }
        for (auto& mark : result.extrema)
            mark.point = point(mark.point);
        result.debugPath = {}; // Temporary search overlays use the trial coordinate system.
        candidate = std::move(result);
        return true;
    };
    const auto sameBoundaries = [&](const auto& lower, const auto& upper) {
        if (lower.matchedTemplates != upper.matchedTemplates || lower.joins.size() != upper.joins.size() ||
            lower.kashidaPaths.size() != upper.kashidaPaths.size() ||
            lower.baseGlyphs.size() != upper.baseGlyphs.size())
            return false;
        const double tol = settings.value("boundaryTolerance").toDouble();
        auto close = [&](QLineF a, QLineF b) {
            return QLineF(a.p1(), b.p1()).length() <= tol && QLineF(a.p2(), b.p2()).length() <= tol;
        };
        auto b = upper.joins.begin();
        for (const auto& a : lower.joins) {
            auto first = a.cutSegments(), second = b->cutSegments();
            if (first.size() != second.size())
                return false;
            for (size_t i = 0; i < first.size(); ++i)
                if (!close(first[i], second[i]))
                    return false;
            ++b;
        }
        for (size_t i = 0; i < lower.kashidaPaths.size(); ++i) {
            if (lower.kashidaPaths[i].isEmpty() != upper.kashidaPaths[i].isEmpty())
                return false;
            if (!lower.kashidaPaths[i].isEmpty() && (!close(lower.kashidaCuts[i].first, upper.kashidaCuts[i].first) ||
                                                     !close(lower.kashidaCuts[i].second, upper.kashidaCuts[i].second)))
                return false;
        }
        return true;
    };
    const auto stableAt = [&](double low, double high, double angleLow = 0, double angleHigh = 0) {
        WordResultInfo::SubWordInfo lower{}, upper{};
        if (!trial(low, angleLow, lower) || !trial(high, angleHigh, upper) || !sameBoundaries(lower, upper))
            return false;
        lower.paths = sub.paths;
        lower.secondaryBodyPath = sub.secondaryBodyPath;
        lower.bodyCounter = sub.bodyCounter;
        if (!plausibleRecoveredBodies(word, lower, config.value("recoveryChecks").toObject()) ||
            !coversOriginalInk(word, lower))
            return false;
        if (!shapeFrames.isEmpty())
            lower.templateTrace.append(QStringLiteral("Contour cuts agree at 99% and 101% scale with width %1, slant %2 and tilt %3°")
                                       .arg(width).arg(slant).arg(frameAngle));
        else if (angleLow != 0 || angleHigh != 0)
            lower.templateTrace.append(
                QStringLiteral("Same contour attachments at %1° and %2° tilt").arg(angleLow).arg(angleHigh));
        else
            lower.templateTrace.append(QStringLiteral("Same template boundaries at %1% and %2% scale")
                                           .arg(qRound(100 * low))
                                           .arg(qRound(100 * high)));
        sub = std::move(lower);
        return true;
    };
    if (!shapeFrames.isEmpty()) {
        for (const auto& frame : shapeFrames) {
            const auto pair = frame.toArray();
            width = pair[0].toDouble();
            slant = pair[1].toDouble();
            frameAngle = pair.size() == 3 ? pair[2].toDouble() : 0;
            if (stableAt(.99, 1.01)) return true;
        }
        return false;
    }
    const double step = settings.value("fraction").toDouble();
    if (stableAt(1 - step, 1 + step))
        return true;
    for (const auto& value : settings.value("alternateScales").toArray()) {
        const double center = value.toDouble();
        if (stableAt(center - step / 2, center + step / 2))
            return true;
    }
    for (const auto& value : settings.value("tilts").toArray()) {
        const double tilt = value.toDouble();
        if (stableAt(1, 1, tilt - .8, tilt + .8))
            return true;
    }
    return false;
}

// Confirm an existing boundary from the candidate body's own counter(s).
// Both attachments of a medial form must independently land on its rim.
static bool matchesCounterOutline(SearchContext &source, const QPainterPath &body, const QJsonObject &rule,
                                  const QLineF &entry, const QLineF &exit) {
    const auto contours = toSubpaths(body);
    const auto spec = counterOutlineSettings(rule);
    if (contours.size() != spec.value("counters").toInt() + 1 || contours.size() < 2)
        return false;
    QRectF eye;
    for (int i = 1; i < contours.size(); ++i) {
        if (!contours[0].contains(contours[i]))
            return false;
        eye = eye.united(contours[i].boundingRect());
    }
    const auto box = body.boundingRect();
    const double h = eye.height();
    if (h <= .1 || eye.width() <= .1)
        return false;
    const auto inside = [](double value, QJsonArray interval) {
        return interval.size() == 2 && value >= interval[0].toDouble() && value <= interval[1].toDouble();
    };
    if (!inside(eye.width() / h, spec.value("aspect").toArray()))
        return false;
    if (rule.value("body").toObject().value("shape") == "counter-outline") {
        // Recognize counter groups in the uncut source. Assembly may retain
        // or reject one of them, but cannot create evidence by clipping a hole.
        const auto evidence = recognizedSharedShape(source, rule.value("body").toObject());
        const bool retained = std::any_of(evidence->outlines.begin(), evidence->outlines.end(), [&](const auto &group) {
            if (group.counters.size() + 1 != contours.size())
                return false;
            return std::all_of(group.counters.begin(), group.counters.end(), [&](const auto &hole) {
                return std::find(contours.begin() + 1, contours.end(), source.subPaths[hole.index]) != contours.end();
            });
        });
        if (!retained)
            return false;
    }
    const auto margins = spec.value("rim").toObject();
    if (!inside((eye.left() - box.left()) / h, margins.value("left").toArray()) ||
        !inside((eye.top() - box.top()) / h, margins.value("top").toArray()) ||
        !inside((box.right() - eye.right()) / h, margins.value("right").toArray()) ||
        !inside((box.bottom() - eye.bottom()) / h, margins.value("bottom").toArray()))
        return false;
    const auto attached = [&](const QLineF& cut, const QJsonObject& attachment) {
        if (attachment.isEmpty())
            return cut.isNull();
        if (cut.isNull())
            return false;
        const auto p = cut.center();
        return inside((p.x() - eye.left()) / eye.width(), attachment.value("x").toArray()) &&
               inside((p.y() - eye.top()) / h, attachment.value("y").toArray()) &&
               inside(cut.length() / h, attachment.value("width").toArray());
    };
    return attached(entry, spec.value("entry").toObject()) && attached(exit, spec.value("exit").toObject());
}

static void removeUnresolvedSlots(QStringList& trace, const QJsonArray& slotsBeforeRecovery) {
    for (const auto& value : slotsBeforeRecovery) {
        const auto slot = value.toObject();
        if (slot.value("status") != "matched")
            trace.removeAll(QString("%1 %2 · %3").arg(slot.value("position").toString(),
                            slot.value("character").toString(), slot.value("status").toString()));
    }
}

// Rebuild the last two masks together at the terminal letter's own attachment.
// The approach to the shared turn stays in the head stacked above it.
bool recoverSharedTerminal(const Word& word, WordResultInfo::SubWordInfo& sub,
                              const QJsonObject& config, const QJsonObject& checks) {
    const auto text = PageAnalysisResult::bodyLetters(sub.text);
    const int count = text.size();
    if (count < 2 || sub.paths.empty() || sub.provisional() || sub.letterDiagnostics.size() != count ||
        sub.baseGlyphs.size() != sub.joins.size() + 1 ||
        (sub.joins.size() != size_t(count - 1) && sub.joins.size() != size_t(count - 2)))
        return false;
    for (int i = 0; i + 2 < count; ++i)
        if (sub.letterDiagnostics[i].toObject().value("status") != "matched")
            return false;
    QJsonObject receiver;
    for (const auto& value : config.value("final").toArray()) {
        const auto rule = value.toObject();
        if ((rule.value("body").toObject().value("shape") == "folded-return" ||
             rule.value("body").toObject().value("shape") == "rising-return") &&
            rule.value("sharedBoundary").toBool() && rule.value("current").toString().contains(text.back())) {
            receiver = rule;
            break;
        }
    }
    if (receiver.isEmpty()) return false;
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    auto ink = mapped(shape.path);
    ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy{};
    SearchContext ctx{.transformedPath = ink, .subword = dummy};
    ctx.bbox = ink.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(ink, ctx.subPaths, extrema);
    for (const auto& e : extrema) if (e.pathIndex == 0) {
        ctx.timeSorted.insert(e); ctx.xSorted.insert(e); ctx.ySorted.insert(e);
    }
    if (ctx.timeSorted.size() < 3) return false;
    ctx.setOrientation();
    Stretching seam{};
    const bool folded = formDescription(config, receiver, "body").value("shape") == "folded-return";
    if (!declaredSharedBoundary(ctx, resolvedBodyForm(config, receiver)))
        return false;
    seam = ctx.res;
    const auto incoming = [&](int gap) -> std::optional<Stretching> {
        auto boundary = *std::next(sub.joins.begin(), gap);
        if (gap >= int(sub.kashidaPaths.size()) || sub.kashidaPaths[gap].isEmpty()) return boundary;
        if (gap >= int(sub.kashidaCuts.size())) return {};
        const auto edge = sub.kashidaCuts[gap].second;
        boundary = {};
        boundary.topBaseLine.point = edge.p1();
        boundary.bottomBaseLine.point = edge.p2();
        const std::vector<std::vector<QBezier>> outer{baseContourCurves(ctx.subPaths.front())};
        if (!sourceLandmark(boundary.topBaseLine, outer) || !sourceLandmark(boundary.bottomBaseLine, outer))
            return {};
        orientSection(ctx, boundary, seam.topBaseLine.time, true);
        return boundary;
    };
    // When the preceding mask enclosed the head's entire eye, its boundary
    // was on the wrong stroke. Recover the incoming edge from that eye as well.
    std::optional<Stretching> rim;
    if (count >= 3 && sub.letterDiagnostics[count - 2].toObject().value("status") != "matched") {
        const auto captured = toSubpaths(mapped(sub.baseGlyphs[count - 3].path));
        for (const auto& value : config.value("medial").toArray()) {
            const auto rule = value.toObject();
            if (!rule.contains("rimOffset") || rule.value("requiresExitTemplate") != receiver.value("id") ||
                !rule.value("current").toString().contains(text[count - 2])) continue;
            for (int i = 1; i < captured.size(); ++i) {
                const auto eye = captured[i].boundingRect();
                Stretching edge{};
                if (!medialChordAt(ctx, eye.right() + rule.value("rimOffset").toDouble() * eye.height(),
                                  eye.center().y(), 1.3 * eye.height(), edge)) continue;
                orientSection(ctx, edge, seam.topBaseLine.time, true);
                const auto parts = cutRegions(ink, {}, {edge, seam}, ctx.orientation, true);
                if (parts.size() != 3 || !matchesCounterOutline(ctx, parts[1].path, rule, edge.cutSegments().front(),
                                                                seam.cutSegments().front()))
                    continue;
                if (rim) return false;
                rim = edge;
            }
        }
    }
    std::vector<Stretching> cuts;
    QLineF entry;
    if (rim && count > 3) {
        const auto preceding = incoming(count - 4);
        if (!preceding) return false;
        cuts.push_back(*preceding);
    }
    if (count > 2) {
        const auto prior = rim ? rim : incoming(count - 3);
        if (!prior) return false;
        if (prior->cutSegments().size() == 1) entry = prior->cutSegments().front();
        cuts.push_back(*prior);
    }
    cuts.push_back(seam);
    const auto pieces = cutRegions(shape.path, {}, cuts, ctx.orientation, true);
    if (pieces.size() != cuts.size() + 1) return false;
    auto candidate = sub;
    candidate.baseGlyphs.resize(count);
    const auto& head = pieces[pieces.size() - 2].path;
    auto sender = candidate.letterDiagnostics[count - 2].toObject();
    if (sender.value("status") != "matched") {
        bool matched = false;
        for (const auto& value : config.value(sender.value("position").toString()).toArray()) {
            const auto rule = value.toObject();
            if (!hasCounterOutline(rule) || !rule.value("current").toString().contains(text[count - 2]) ||
                (rule.contains("requiresExitTemplate") && rule.value("requiresExitTemplate") != receiver.value("id")) ||
                !matchesCounterOutline(ctx, mapped(head), rule, entry, seam.cutSegments().front()))
                continue;
            auto names = sender.value("candidates").toArray();
            if (!names.contains(rule.value("id"))) names.append(rule.value("id"));
            sender.insert("candidates", names);
            sender.insert("template", rule.value("id"));
            sender.insert("status", "matched");
            sender.insert("matchMethod", "counter-outline");
            candidate.matchedTemplates.append(rule.value("id").toString());
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    candidate.baseGlyphs[count - 2].path = head;
    candidate.baseGlyphs.back().path = pieces.back().path;
    candidate.baseGlyphs.back().text = text.right(1);
    candidate.kashidaPaths.resize(count - 1);
    candidate.kashidaCuts.resize(count - 1);
    candidate.kashidaPaths.back() = {};
    candidate.kashidaCuts.back() = {};
    if (rim) {
        candidate.baseGlyphs[count - 3].path = pieces[pieces.size() - 3].path;
        candidate.kashidaPaths[count - 3] = {};
        candidate.kashidaCuts[count - 3] = {};
    }
    candidate.joins.clear();
    for (auto it = sub.joins.begin(); candidate.joins.size() < size_t(count - (rim ? 3 : 2)); ++it) {
        auto kept = *it;
        kept.readingOrder = int(candidate.joins.size());
        candidate.joins.insert(kept);
    }
    if (rim) { rim->readingOrder = count - 3; candidate.joins.insert(*rim); }
    seam.readingOrder = count - 2;
    candidate.joins.insert(seam);
    const auto id = receiver.value("id").toString();
    sender.insert("sharedExitTemplate", id);
    candidate.letterDiagnostics[count - 2] = sender;
    auto terminal = candidate.letterDiagnostics.last().toObject();
    auto names = terminal.value("candidates").toArray();
    if (!names.contains(id)) names.append(id);
    terminal.insert("candidates", names);
    terminal.insert("template", id);
    terminal.insert("status", "matched");
    candidate.letterDiagnostics[count - 1] = terminal;
    candidate.matchedTemplates.append(id);
    removeUnresolvedSlots(candidate.templateTrace, sub.letterDiagnostics);
    if (sub.letterDiagnostics[count - 2].toObject().value("status") != "matched")
        candidate.templateTrace.append(sender.value("template").toString() + " · counter and body attachments matched");
    candidate.templateTrace.append(id + (folded ? " · shared cut at first turn of bowl" :
                                                 " · shared cut at root of rising tail"));
    if (!plausibleRecoveredBodies(word, candidate, checks) || !coversOriginalInk(word, candidate))
        return false;
    candidate.correct = true;
    sub = std::move(candidate);
    return true;
}

bool confirmCounterOutlines(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config, const QJsonObject& recoveryChecks) {
    const auto text = PageAnalysisResult::bodyLetters(sub.text);
    if (sub.provisional() || text.size() < 2 || sub.baseGlyphs.size() != size_t(text.size()) ||
        sub.letterDiagnostics.size() != text.size() || sub.joins.size() + 1 != sub.baseGlyphs.size())
        return false;
    const auto shape = word.bodyShape(sub);
    WordResultInfo::SubWordInfo empty;
    OutlineSearchCache cache;
    SearchContext source{.transformedPath = shape.transform.map(shape.path).translated(shape.pos), .subword = empty};
    source.outlineCache = &cache;
    if (!readTerminalOutline(source))
        return false;
    std::vector<QLineF> incoming(text.size()), outgoing(text.size());
    size_t j = 0;
    for (const auto& join : sub.joins) {
        if (join.cutSegments().size() == 1)
            outgoing[j] = incoming[j + 1] = QLineF(join.topBaseLine.point, join.bottomBaseLine.point);
        if (j < sub.kashidaPaths.size() && !sub.kashidaPaths[j].isEmpty()) {
            if (j >= sub.kashidaCuts.size())
                return false;
            outgoing[j] = sub.kashidaCuts[j].first;
            incoming[j + 1] = sub.kashidaCuts[j].second;
        }
        ++j;
    }
    auto diagnostics = sub.letterDiagnostics;
    QStringList matched, trace;
    for (int i = 0; i < text.size(); ++i) {
        auto slot = diagnostics[i].toObject();
        if (slot.value("status") == "matched")
            continue;
        const auto pos = slot.value("position").toString();
        const auto mask = shape.transform.map(sub.baseGlyphs[i].path).translated(shape.pos);
        bool certified = false;
        for (const auto& value : config.value(pos).toArray()) {
            const auto form = value.toObject();
            if (!hasCounterOutline(form) || form.contains("requiresExitTemplate") ||
                !form.value("current").toString().contains(text[i]) ||
                !matchesCounterOutline(source, mask, form, incoming[i], outgoing[i]))
                continue;
            const auto id = form.value("id").toString();
            auto candidates = slot.value("candidates").toArray();
            if (!candidates.contains(id))
                candidates.append(id);
            slot.insert("candidates", candidates);
            slot.insert("status", "matched");
            slot.insert("template", id);
            slot.insert("matchMethod", "counter-outline");
            diagnostics[i] = slot;
            matched.append(id);
            trace.append(id + " · counter and body attachments matched");
            certified = true;
            break;
        }
        if (!certified)
            return false;
    }
    if (matched.isEmpty() || !plausibleRecoveredBodies(word, sub, recoveryChecks) ||
        !coversOriginalInk(word, sub))
        return false;
    removeUnresolvedSlots(sub.templateTrace, sub.letterDiagnostics);
    sub.letterDiagnostics = std::move(diagnostics);
    for (const auto& id : matched)
        if (!sub.matchedTemplates.contains(id))
            sub.matchedTemplates.append(id);
    sub.templateTrace.append(trace);
    sub.correct = true;
    return true;
}

} // namespace MedinaCutter
