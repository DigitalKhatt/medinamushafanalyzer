#include "pageanalysisresult.h"
#include "afont.h"
#include "GlobalParams.h"
#include <QApplication>
#include <QFile>

static void check(bool ok, const QString& message) { if (!ok) qFatal("%s", qPrintable(message)); }
static PageAnalysisResult readPage() {
    QFile file("pages/page2.dat"); check(file.open(QIODevice::ReadOnly), "Missing native page cache");
    PageAnalysisResult result; QDataStream stream(&file); stream >> result.page;
    check(stream.status() == QDataStream::Ok, "Invalid native page cache"); return result;
}
static QByteArray shapeBytes(Apath path) {
    path.type = CustomDataType(0); // The flag is the only permitted mutation.
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream << path; return bytes;
}
static QByteArray pageBytes(const APage& page) {
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream << page; return bytes;
}
static QRectF worldBox(const Apath& path) {
    auto world = path.transform.map(path.path); world.translate(path.pos); return world.boundingRect();
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    globalParams = std::make_unique<GlobalParams>(); PageAnalysisResult::initQuranText();
    AFont font; QFile file("input/font.dat"); check(file.open(QIODevice::ReadOnly), "Missing native font");
    QDataStream stream(&file); stream >> font; check(stream.status() == QDataStream::Ok, "Invalid native font");
    auto cached = readPage(); const auto before = cached.page.lines[6].words[4];
    const auto& stop = before.paths[8];
    check(int(stop.type) == 0, "Fixture no longer exercises a missing flag");
    check(font.checkGlyph(stop.path, "sad.waqf") == -1, "Fixture no longer exercises fixed-scale failure");
    double distance = 1;
    check(font.checkGlyphScaled(stop.path, "sad.waqf", constants::MATCHING_ERROR_RATIO, &distance) != -1,
          "Scaled stop-sign template did not recognize page002:l7-w5-p8");
    for (int pi : {3, 5, 6, 7, 9})
        check(font.checkGlyphScaled(before.paths[pi].path, "sad.waqf") == -1,
              "Kasra, dot, shadda or sukun was mistaken for a stop sign");
    for (double scale : {.4, 1.7, 3.0}) {
        auto moved = QTransform::fromScale(scale, scale).map(stop.path); moved.translate(37, -21);
        check(font.checkGlyphScaled(moved, "sad.waqf") != -1, "Stop-sign recognition depends on size or position");
    }
    check(font.checkGlyphScaled(QTransform::fromScale(2, 1).map(stop.path), "sad.waqf") == -1,
          "Nonuniform distortion bypassed the aspect-ratio guard");
    check(font.checkGlyphScaled(QPainterPath(), "sad.waqf") == -1, "Empty ink matched a stop sign");
    check(font.checkGlyphScaled(stop.path, "missing-template") == -1, "Missing template matched a stop sign");
    cached.detectSubWords(1, 6, 4, &font);
    const auto& after = cached.page.lines[6].words[4];
    check(after.paths[8].type == CustomDataType::WAQF, "Cached flag was not restored");
    check(after.paths.size() == before.paths.size(), "Flag restoration changed source paths");
    for (int pi = 0; pi < before.paths.size(); ++pi) {
        check(shapeBytes(before.paths[pi]) == shapeBytes(after.paths[pi]), "Restoration changed ink or contour metadata");
        if (pi != 8) check(before.paths[pi].type == after.paths[pi].type, "An unrelated path was retagged");
    }
    check(before.wordResultInfo.subWords.size() == after.wordResultInfo.subWords.size(), "Subword count changed");
    for (int si = 0; si < int(before.wordResultInfo.subWords.size()); ++si)
        check(before.wordResultInfo.subWords[si].paths == after.wordResultInfo.subWords[si].paths,
              "Cached subword ownership changed");
    check(cached.waqfTypeRepairs.size() == 1, "Restored flag must have one provenance record");
    const auto stable = pageBytes(cached.page);
    cached.detectSubWords(1, 6, 4, &font);
    check(pageBytes(cached.page) == stable && cached.waqfTypeRepairs.size() == 1, "Restoration is not idempotent");
    APage roundtrip; QDataStream restored(stable); restored >> roundtrip;
    check(roundtrip.lines[6].words[4].paths[8].type == CustomDataType::WAQF, "Restored type did not survive serialization");
    check(int(readPage().page.lines[6].words[4].paths[8].type) == 0, "Test modified the source cache");
    // Exercise the original AI/PDF without reading the page cache.
    auto& fresh = cached; check(fresh.loadPage(2, &font, false) == 0, "Fresh PDF parsing failed");
    check(fresh.waqfTypeRepairs.isEmpty(), "Fresh page load retained stale cache-restoration records");
    const auto expected = worldBox(stop); bool found = false;
    const auto& freshWord = fresh.page.lines[6].words[4];
    for (int pi : freshWord.wordResultInfo.subWords[1].paths) {
        const auto& path = freshWord.paths[pi]; const auto actual = worldBox(path);
        if (QLineF(expected.topLeft(), actual.topLeft()).length() < .01 &&
            QLineF(expected.bottomRight(), actual.bottomRight()).length() < .01) {
            check(path.type == CustomDataType::WAQF, "Fresh parsing omitted the WAQF flag"); found = true;
        }
    }
    check(found, "Fresh source parsing assigned the stop sign to another word/subword");
    qInfo() << "PASS: stop-sign distance" << distance
            << "; confusing marks rejected; scale/translation controls; cached/fresh PDF; exact ink; idempotence; serialization";
}
