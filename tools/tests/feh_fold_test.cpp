#include "../../cutter/detail.h"
#include <GlobalParams.h>
#include <QApplication>
#include <QFile>

using namespace MedinaCutter;

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

static void loadWord(PageAnalysisResult& analysis, int page, int line, int word, AFont& font) {
    check(analysis.loadPage(page, &font, false) >= 0, "Missing native page artwork");
    analysis.resolveWordBoundaries(page, &font);
    if (analysis.page.lines[line-1].words[word-1].wordResultInfo.subWords.empty())
        analysis.detectSubWords(page-1, line-1, word-1, &font);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    globalParams = std::make_unique<GlobalParams>();
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing native font");
    QDataStream fontStream(&fontFile);
    AFont font;
    fontStream >> font;
    font.cacheAreaComparisons();
    PageAnalysisResult::initQuranText();

    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Invalid ligature configuration");
    const auto rules = config.value("ligatures").toArray();
    const auto pair = rules.first().toObject();
    const auto yeh = ligatureForm(config, pair.value("final").toObject());
    struct Example {
        int page, line, word, subword;
    };
    const Example examples[] = {
        {31, 2, 6, 1},
        {32, 6, 4, 1},
        {280, 1, 9, 3},
        {432, 5, 6, 1},
        {418, 10, 6, 1},
    };
    for (const auto& [page, line, wordIndex, subIndex] : examples) {
        PageAnalysisResult analysis;
        loadWord(analysis, page, line, wordIndex, font);
        const auto& word = analysis.page.lines[line-1].words[wordIndex-1];
        const auto& sub = word.wordResultInfo.subWords[subIndex-1];
        const auto shape = word.bodyShape(sub);
        WordResultInfo::SubWordInfo geometry;
        OutlineSearchCache cache;
        SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                              .subword = geometry, .outlineCache = &cache};
        context.bbox = context.transformedPath.boundingRect();
        check(preparePassOutline(context, cache) && declaredSharedBoundary(context, yeh),
              "Final yeh must recognize its own fold");
        check(analysis.segmentSubword(page-1, line-1, wordIndex-1, subIndex-1, &font, true, &config) &&
              sub.joins.size() == 1 && sub.baseGlyphs.size() == 2 && sub.letterDiagnostics.size() == 2 &&
              sub.letterDiagnostics[0].toObject().value("template") == "feh-descending-init" &&
              sub.letterDiagnostics[1].toObject().value("template") == "yeh-first-turn-fina",
              "The contextual feh and yeh forms must match both letters");
        const auto& cut = *sub.joins.begin();
        check(cut.topBaseLine.point == context.res.topBaseLine.point &&
              cut.bottomBaseLine.point == context.res.bottomBaseLine.point,
              "The recovered join moved the final yeh cut");
        check(toSubpaths(sub.baseGlyphs.front().path).size() == 2 && coversOriginalInk(word, sub),
              "Feh must own its counter and the two bodies must account for the source ink");
        check(sub.kashidaPaths.size() == 1 && sub.kashidaPaths.front().isEmpty(),
              "A shared boundary cannot create a kashida");
        if (page == 432) {
            auto exclusive = config;
            auto lockedPair = pair;
            auto final = pair.value("final").toObject();
            final.insert("cuts", QJsonObject{{"return", QJsonObject{{"maxChord", .01}}}});
            lockedPair.insert("final", final);
            exclusive.insert("ligatures", QJsonArray{lockedPair});
            check(PageAnalysisResult::prepareLetterForms(exclusive).isEmpty(), "Invalid exclusive test configuration");
            check(!analysis.segmentSubword(page-1, line-1, wordIndex-1, subIndex-1, &font, true, &exclusive) &&
                  sub.joins.empty() && sub.baseGlyphs.empty() && sub.templateTrace.size() == 1 &&
                  sub.templateTrace.front().startsWith("feh-yeh-ligature"),
                  "A failing contextual rule must stop before the ordinary templates");
            exclusive.remove("ligatures");
            check(analysis.segmentSubword(page-1, line-1, wordIndex-1, subIndex-1, &font, true, &exclusive) &&
                  sub.kashidaPaths.size() == 1 && !sub.kashidaPaths.front().isEmpty(),
                  "Ordinary forms must be available when the contextual rule is absent");
        }
    }

    {
        PageAnalysisResult analysis;
        loadWord(analysis, 397, 3, 6, font);
        const auto& word = analysis.page.lines[2].words[5];
        const auto& sub = word.wordResultInfo.subWords[0];
        check(analysis.segmentSubword(396, 2, 5, 0, &font, true, &config) &&
              sub.joins.size() == 1 && sub.baseGlyphs.size() == 2 &&
              sub.kashidaPaths.size() == 1 && sub.kashidaPaths.front().isEmpty() &&
              coversOriginalInk(word, sub), "The senna arm and yeh must share one cut");
        const auto& cut = *sub.joins.begin();
        check(std::abs(cut.topBaseLine.time - 7.333333333) < .005 &&
              std::abs(cut.bottomBaseLine.time - 19) < 1e-6,
              "Page 397 must keep the senna arm up to the approved yeh fold");
    }
    {
        PageAnalysisResult analysis;
        loadWord(analysis, 403, 5, 7, font);
        const auto& word = analysis.page.lines[4].words[6];
        const auto& sub = word.wordResultInfo.subWords[1];
        check(analysis.segmentSubword(402, 4, 6, 1, &font, true, &config) &&
              sub.baseGlyphs.size() == 2 && sub.kashidaPaths.size() == 1 &&
              !sub.kashidaPaths.front().isEmpty() && coversOriginalInk(word, sub),
              "The stretched senna–yeh join must retain its separate connector");
    }

    qInfo("Yeh ligatures passed: contextual priority, shared cuts, counter ownership and a stretched connector.");
}
