#include "pageanalysisresult.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Position schema rejected bundled forms");
    {
        auto legacy = config;
        auto initial = legacy.value("initial").toArray();
        auto form = initial.first().toObject();
        form.remove("priorityWithinPass");
        form.insert("onIncomplete", true);
        form.insert("onUnresolved", true);
        initial.replace(0, form);
        legacy.insert("initial", initial);
        check(PageAnalysisResult::prepareLetterForms(legacy).isEmpty(), "Legacy priority flags were rejected");
        const auto migrated = legacy.value("initial").toArray().first().toObject();
        check(migrated.value("priorityWithinPass").toInt() == 2 && !migrated.contains("onIncomplete") &&
              !migrated.contains("onUnresolved"), "Legacy priority flags changed template order");
    }
    for (const auto& bad : {QJsonObject{{"boundaries", QJsonObject{{"right", "vertical"}, {"left", "free"}}}}, QJsonObject{{"preserveEntryAfter", true}}}) {
        auto invalid = config;
        auto forms = invalid.value("medial").toArray();
        for (int i = 0; i < forms.size(); ++i) {
            auto form = forms[i].toObject();
            if (form.value("id") != "lam-medi") continue;
            auto cuts = form.value("cuts").toObject();
            for (auto it = bad.begin(); it != bad.end(); ++it) cuts.insert(it.key(), it.value());
            form.insert("cuts", cuts);
            forms[i] = form;
        }
        invalid.insert("medial", forms);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid terminal lam policy accepted");
    }
    for (const auto& inherited : {QString("missing-form"), QString("lam-bowl-fina")}) {
        auto invalid = config;
        auto finals = invalid.value("final").toArray();
        for (int i = 0; i < finals.size(); ++i) {
            auto form = finals[i].toObject();
            if (form.value("id") != "lam-bowl-fina") continue;
            form.insert("body", QJsonObject{{"inherit", inherited}});
            finals[i] = form;
        }
        invalid.insert("final", finals);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Missing or cyclic body reference accepted");
    }
    for (bool badReference : {false, true}) {
        auto invalid = config;
        auto forms = invalid.value("medial").toArray();
        for (int i = 0; i < forms.size(); ++i) {
            auto form = forms[i].toObject();
            if (form.value("id") != "lam-medi") continue;
            auto settings = form.value("cuts").toObject();
            auto terminal = settings.value("beforeFinal").toObject();
            auto junction = terminal.value("junction").toObject();
            if (badReference) junction.insert("template", "missing-body");
            else junction.insert("at", "normal");
            terminal.insert("junction", junction);
            settings.insert("beforeFinal", terminal);
            form.insert("cuts", settings);
            forms[i] = form;
        }
        invalid.insert("medial", forms);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid alef junction policy accepted");
    }
    {
        const auto withShortExit = [&](const QJsonValue& value) {
            auto draft = config;
            auto forms = draft.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != "lam-medi") continue;
                auto cuts = form.value("cuts").toObject();
                cuts.insert("shortExit", value);
                form.insert("cuts", cuts);
                forms[i] = form;
            }
            draft.insert("medial", forms);
            return draft;
        };
        auto draft = withShortExit(false);
        check(PageAnalysisResult::prepareLetterForms(draft).isEmpty(), "Short lam exit cannot be disabled");
        QJsonObject shortExit;
        for (const auto& value : config.value("medial").toArray())
            if (value.toObject().value("id") == "lam-medi")
                shortExit = value.toObject().value("cuts").toObject().value("shortExit").toObject();
        shortExit.insert("minLengthRatio", 1);
        draft = withShortExit(shortExit);
        check(!PageAnalysisResult::prepareLetterForms(draft).isEmpty(), "Invalid short lam exit ratio accepted");
    }
    // Lām forms cannot silently re-enter the old method dispatcher.
    for (const auto* key : {"method", "legacyLamRoutes"}) {
        auto invalid = config;
        auto forms = invalid.value("initial").toArray();
        for (int i = 0; i < forms.size(); ++i) {
            auto form = forms[i].toObject();
            if (form.value("id") != "lam-init") continue;
            form.insert(key, "walk");
            forms[i] = form;
        }
        invalid.insert("initial", forms);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Legacy lām matcher configuration accepted");
    }
    check(!config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool(), "Tracker was enabled");
    {
        auto invalid = config;
        auto forms = invalid.value("final").toArray();
        const auto rim = QJsonObject{{"top", QJsonArray{0, 1}}, {"bottom", QJsonArray{0, 1}},
                                     {"left", QJsonArray{0, 1}}, {"right", QJsonArray{0, 1}}};
        forms.append(QJsonObject{{"id", "missing-rim-attachment"}, {"current", "م"}, {"priorityWithinPass", 2},
            {"body", QJsonObject{{"shape", "counter-outline"}, {"counters", 1},
                                 {"outline", QJsonObject{{"aspect", QJsonArray{1, 2}}, {"rim", rim}}}}},
            {"cuts", QJsonObject{{"boundaries", QJsonObject{{"left", "free"}, {"right", "outline-entry"}}}}}});
        invalid.insert("final", forms);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "An unbounded counter outline was accepted");
    }
    {
        auto invalid = config;
        auto finals = invalid.value("final").toArray();
        for (auto entry = finals.begin(); entry != finals.end(); ++entry) {
            auto form = entry->toObject();
            if (form.value("id") != "yeh-first-turn-fina") continue;
            form.remove("sharedBoundary");
            *entry = form;
        }
        invalid.insert("final", finals);
        check(PageAnalysisResult::prepareLetterForms(invalid).contains("shareEntryWhenUnresolved"),
              "A terminal assembly without a declared shared boundary was accepted");
    }
    for (int scenario = 0; scenario < 30; ++scenario) {
        auto invalid = config;
        auto initial = invalid.value("initial").toArray();
        auto medial = invalid.value("medial").toArray();
        if (scenario == 0) invalid.insert("version", 1);
        if (scenario == 1) { auto rule = initial[0].toObject(); rule.insert("method", "join"); initial[0] = rule; }
        if (scenario == 2) { auto rule = initial[0].toObject(); rule.remove("body"); rule.remove("cuts"); initial[0] = rule; }
        if (scenario == 3) { auto rule = medial[0].toObject(); rule.insert("id", initial[0].toObject().value("id")); medial[0] = rule; }
        if (scenario == 4) { auto rule = initial[0].toObject(); rule.insert("next", "م"); initial[0] = rule; }
        if (scenario == 5) medial.append(QJsonObject{{"id", "one-sided-medial"}, {"current", "م"},
                                                    {"right", QJsonObject{{"walk", QJsonArray{"down"}}}}});
        if (scenario == 6) {
            for (int i = 0; i < initial.size(); ++i) {
                auto rule = initial[i].toObject();
                if (rule.value("id") != "ain-init") continue;
                auto body = rule.value("body").toObject(), route = body.value("route").toObject();
                route.insert("top", QJsonArray{QJsonObject{{"direction", "down"},
                    {"end", QJsonObject{{"sharp", "yes"}}}}});
                body.insert("route", route); rule.insert("body", body);
                initial[i] = rule;
            }
        }
        if (scenario == 7) {
            for (int i = 0; i < medial.size(); ++i) {
                auto rule = medial[i].toObject();
                if (rule.value("id") != "lam-medi") continue;
                auto body = rule.value("body").toObject();
                body.insert("exitRoutes", QJsonArray{});
                rule.insert("body", body);
                medial[i] = rule;
            }
        }
        if (scenario == 8) medial.append(QJsonObject{{"id", "incomplete-contour"}, {"current", "ك"},
            {"contourBody", QJsonObject{{"right", QJsonObject{{"top", QJsonArray{"down"}}}}}}});
        if (scenario == 9) initial.append(QJsonObject{{"id", "invalid-offset"}, {"current", "ل"},
            {"method", "walk"}, {"top", QJsonArray{"down"}}, {"walkNormalOffset", -0.5}});
        if (scenario == 10 || scenario == 13) {
            for (int i = 0; i < medial.size(); ++i) {
                auto rule = medial[i].toObject();
                if (rule.value("id") != "lam-medi") continue;
                auto boundaries = rule.value("cuts").toObject();
                if (scenario == 10) boundaries.insert("entryRoute", QJsonObject{{"top", QJsonArray{"down"}},
                    {"anchorSide", "sideways"}});
                else {
                    auto body = rule.value("body").toObject();
                    body.insert("maxWidthToHeight", 0);
                    rule.insert("body", body);
                }
                rule.insert("cuts", boundaries);
                medial[i] = rule;
            }
        }
        if (scenario == 11) medial.append(QJsonObject{{"id", "invalid-counter-pair"}, {"current", "ه"},
            {"loopBoundaries", QJsonObject{{"counterPair", "diagonal"}}}});
        if (scenario == 12) {
            auto settings = invalid.value("kashidaDefaults").toObject();
            settings.insert("sharedCutTolerance", 1.5);
            invalid.insert("kashidaDefaults", settings);
        }
        if (scenario >= 14 && scenario <= 16) {
            for (int i = 0; i < medial.size(); ++i) {
                auto rule = medial[i].toObject();
                if (rule.value("id") != "heh-body-medi") continue;
                auto body = rule.value("body").toObject();
                auto settings = body.value("pair").toObject();
                settings.insert("maxFootInwardAngle", scenario == 14 ? QJsonValue(-1) :
                    scenario == 15 ? QJsonValue(90) : QJsonValue("10"));
                body.insert("pair", settings); rule.insert("body", body);
                medial[i] = rule;
            }
        }
        invalid.insert("initial", initial);
        invalid.insert("medial", medial);
        if (scenario >= 17 && scenario < 20) {
            const auto phase = scenario == 17 ? "initial" : scenario == 18 ? "medial" : "final";
            auto forms = invalid.value(phase).toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("body").toObject().value("shape") != "tooth-cluster") continue;
                auto body = form.value("body").toObject();
                auto settings = body.value("cluster").toObject();
                settings.insert("maxTipHeightSpreadRatio", scenario == 17 ? QJsonValue(0) :
                    scenario == 18 ? QJsonValue("1") : QJsonValue(-1));
                body.insert("cluster", settings); form.insert("body", body);
                forms[i] = form;
            }
            invalid.insert(phase, forms);
        }
        if (scenario >= 20 && scenario < 23) {
            auto forms = invalid.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != "seen-medi") continue;
                auto cuts = form.value("cuts").toObject();
                auto settings = cuts.value("cluster").toObject();
                settings.insert("rightOffset", scenario == 20 ? QJsonValue(-.5) :
                    scenario == 21 ? QJsonValue("0.5") : QJsonValue(true));
                cuts.insert("cluster", settings); form.insert("cuts", cuts);
                forms[i] = form;
            }
            invalid.insert("medial", forms);
        }
        if (scenario >= 23 && scenario < 26) {
            auto forms = invalid.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != "seen-medi") continue;
                auto cuts = form.value("cuts").toObject();
                auto settings = cuts.value("cluster").toObject();
                settings.insert("maxExitBendDistance", scenario == 23 ? QJsonValue(0) :
                    scenario == 24 ? QJsonValue(-1) : QJsonValue("2"));
                cuts.insert("cluster", settings); form.insert("cuts", cuts);
                forms[i] = form;
            }
            invalid.insert("medial", forms);
        }
        if (scenario >= 26) {
            auto forms = invalid.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != "feh-head-medi") continue;
                auto policy = form.value("cuts").toObject();
                auto settings = policy.value("head").toObject();
                if (scenario == 26) settings.insert("maxExitDistance", 2.5);
                else settings.insert("maxNeckDistance", scenario == 27 ? QJsonValue(0) :
                    scenario == 28 ? QJsonValue(-1) : QJsonValue("2"));
                policy.insert("head", settings);
                form.insert("cuts", policy);
                forms[i] = form;
            }
            invalid.insert("medial", forms);
        }
        if(PageAnalysisResult::prepareLetterForms(invalid).isEmpty())qFatal("Invalid template scenario %d accepted",scenario);
    }

    const auto withForkOffset = [&](QJsonValue offset) {
        auto draft = config;
        auto forms = draft.value("medial").toArray();
        for (auto entry = forms.begin(); entry != forms.end(); ++entry) {
            auto form = entry->toObject();
            if (form.value("id") != "lam-medi") continue;
            auto bounds = form.value("cuts").toObject();
            if (offset.isDouble()) bounds.insert("forkOffset", offset);
            else bounds.remove("forkOffset");
            form.insert("cuts", bounds);
            *entry = form;
        }
        draft.insert("medial", forms);
        return draft;
    };
    QFile tightForkPage("pages/page81.dat");
    check(tightForkPage.open(QIODevice::ReadOnly), "Could not open page 81");
    PageAnalysisResult tightForkAnalysis;
    QDataStream tightForkStream(&tightForkPage);
    tightForkStream >> tightForkAnalysis.page;
    check(tightForkAnalysis.segmentSubword(80, 13, 0, 1, nullptr, true), "Page 81 fork did not segment");
    const auto& tightFork = tightForkAnalysis.page.lines[13].words[0].wordResultInfo.subWords[1];
    check(tightFork.baseGlyphs.size() == 5 && tightFork.kashidaCuts.size() == 4 &&
          tightFork.kashidaCuts[0].second.length() < 1.5 && tightFork.kashidaCuts[1].first.length() < 1.5 &&
          tightFork.kashidaCuts[0].second.p2().x() > tightFork.kashidaCuts[1].first.p2().x(),
          "Expected two short cuts on opposite sides of the page 81 fork");
    const auto separated = tightFork.kashidaCuts;
    const auto noRoom = withForkOffset(30);
    check(tightForkAnalysis.segmentSubword(80, 13, 0, 1, nullptr, true, &noRoom), "Shared-tip fork did not segment");
    const auto rightNormal = tightFork.kashidaCuts[0].second;
    const auto leftNormal = tightFork.kashidaCuts[1].first;
    check(rightNormal.p2() == leftNormal.p2() && rightNormal.p1().x() > rightNormal.p2().x() &&
          leftNormal.p1().x() < leftNormal.p2().x() && rightNormal.p1().y() < rightNormal.p2().y() &&
          leftNormal.p1().y() < leftNormal.p2().y() && separated[0].first == tightFork.kashidaCuts[0].first &&
          separated[1].second == tightFork.kashidaCuts[1].second,
          "Shared-tip cuts must follow opposite normals and keep the neighboring attachments");

    QFile forkPage("pages/page372.dat");
    check(forkPage.open(QIODevice::ReadOnly), "Could not open page 372");
    PageAnalysisResult forkAnalysis;
    QDataStream forkStream(&forkPage);
    forkStream >> forkAnalysis.page;
    check(forkAnalysis.segmentSubword(371, 8, 8, 1, nullptr, true, &config), "Page 372 lām fork did not segment");
    const auto& fork = forkAnalysis.page.lines[8].words[8].wordResultInfo.subWords[1];
    check(fork.letterDiagnostics[1].toObject()["template"] == "lam-medi" &&
          fork.kashidaCuts.size() == 3 && !fork.kashidaPaths[1].isEmpty(), "Missing outgoing lām connector");
    const auto lamExit = fork.kashidaCuts[1].first;
    const auto lamEntry = fork.kashidaCuts[0].second;
    check(QLineF(lamExit.p2(), {131.471, 305.315}).length() < .02 &&
          QLineF(lamEntry.p2(), {131.852, 305.331}).length() < .02 &&
          lamExit.length() < 1.5 && lamEntry.length() < 1.5,
          "The lām cuts left the two flanks of the fork");

    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 cache is required for the native checks");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Could not read page 3");
    int checked = 0;
    for (int li = 0; li < analysis.page.lines.size(); ++li) {
        auto& line = analysis.page.lines[li];
        for (int wi = 0; wi < line.words.size(); ++wi) {
            auto& word = line.words[wi];
            for (int si = 0; si < word.wordResultInfo.subWords.size(); ++si) {
                const bool complete = analysis.segmentSubword(2, li, wi, si, nullptr, true);
                const auto& sub = word.wordResultInfo.subWords[si];
                const auto text = PageAnalysisResult::bodyLetters(sub.text);
                if (sub.matchedTemplates.contains("alef-folded-fina"))
                    check(text.size() == 2 && text.front() == QChar(0x0644) &&
                          QStringLiteral("اآأإٱ").contains(text.back()), "Ligature rule matched an unrelated subword");
                check(sub.letterDiagnostics.size() == text.size(), "Missing per-letter diagnostic");
                bool covered = true;
                for (const auto& value : sub.letterDiagnostics) {
                    const auto letter = value.toObject();
                    const auto status = letter.value("status").toString();
                    covered &= status == "matched" || status == "not-required";
                    if (status != "matched") continue;
                    const auto forms = config.value(letter.value("position").toString()).toArray();
                    const bool owns = std::any_of(forms.begin(), forms.end(), [&](const auto& form) {
                        const auto rule = form.toObject();
                        return rule.value("id") == letter.value("template") &&
                            rule.value("current").toString().contains(letter.value("character").toString());
                    });
                    check(owns, "A template was assigned to the wrong letter or position");
                }
                check(!complete || covered, "Region count concealed a missing body template");
                ++checked;
            }
        }
    }
    check(checked == 307, "Page audit did not cover all subwords");
    for (const auto& location : {std::array{1, 5, 1}, std::array{1, 9, 1}, std::array{2, 5, 2},
                                 std::array{2, 9, 1}, std::array{3, 8, 2}, std::array{1, 3, 1}, std::array{1, 2, 2}})
        check(analysis.segmentSubword(2, location[0] - 1, location[1] - 1, location[2] - 1, nullptr, true),
              "Reviewed body-template control became partial");
    for (const auto& location : {std::array{2, 1, 1}, std::array{4, 6, 2}, std::array{5, 7, 2},
             std::array{8, 1, 1}, std::array{8, 4, 2}, std::array{8, 10, 2}, std::array{9, 4, 1},
             std::array{11, 1, 2}, std::array{11, 6, 1}}) {
        check(analysis.segmentSubword(2, location[0] - 1, location[1] - 1, location[2] - 1, nullptr, true),
              "Standalone lam-alef ligature is still partial");
        const auto& ligature = analysis.page.lines[location[0] - 1].words[location[1] - 1]
            .wordResultInfo.subWords[location[2] - 1];
        check(ligature.matchedTemplates.contains("alef-folded-fina") && ligature.baseGlyphs.size() == 2 &&
              ligature.joins.size() == 1 && !ligature.joins.begin()->fixedLeftBoundary &&
              std::all_of(ligature.kashidaPaths.begin(), ligature.kashidaPaths.end(), [](const auto& p) { return p.isEmpty(); }),
              "Ligature should use one shared cut with no kashida");
    }
    const auto& ligature = analysis.page.lines[1].words[0].wordResultInfo.subWords[0];
    check(ligature.joins.begin()->topBaseLine.time == 4 && ligature.joins.begin()->bottomBaseLine.time == 12,
          "Reviewed lam-alef boundary moved away from corners 4 and 12");
    for (const auto& location : {std::array{2, 6, 1}, std::array{2, 8, 2}, std::array{2, 10, 2}}) {
        check(analysis.segmentSubword(2, location[0] - 1, location[1] - 1, location[2] - 1, nullptr, true, &config),
              "Leaning medial lam is still partial");
        const auto& word = analysis.page.lines[location[0] - 1].words[location[1] - 1];
        const auto& sub = word.wordResultInfo.subWords[location[2] - 1];
        check(sub.matchedTemplates.contains("lam-medi") && sub.baseGlyphs.size() == 3 &&
              sub.joins.size() == 2 && sub.kashidaPaths.size() == 2 &&
              !sub.kashidaPaths.front().isEmpty() && sub.kashidaPaths.back().isEmpty(),
              "The leaning lām lost its incoming connector or shared yeh cut");
        const auto& ain = *sub.joins.begin();
        const auto& exit = *std::next(sub.joins.begin());
        check(ain.topBaseLine.isSharp, "Ain exit did not select its inward corner");
        check(location[1] != 10 || ain.topBaseLine.time == 25, "Reviewed ain anchor moved from corner 25");
        check(!exit.fixedLeftBoundary && exit.bottomBaseLine.time == (location[1] == 10 ? 13 : 14),
              "The lām–yeh shared cut moved away from its contour junction");
        const auto& transform = word.paths[sub.paths.front()].transform;
        const auto lamBox = transform.map(sub.baseGlyphs[1].path).boundingRect();
        const auto yehBox = transform.map(sub.baseGlyphs[2].path).boundingRect();
        check(lamBox.left() > yehBox.left() && lamBox.bottom() < yehBox.bottom(),
              "Lam cut swallowed the yeh bowl or left a sliver as its body");
    }
    auto withoutLigature = config;
    QJsonArray remainingFinal;
    for (const auto& value : config.value("final").toArray())
        if (value.toObject().value("id") != "alef-folded-fina") remainingFinal.append(value);
    withoutLigature.insert("final", remainingFinal);
    check(analysis.segmentSubword(2, 1, 0, 0, nullptr, true, &withoutLigature) &&
          analysis.page.lines[1].words[0].wordResultInfo.subWords[0].matchedTemplates.contains("alef-tapered-arm-fina"),
          "Expected the folded arm to supply the alternative ligature cut");
    QJsonArray withoutFoldedArms;
    for (const auto& value : remainingFinal)
        if (value.toObject().value("body").toObject().value("shape") != "tapered-arm") withoutFoldedArms.append(value);
    withoutLigature.insert("final", withoutFoldedArms);
    check(!analysis.segmentSubword(2, 1, 0, 0, nullptr, true, &withoutLigature),
          "Upright alef incorrectly substitutes for the missing folded arm");
    auto& target = analysis.page.lines[1].words[8].wordResultInfo.subWords[0];
    std::vector<Stretching> joins(target.joins.begin(), target.joins.end());
    check(joins.size() == 4 && joins[0].bottomLeft.time == 21 && joins[3].bottomLeft.time == 10,
          "Reviewed meem corner anchors moved");
    auto missingFinal = config;
    missingFinal.insert("final", QJsonArray{});
    check(!analysis.segmentSubword(2, 1, 8, 0, nullptr, true, &missingFinal), "Missing final body reported complete");
    check(target.baseGlyphs.empty() && target.letterDiagnostics.last().toObject().value("status") == "no-template",
          "The cutter kept a stale partition after the final template was removed");

    QFile rehPage("pages/page131.dat");
    check(rehPage.open(QIODevice::ReadOnly), "Missing raised reh crest fixture");
    PageAnalysisResult raisedReh;
    QDataStream rehStream(&rehPage);
    rehStream >> raisedReh.page;
    check(raisedReh.segmentSubword(130, 7, 5, 0, nullptr, true), "Raised reh crest did not match");
    const auto& raised = raisedReh.page.lines[7].words[5].wordResultInfo.subWords[0];
    check(raised.baseGlyphs.size() == 3 && raised.joins.size() == 2 && raised.kashidaPaths.size() == 2 &&
          !raised.kashidaPaths.back().isEmpty() && raised.joins.rbegin()->bottomLeft.time == 7,
          "The raised reh lost its crest or the genuine incoming connector");

    struct ReviewedJoin {
        int page, line, word, subword, join;
        double upper, lower;
    };
    for (const auto& at : {ReviewedJoin{225, 9, 8, 1, 3, 24, 34},
                           ReviewedJoin{274, 6, 2, 2, 0, 30, 19},
                           ReviewedJoin{274, 6, 7, 2, 2, 33.9905, 45},
                           ReviewedJoin{357, 12, 6, 2, 2, 28.3481, 35},
                           ReviewedJoin{456, 1, 8, 3, 0, 8.0687, 17},
                           ReviewedJoin{521, 1, 8, 1, 0, 40, 30},
                           ReviewedJoin{539, 13, 7, 1, 1, 5.731, 15}}) {
        QFile fixture(QString("pages/page%1.dat").arg(at.page));
        check(fixture.open(QIODevice::ReadOnly), "Missing reviewed join fixture");
        PageAnalysisResult reviewed;
        QDataStream input(&fixture);
        input >> reviewed.page;
        check(reviewed.segmentSubword(at.page - 1, at.line - 1, at.word - 1, at.subword - 1, nullptr, true),
              "Reviewed stacked or final join did not match");
        const auto& sub = reviewed.page.lines[at.line - 1].words[at.word - 1].wordResultInfo.subWords[at.subword - 1];
        check(sub.joins.size() > size_t(at.join) && sub.kashidaPaths.size() > size_t(at.join),
              "Reviewed join is missing");
        const auto& cut = *std::next(sub.joins.begin(), at.join);
        check(std::abs(cut.topBaseLine.time - at.upper) < .001 &&
              std::abs(cut.bottomBaseLine.time - at.lower) < .001 && sub.kashidaPaths[at.join].isEmpty(),
              "The shared cut moved off the reviewed attachment");
    }

    qInfo("Native letter-form checks passed: schema rejection, 307 subwords, seven reviewed controls, nine lam-alef ligatures, three leaning medial lams, fixed ain/ligature/meem anchors and missing-body coverage.");
}
