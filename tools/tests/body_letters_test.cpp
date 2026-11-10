#include "pageanalysisresult.h"
#include "lettersegmentationcache.h"
#include "afont.h"
#include <QApplication>
#include <QTemporaryDir>

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

static QByteArray sourceBytes(const PageAnalysisResult& analysis) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << analysis.page;
    return bytes;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto text = QStringLiteral("لَتُسْـَٔلُنَّ");
    const auto expected = QStringLiteral("لتسلن");
    check(PageAnalysisResult::bodyLetters(text) == expected, "Tatweel became a letter body");
    check(PageAnalysisResult::bodyLetters(QStringLiteral("بِـٔــۧمْ")) == QStringLiteral("بم"),
          "Repeated tatweel or its marks created body slots");
    check(PageAnalysisResult::bodyLetters(QStringLiteral("أإآؤئء")) == QStringLiteral("أإآؤئء"),
          "Actual hamza-bearing letters were removed");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 is missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[14].words[6];
    auto& sub = word.wordResultInfo.subWords[0];
    check(sub.text == text && sub.paths.size() == 11, "Unexpected source text/components");
    const auto original = sourceBytes(analysis);
    const auto config = PageAnalysisResult::letterForms();
    check(!config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool(),
          "Body letter regression requires the tracker disabled");
    check(analysis.segmentSubword(599, 14, 6, 0, nullptr, true), "Spacer still blocks medial lam/final noon");
    check(sub.baseGlyphs.size() == 5 && sub.joins.size() == 4 && sub.kashidaPaths.size() == 4 &&
          std::all_of(sub.kashidaPaths.begin(), sub.kashidaPaths.end(), [](const auto& p) { return !p.isEmpty(); }),
          "Expected five connected bodies and four connectors");
    QString labels;
    for (const auto& value : sub.letterDiagnostics) {
        const auto diagnostic = value.toObject();
        labels += diagnostic.value("character").toString();
        check(diagnostic.value("status") == "matched", "Unmatched body was reported complete");
    }
    check(labels == expected && sub.matchedTemplates.contains("lam-medi") &&
          sub.matchedTemplates.contains("noon-fina"), "Wrong body labels or final templates");
    check(sourceBytes(analysis) == original, "Source text or mark components changed");
    const auto bodies = sub.baseGlyphs;
    const auto connectors = sub.kashidaPaths;
    // The notation's presence must not change geometry; never rewrite the stored text.
    sub.text.remove(QChar(0x0640));
    check(analysis.segmentSubword(599, 14, 6, 0, nullptr, true) && sub.kashidaPaths == connectors,
          "Removing optional tatweel changed connectors");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(bodies[i].path == sub.baseGlyphs[i].path, "Removing optional tatweel changed a body mask");
    sub.text = text;
    check(analysis.segmentSubword(599, 14, 6, 0, nullptr, true) && sub.kashidaPaths == connectors,
          "Repeat segmentation changed connectors");

    AFont font;
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Font cache is missing");
    QDataStream fontStream(&fontFile); fontStream >> font;
    QTemporaryDir temporary;
    check(temporary.isValid(), "No temporary cache directory");
    const auto workingDirectory = QDir::currentPath();
    check(QDir::setCurrent(temporary.path()), "Cannot select temporary cache directory");
    bool hit = true;
    const auto exported = loadLetterSegmentation(analysis, 600, font, &hit, true);
    check(!hit && exported.value("version").toInt() >= 60, "Old cache version retained");
    QJsonObject target;
    for (const auto& value : exported.value("subwords").toArray()) {
        const auto row = value.toObject();
        if (row.value("line") == 15 && row.value("word") == 7 && row.value("subword") == 1) target = row;
    }
    check(target.value("complete").toBool() && target.value("text") == text, "Export lost complete state or source text");
    labels.clear();
    for (const auto& body : target.value("bodies").toArray()) labels += body.toObject().value("letter").toString();
    check(labels == expected && target.value("letters").toArray().size() == 5, "Export misaligned body labels after tatweel");
    check(loadLetterSegmentation(analysis, 600, font, &hit) == exported && hit, "Fresh segmentation cache was not reused");
    auto oldCache = exported; oldCache.insert("version", 59);
    check(saveLetterSegmentation("output/segmentation-cache/page600.json", oldCache), "Cannot write stale fixture cache");
    check(loadLetterSegmentation(analysis, 600, font, &hit) == exported && !hit, "Stale tatweel cache was not invalidated");
    check(QDir::setCurrent(workingDirectory), "Cannot restore working directory");
    check(sourceBytes(analysis) == original, "Export changed source text or marks");
    // A spacer/mark alone cannot silently become a complete, unnamed letter.
    const auto& shape = word.paths[sub.paths.front()];
    sub.text = QStringLiteral("ـٔ");
    check(!analysis.segmentSubword(599, 14, 6, 0, nullptr, true) && sub.baseGlyphs.size() == 1 &&
          sub.baseGlyphs[0].path == shape.path && sub.letterDiagnostics.isEmpty(),
          "No-letter input was falsely accepted or its ink was discarded");
    qInfo("Body letters passed: tatweel ignored, hamza-bearing letters preserved, five bodies/four connectors, unchanged source/marks, deterministic masks, aligned exports and stale-cache invalidation.");
}
