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
    check(!config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool(), "Tracker was enabled");
    for (const auto& invalidRange : {QJsonArray{-30, 0}, QJsonArray{"6"}}) {
        auto invalid = config;
        invalid.insert("unresolvedTiltAngles", invalidRange);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid contour tilt accepted");
    }
    {
        auto invalid = config;
        auto forms = invalid.value("final").toArray();
        forms.append(QJsonObject{{"id", "missing-rim-attachment"}, {"current", "م"}, {"onUnresolved", true},
                                 {"bodyOutline", QJsonObject{{"aspect", QJsonArray{1, 2}}, {"counters", 1}}}});
        invalid.insert("final", forms);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "An unbounded counter outline was accepted");
    }
    for (int scenario = 0; scenario < 30; ++scenario) {
        auto invalid = config;
        auto initial = invalid.value("initial").toArray();
        auto medial = invalid.value("medial").toArray();
        if (scenario == 0) invalid.insert("version", 1);
        if (scenario == 1) { auto rule = initial[0].toObject(); rule.insert("method", "join"); initial[0] = rule; }
        if (scenario == 2) { auto rule = initial[0].toObject(); rule.remove("method"); initial[0] = rule; }
        if (scenario == 3) { auto rule = medial[0].toObject(); rule.insert("id", initial[0].toObject().value("id")); medial[0] = rule; }
        if (scenario == 4) { auto rule = initial[0].toObject(); rule.insert("next", "م"); initial[0] = rule; }
        if (scenario == 5) medial.append(QJsonObject{{"id", "one-sided-medial"}, {"current", "م"},
                                                    {"right", QJsonObject{{"walk", QJsonArray{"down"}}}}});
        if (scenario == 6) {
            for (int i = 0; i < initial.size(); ++i) {
                auto rule = initial[i].toObject();
                if (rule.value("id") != "ain-init") continue;
                rule.insert("top", QJsonArray{QJsonObject{{"direction", "down"},
                    {"end", QJsonObject{{"sharp", "yes"}}}}});
                initial[i] = rule;
            }
        }
        if (scenario == 7) {
            for (int i = 0; i < medial.size(); ++i) {
                auto rule = medial[i].toObject();
                if (rule.value("id") != "lam-medi") continue;
                auto boundaries = rule.value("lamBoundaries").toObject();
                boundaries.insert("left", QJsonArray{});
                rule.insert("lamBoundaries", boundaries);
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
                auto boundaries = rule.value("lamBoundaries").toObject();
                if (scenario == 10) boundaries.insert("right", QJsonObject{{"top", QJsonArray{"down"}},
                    {"anchorSide", "sideways"}});
                else boundaries.insert("maxWidthToHeight", 0);
                rule.insert("lamBoundaries", boundaries);
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
                auto settings = rule.value("pairedCounterCorners").toObject();
                settings.insert("maxFootInwardAngle", scenario == 14 ? QJsonValue(-1) :
                    scenario == 15 ? QJsonValue(90) : QJsonValue("10"));
                rule.insert("pairedCounterCorners", settings);
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
                if (form.value("method") != "seen-teeth" && !form.contains("seenTeeth")) continue;
                auto settings = form.contains("seenTeeth") ? form.value("seenTeeth").toObject() : form;
                settings.insert("maxTipHeightSpreadRatio", scenario == 17 ? QJsonValue(0) :
                    scenario == 18 ? QJsonValue("1") : QJsonValue(-1));
                if (form.contains("seenTeeth")) form.insert("seenTeeth", settings);
                else form = settings;
                forms[i] = form;
            }
            invalid.insert(phase, forms);
        }
        if (scenario >= 20 && scenario < 23) {
            auto forms = invalid.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != "seen-medi") continue;
                auto settings = form.value("seenTeeth").toObject();
                settings.insert("rightOffset", scenario == 20 ? QJsonValue(-.5) :
                    scenario == 21 ? QJsonValue("0.5") : QJsonValue(true));
                form.insert("seenTeeth", settings);
                forms[i] = form;
            }
            invalid.insert("medial", forms);
        }
        if (scenario >= 23 && scenario < 26) {
            auto forms = invalid.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != "seen-medi") continue;
                auto settings = form.value("seenTeeth").toObject();
                settings.insert("maxExitBendDistance", scenario == 23 ? QJsonValue(0) :
                    scenario == 24 ? QJsonValue(-1) : QJsonValue("2"));
                form.insert("seenTeeth", settings);
                forms[i] = form;
            }
            invalid.insert("medial", forms);
        }
        if (scenario >= 26) {
            auto forms = invalid.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != "feh-head-medi") continue;
                auto settings = form.value("headEdges").toObject();
                if (scenario == 26) settings.insert("maxExitDistance", 2.5);
                else settings.insert("maxNeckDistance", scenario == 27 ? QJsonValue(0) :
                    scenario == 28 ? QJsonValue(-1) : QJsonValue("2"));
                form.insert("headEdges", settings);
                forms[i] = form;
            }
            invalid.insert("medial", forms);
        }
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid or generic join template accepted");
    }

    QFile forkPage("pages/page372.dat");
    check(forkPage.open(QIODevice::ReadOnly), "Could not open page 372");
    PageAnalysisResult forkAnalysis;
    QDataStream forkStream(&forkPage);
    forkStream >> forkAnalysis.page;
    check(forkAnalysis.segmentSubword(371, 8, 8, 1, nullptr, true), "Medial lam with a forked foot became partial");
    const auto& fork = forkAnalysis.page.lines[8].words[8].wordResultInfo.subWords[1];
    check(fork.letterDiagnostics[1].toObject()["template"] == "lam-medi" &&
          fork.kashidaCuts.size() == 3 && !fork.kashidaPaths[1].isEmpty(), "Missing outgoing lam kashida");
    const auto lamExit = fork.kashidaCuts[1].first;
    // Corner 21 divides the two lower flanks. The cut belongs just left of
    // it, across the narrow outgoing stroke before the medial kaf.
    check(QLineF(lamExit.p1(), {130.494, 305.017}).length() < .1 &&
          QLineF(lamExit.p2(), {130.999, 305.845}).length() < .1,
          "The lam exit crossed its foot into the incoming connector");
    const auto lamEntry = fork.kashidaCuts[0].second;
    check(QLineF(lamEntry.p1(), {133.180, 305.058}).length() < .1 &&
          QLineF(lamEntry.p2(), {132.164, 305.845}).length() < .1 &&
          std::abs(lamEntry.p2().y() - lamExit.p2().y()) < 1e-6,
          "The incoming cut must stay beside the lam at the height of its outgoing cut");
    const auto correctedEnds = fork.kashidaCuts;
    auto withoutGuard = config;
    auto medialForms = withoutGuard.value("medial").toArray();
    for (auto entry = medialForms.begin(); entry != medialForms.end(); ++entry) {
        auto rule = entry->toObject();
        if (rule.value("id") != "lam-medi") continue;
        auto bounds = rule.value("lamBoundaries").toObject();
        auto routes = bounds.value("left").toArray();
        for (auto it = routes.begin(); it != routes.end(); ++it) {
            auto route = it->toObject();
            route.remove("forkSide");
            *it = route;
        }
        bounds.insert("left", routes);
        rule.insert("lamBoundaries", bounds);
        *entry = rule;
    }
    withoutGuard.insert("medial", medialForms);
    check(forkAnalysis.segmentSubword(371, 8, 8, 1, nullptr, true, &withoutGuard),
          "Could not reproduce the previous lam exit");
    check(fork.kashidaCuts[1].first.length() > 4 && correctedEnds[1].second == fork.kashidaCuts[1].second &&
          correctedEnds[0].first == fork.kashidaCuts[0].first && correctedEnds[2] == fork.kashidaCuts[2],
          "Refining the lam foot moved a neighboring letter's attachment");

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
                QString text;
                for (const auto ch : sub.text) if (!ch.isMark()) text += ch;
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
        check(analysis.segmentSubword(2, location[0] - 1, location[1] - 1, location[2] - 1, nullptr, true),
              "Leaning medial lam is still partial");
        const auto& word = analysis.page.lines[location[0] - 1].words[location[1] - 1];
        const auto& sub = word.wordResultInfo.subWords[location[2] - 1];
        check(sub.matchedTemplates.contains("lam-medi") && sub.baseGlyphs.size() == 3 &&
              sub.joins.size() == 2 && sub.kashidaPaths.size() == 2 &&
              std::all_of(sub.kashidaPaths.begin(), sub.kashidaPaths.end(), [](const auto& p) { return !p.isEmpty(); }),
              "Expected three bodies with independent cuts on both sides of lam");
        const auto& ain = *sub.joins.begin();
        const auto& exit = *std::next(sub.joins.begin());
        check(ain.topBaseLine.isSharp, "Ain exit did not select its inward corner");
        check(location[1] != 10 || ain.topBaseLine.time == 25, "Reviewed ain anchor moved from corner 25");
        check(exit.fixedLeftBoundary && std::abs(exit.topBaseLine.point.y() - exit.bottomBaseLine.point.y()) < 1e-5,
              "Lam exit should be normal to its vertical tangent, with yeh's separate boundary retained");
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
        if (value.toObject().value("method") != "folded-arm") withoutFoldedArms.append(value);
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
    check(target.baseGlyphs.size() == 5 && target.letterDiagnostics.last().toObject().value("status") == "no-template",
          "Expected five regions to remain visibly missing their final body definition");
    qInfo("Native letter-form checks passed: schema rejection, 307 subwords, seven reviewed controls, nine lam-alef ligatures, three leaning medial lams, fixed ain/ligature/meem anchors and missing-body coverage.");
}
