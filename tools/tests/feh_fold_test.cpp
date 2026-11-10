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

    const auto config = PageAnalysisResult::letterForms();
    QJsonArray otherForms;
    QJsonObject fold;
    for (const auto& value : config.value("initial").toArray()) {
        if (value.toObject().value("id") == "feh-folded-init") fold = value.toObject();
        else otherForms.append(value);
    }
    check(!fold.isEmpty(), "Missing folded feh form");
    const auto yeh = namedLetterForm(config, "yeh-fina");
    for (const auto& [page, line, wordIndex, subIndex] : {
            std::array{31, 2, 6, 1}, std::array{32, 6, 4, 1}, std::array{280, 1, 9, 3}}) {
        PageAnalysisResult analysis;
        loadWord(analysis, page, line, wordIndex, font);
        const auto segment = [&](const QJsonObject& forms) {
            return analysis.segmentSubword(page-1, line-1, wordIndex-1, subIndex-1, &font, true, &forms);
        };
        const auto& word = analysis.page.lines[line-1].words[wordIndex-1];
        const auto& sub = word.wordResultInfo.subWords[subIndex-1];
        const auto shape = word.bodyShape(sub);
        WordResultInfo::SubWordInfo geometry;
        SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                              .subword = geometry};
        check(readTerminalOutline(context) && declaredSharedBoundary(context, yeh),
              "Final yeh must recognize its own fold");
        check(segment(config) && sub.joins.size() == 1 && sub.baseGlyphs.size() == 2 &&
              sub.matchedTemplates == QStringList{"feh-folded-init", "yeh-fina"},
              "Both letters of the folded form must match");
        const auto& cut = *sub.joins.begin();
        check(cut.topBaseLine.point == context.res.topBaseLine.point &&
              cut.bottomBaseLine.point == context.res.bottomBaseLine.point,
              "The contextual form moved the final yeh cut");
        check(toSubpaths(sub.baseGlyphs.front().path).size() == 2 && coversOriginalInk(word, sub),
              "The folded feh must own its counter and the two bodies must account for the source ink");
        check(sub.kashidaPaths.size() == 1 && sub.kashidaPaths.front().isEmpty(),
              "A shared boundary cannot create a kashida");
    }

    // This pair admits both feh forms. Edit only the order to select between them.
    PageAnalysisResult analysis;
    loadWord(analysis, 5, 14, 3, font);
    const auto& sub = analysis.page.lines[13].words[2].wordResultInfo.subWords[0];
    const auto checkForm = [&](const QJsonObject& forms, const QString& expected) {
        check(analysis.segmentSubword(4, 13, 2, 0, &font, true, &forms) &&
              sub.letterDiagnostics[0].toObject().value("template") == expected,
              "Template priority did not follow the configured order");
    };
    checkForm(config, "feh-descending-init");
    fold.remove("onIncomplete");
    fold.remove("onUnresolved");
    auto reordered = config;
    otherForms.append(fold);
    reordered.insert("initial", otherForms);
    checkForm(reordered, "feh-descending-init");
    otherForms.removeLast();
    otherForms.prepend(fold);
    reordered.insert("initial", otherForms);
    checkForm(reordered, "feh-folded-init");
    qInfo("Folded feh passed: exact shared cut, counter ownership, and configured template priority.");
}
