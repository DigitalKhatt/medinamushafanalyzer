#include "pageanalysisresult.h"
#include <QApplication>
#include <QFile>

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto completeConfig = PageAnalysisResult::letterForms();
    auto config = completeConfig;
    // Exercise the provisional mechanism with feh body definitions
    // intentionally withheld, even when new supported forms are added.
    QJsonArray initial;
    for (const auto& value : config.value("initial").toArray())
        if (!value.toObject().value("current").toString().contains(QChar(0x0641))) initial.append(value);
    config.insert("initial", initial);
    check(config.value("provisionalFallbacks").toObject().value("finalYehRemainder").toBool(), "Fallback not enabled");
    for (const auto& invalid : {QJsonValue(true), QJsonValue(QJsonObject{{"finalYehRemainder", "yes"}}),
                                QJsonValue(QJsonObject{{"anyRemainder", true}})}) {
        auto draft = config; draft.insert("provisionalFallbacks", invalid);
        check(!PageAnalysisResult::prepareLetterForms(draft).isEmpty(), "Malformed fallback setting accepted");
    }
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    auto disabled = config;
    disabled.insert("provisionalFallbacks", QJsonObject{{"finalYehRemainder", false}});
    for (const auto& location : {std::array{2, 9}, std::array{3, 3}, std::array{8, 9}}) {
        const int li = location[0] - 1, wi = location[1] - 1;
        auto& word = analysis.page.lines[li].words[wi];
        auto& sub = word.wordResultInfo.subWords[0];
        const auto& shape = word.paths[sub.paths.front()];
        check(!analysis.segmentSubword(599, li, wi, 0, nullptr, true, &disabled), "Fixture already has both body templates");
        const auto oldBodies = sub.baseGlyphs;
        const auto oldJoin = *sub.joins.begin();
        check(!analysis.segmentSubword(599, li, wi, 0, nullptr, true, &config), "A remainder must not count as template complete");
        check(sub.provisional() && sub.baseGlyphs.size() == 2 && sub.joins.size() == 1,
              "Expected provisional two-body partition");
        check(sub.letterDiagnostics[0].toObject().value("status") == "remainder" &&
              sub.letterDiagnostics[0].toObject().value("character") == "ف" &&
              sub.letterDiagnostics[0].toObject().value("inferredFrom") == "yeh-fina" &&
              sub.letterDiagnostics[0].toObject().value("template").toString().isEmpty() &&
              sub.letterDiagnostics[1].toObject().value("status") == "matched",
              "Remainder lost its owner or was reported as a matched initial form");
        check(sub.matchedTemplates == QStringList{"yeh-fina"}, "A fake initial template was added");
        check(sub.joins.begin()->topBaseLine.point == oldJoin.topBaseLine.point &&
              sub.joins.begin()->bottomBaseLine.point == oldJoin.bottomBaseLine.point,
              "Fallback changed the final-yeh cut");
        for (int i = 0; i < 2; ++i) check(sub.baseGlyphs[i].path == oldBodies[i].path, "Fallback changed existing ink masks");
        for (const auto& path : sub.kashidaPaths) check(path.isEmpty(), "Remainder invented a kashida");
        auto ink = shape.transform.map(shape.path); ink.translate(shape.pos);
        std::vector<QPainterPath> bodies;
        for (const auto& glyph : sub.baseGlyphs) {
            auto body = shape.transform.map(glyph.path); body.translate(shape.pos); bodies.push_back(body);
        }
        const auto bounds = ink.boundingRect().adjusted(-.1, -.1, .1, .1);
        int sampled = 0;
        for (double y = bounds.top() + .013; y < bounds.bottom(); y += .07)
            for (double x = bounds.left() + .017; x < bounds.right(); x += .07) {
                const QPointF p(x, y);
                const int count = bodies[0].contains(p) + bodies[1].contains(p);
                check(count == int(ink.contains(p)), "Source ink was lost, duplicated or fabricated");
                sampled += ink.contains(p);
            }
        check(sampled > 1000, "Ink check did not exercise the bowl and counter");
        auto tracker = config;
        auto kashida = tracker.value("kashidaDefaults").toObject(); kashida.insert("trackingEnabled", true);
        tracker.insert("kashidaDefaults", kashida);
        analysis.segmentSubword(599, li, wi, 0, nullptr, true, &tracker);
        check(sub.provisional(), "Tracker changed fallback status");
        for (const auto& path : sub.kashidaPaths) check(path.isEmpty(), "Tracker added kashida to a shared fallback cut");
        auto noFinal = config; noFinal.insert("final", QJsonArray{});
        analysis.segmentSubword(599, li, wi, 0, nullptr, true, &noFinal);
        check(!sub.provisional(), "Remainder assigned without a matched final template");
        analysis.segmentSubword(599, li, wi, 0, nullptr, true, &disabled);
        check(!sub.provisional(), "Disabled fallback stayed active");
    }
    // Other final letters and longer words must never inherit the remainder.
    auto missingInitial = config; missingInitial.insert("initial", QJsonArray{});
    analysis.segmentSubword(599, 1, 1, 0, nullptr, true, &missingInitial); // لشد
    check(!analysis.page.lines[1].words[1].wordResultInfo.subWords[0].provisional(), "Three-letter form used remainder fallback");
    analysis.segmentSubword(599, 0, 3, 0, nullptr, true, &missingInitial); // لكنو
    check(!analysis.page.lines[0].words[3].wordResultInfo.subWords[0].provisional(), "Non-yeh final used remainder fallback");
    QFile page3("pages/page3.dat"); check(page3.open(QIODevice::ReadOnly), "Page 3 missing");
    QDataStream p3(&page3); p3 >> analysis.page;
    int checked = 0;
    for (int li = 0; li < analysis.page.lines.size(); ++li)
        for (int wi = 0; wi < analysis.page.lines[li].words.size(); ++wi) {
            auto& word = analysis.page.lines[li].words[wi];
            for (int si = 0; si < word.wordResultInfo.subWords.size(); ++si) {
                auto& sub = word.wordResultInfo.subWords[si];
                QString letters; for (const auto ch : sub.text) if (!ch.isMark()) letters += ch;
                if (letters != QStringLiteral("في")) continue;
                check(analysis.segmentSubword(2, li, wi, si, nullptr, true, &completeConfig) && !sub.provisional() &&
                      sub.letterDiagnostics[0].toObject().value("status") == "matched",
                      "Fallback replaced a successful feh body template");
                ++checked;
            }
        }
    check(checked > 0, "No matched feh-yeh control exercised");
    qInfo("Yeh remainder passed: three provisional fi cases, unchanged masks/cuts, exact sampled ink conservation, no invented kashida, tracker isolation, switch-off, missing-final rejection and complete-template precedence.");
}
