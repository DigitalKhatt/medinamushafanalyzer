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

// Match the prefix on its own contour before attaching the detached terminal arm.
template <typename AssemblePrefix>
static bool assembleTerminalArm(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config,
                                AssemblePrefix assemblePrefix) {
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
        mode = recipe.value("detached").toObject();
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
    WordResultInfo::SubWordInfo empty;
    SearchContext source{.transformedPath = toPage(shape.path), .subword = empty};
    if (!readTerminalOutline(source))
        return false;
    const auto componentBodies =
        recognizeComponentArms(inkOutline(source), arm.value("body").toObject().value("arm").toObject());
    const auto paths = toSubpaths(shape.path);
    if (paths.size() < 2)
        return false;
    connected = paths[0];
    for (int i = 1; i < paths.size(); ++i) {
        if (paths[0].contains(paths[i]))
            connected.addPath(paths[i]);
        else if (separate.isEmpty() &&
                 isSeparateFoldedArm(paths[0], paths[i], mode.value("allowPointContact").toBool())) {
            separate = paths[i];
            separateIndex = i;
        } else
            return false;
    }
    if (separate.isEmpty())
        return false;
    connected.setFillRule(shape.path.fillRule());
    const auto armCuts = arm.value("cuts").toObject().value("arm").toObject();
    const double maxTipGap = mode.value("maxTipGap").toDouble(armCuts.value("maxTipGap").toDouble(.75));
    const auto armBody = std::find_if(componentBodies.begin(), componentBodies.end(),
                                     [&](const auto& body) { return body.index == separateIndex; });
    if (armBody == componentBodies.end()) return false;
    const auto box = armBody->bounds;
    const auto tip = armBody->bottom.point;

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
    if (!assemblePrefix(prefix, result, prefixForms, prefixForm.value("id")) ||
        result.baseGlyphs.size() + 1 != size_t(letters.size()))
        return false;
    const auto prefixBody = toPage(result.baseGlyphs.back().path);
    QPainterPathStroker envelope;
    envelope.setWidth(2 * maxTipGap);
    if (!envelope.createStroke(prefixBody).contains(tip) || prefixBody.boundingRect().top() >= box.center().y() ||
        prefixBody.boundingRect().bottom() <= tip.y())
        return false;
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
                                             {"boundaryKind", "separate-contours"},
                                             {"candidates", QJsonArray{arm.value("id")}}});
    sub.templateTrace = result.templateTrace;
    sub.templateTrace.append(sourceTemplate + " · prefix body recovered with its terminal arm");
    sub.templateTrace.append(arm.value("id").toString() +
                             " · separate sloping arm");
    sub.correct = true;
    return true;
}

namespace MedinaCutter {

bool assembleDetachedTerminalArm(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    return assembleTerminalArm(word, sub, config, [](PageAnalysisResult& prefix, auto& result,
            QJsonObject& config, const QJsonValue& prefixId) {
        auto finals = config.value("final").toArray();
        for (qsizetype i = 0; i < finals.size(); ++i) {
            auto form = finals.at(i).toObject();
            if (form.value("id") != prefixId) continue;
            form.remove("anchorPass");
            finals.replace(i, form);
            break;
        }
        config.insert("final", finals);
        const auto expanded = expandedAnchorConfig(config, true);
        AnchorForms forms;
        if (!compileAnchorForms(expanded, forms).isEmpty()) return false;
        const auto& prefixWord = prefix.page.lines.front().words.front();
        const std::array priorities{AnchorPriority{expanded, forms,
            cutGeometryMemo && cutGeometryMemo->verifyContourGeometry}};
        auto partition = segmentWithAnchors(prefixWord, result, priorities, {});
        if (!partition) return false;
        result = std::move(*partition);
        return true;
    });
}

// In some overhanging heads the pen lifts between the incoming attachment
// and the head, even though the transcript remains joined. Recover the
// attachment from its own contour and the head from the other contour.
static bool assembleHeadComponents(const Word& word, WordResultInfo::SubWordInfo& result,
                                   const QJsonObject& config) {
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
                   const QPainterPath* head = nullptr) {
        auto analysis = componentPage(shape, component.ink, text);
        auto& localWord = analysis.page.lines.front().words.front();
        auto& local = localWord.wordResultInfo.subWords.front();
        const auto expanded = expandedAnchorConfig(forms, true);
        AnchorForms primary, continuation;
        if (!compileAnchorForms(forms, primary).isEmpty() ||
            !compileAnchorForms(expanded, continuation).isEmpty()) return std::move(local);
        const std::array priorities{AnchorPriority{forms, primary},
            AnchorPriority{expanded, continuation},
            AnchorPriority{expanded, continuation, true}};
        const auto accepts = [&](const WordResultInfo::SubWordInfo& partition) {
            if (!head) return true;
            const auto stub = world(partition.baseGlyphs.back().path);
            const auto box = stub.boundingRect();
            return toSubpaths(stub).size() == 1 && box.width() > box.height() &&
                box.width() <= settings.value("maxEntryToHead").toDouble() * head->boundingRect().width() &&
                contact.createStroke(stub).intersects(*head);
        };
        if (auto partition = segmentWithAnchors(localWord, local, priorities, accepts,
                head ? form.value("id").toString() : QString{}))
            return std::move(*partition);
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
        auto suffix = fit(pieces[1], letters.mid(slot), config);
        if (!suffix.correct || suffix.baseGlyphs.empty()) continue;
        const auto head = world(suffix.baseGlyphs.front().path);
        auto prefix = fit(pieces[0], letters.left(prefixSize), prefixForms, separate ? nullptr : &head);
        if (!suffix.correct ||
            prefix.baseGlyphs.size() != size_t(prefixSize) || suffix.baseGlyphs.size() != size_t(letters.size() - slot) ||
            prefix.joins.size() != size_t(prefixSize - 1) || suffix.joins.size() + 1 != suffix.baseGlyphs.size() ||
            prefix.letterDiagnostics.size() != prefixSize || suffix.letterDiagnostics.size() != letters.size() - slot)
            continue;
        if (std::any_of(prefix.letterDiagnostics.constBegin(), prefix.letterDiagnostics.constEnd() - 1,
                        [](const auto& v) { return v.toObject().value("status") != "matched"; })) continue;
        const auto attachment = world(prefix.baseGlyphs.back().path);
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

bool assembleDetachedHeadEntry(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    return assembleHeadComponents(word, sub, config);
}

} // namespace MedinaCutter
