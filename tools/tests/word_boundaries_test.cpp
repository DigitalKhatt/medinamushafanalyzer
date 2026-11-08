#include "pageanalysisresult.h"
#include "afont.h"
#include "GlobalParams.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>

static void check(bool ok, const QString& message) { if (!ok) qFatal("%s", qPrintable(message)); }
static QByteArray shapeKey(Apath path) {
    // Native stop-sign tagging can restore 0 -> WAQF independently of ownership.
    // Every other source field, including packed-path types, remains in the key.
    if (path.type == CustomDataType::WAQF) path.type = CustomDataType(0);
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream << path; return bytes;
}
static QByteArray pageKey(const APage& page) { QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream << page; return bytes; }
static QList<QByteArray> allShapes(const APage& page) {
    QList<QByteArray> shapes;
    for (const auto& line : page.lines) for (const auto& word : line.words)
        for (const auto& path : word.paths) shapes.append(shapeKey(path));
    std::sort(shapes.begin(), shapes.end()); return shapes;
}
static PageAnalysisResult readPage(int pageNumber) {
    QFile file(QString("pages/page%1.dat").arg(pageNumber));
    check(file.open(QIODevice::ReadOnly), "Missing native page cache");
    PageAnalysisResult result; QDataStream stream(&file); stream >> result.page;
    check(stream.status() == QDataStream::Ok, "Invalid native page cache"); return result;
}
static bool owns(const Word& word, const QByteArray& key) {
    for (const auto& path : word.paths) if (shapeKey(path) == key) return true;
    return false;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    globalParams = std::make_unique<GlobalParams>();
    PageAnalysisResult::initQuranText();
    AFont font; QFile file("input/font.dat"); check(file.open(QIODevice::ReadOnly), "Missing font");
    QDataStream stream(&file); stream >> font; check(stream.status() == QDataStream::Ok, "Invalid font");
    QJsonArray changes;
    for (int number = 1; number <= 604; ++number) {
        auto source = readPage(number); const auto before = source.page;
        const auto repairs = source.resolveWordBoundaries(number, &font);
        if (!repairs.isEmpty()) {
            check(allShapes(before) == allShapes(source.page), "Ownership repair changed source ink or path metadata");
            for (int li = 0; li < before.lines.size(); ++li)
                for (int wi = 0; wi < before.lines[li].words.size(); ++wi)
                    check(before.lines[li].words[wi].paths.size() == source.page.lines[li].words[wi].paths.size(), "Word contour quota changed");
            for (const auto& value : repairs) { auto item = value.toObject(); item["page"] = number; changes.append(item); }
        }
        const auto after = pageKey(source.page);
        check(source.resolveWordBoundaries(number, &font).isEmpty() && pageKey(source.page) == after,
              "Boundary repair is not idempotent");
        if (number % 100 == 0) qInfo() << "Checked native cached pages:" << number;
    }
    check(changes.size() == 2, "Unexpected repairs elsewhere in the 604-page corpus");
    for (const auto scenario : {QVector<int>{41, 2, 5, 9, 0}, QVector<int>{158, 4, 1, 6, 0}}) {
        const int number = scenario[0], li = scenario[1], wi = scenario[2];
        auto source = readPage(number); const auto before = source.page;
        const auto lower = shapeKey(before.lines[li].words[wi].paths[scenario[3]]);
        const auto upper = shapeKey(before.lines[li].words[wi+1].paths[scenario[4]]);
        check(source.resolveWordBoundaries(number, &font).size() == 1, "Known boundary was not resolved");
        check(owns(source.page.lines[li].words[wi], upper) && owns(source.page.lines[li].words[wi+1], lower),
              "Marks were assigned to the wrong words");
        auto ambiguous = readPage(number);
        auto& ambiguousWord = ambiguous.page.lines[li].words[wi+1];
        ambiguousWord.paths.append(ambiguousWord.paths[scenario[4]]);
        const auto ambiguousInk = allShapes(ambiguous.page);
        check(ambiguous.resolveWordBoundaries(number, &font).isEmpty() && allShapes(ambiguous.page) == ambiguousInk,
              "An ambiguous boundary was changed");
        auto invalidQuota = readPage(number);
        invalidQuota.page.lines[li].words[wi].wordResultInfo.state |= WordResultFlags::DIFFERENT_PATH_NUMBER;
        check(invalidQuota.resolveWordBoundaries(number, &font).isEmpty(), "An invalid contour quota justified a repair");
        // The same rule must work at another page number, position and scale.
        auto moved = readPage(number);
        const auto savedTranscript = PageAnalysisResult::QuranTextByWord[2];
        PageAnalysisResult::QuranTextByWord[2] = PageAnalysisResult::QuranTextByWord[number-1];
        for (auto& line : moved.page.lines) {
            line.baseline = line.baseline * 1.7 + 31;
            for (auto& word : line.words) for (auto& path : word.paths) {
                path.transform = path.transform * QTransform::fromScale(1.7, 1.7);
                path.pos = path.pos * 1.7 + QPointF(17, 31);
            }
        }
        check(moved.resolveWordBoundaries(3, &font).size() == 1, "Repair depends on a page ID, absolute position or scale");
        PageAnalysisResult::QuranTextByWord[2] = savedTranscript;
        PageAnalysisResult fresh;
        check(fresh.loadPage(number, &font, false) == 0, "Fresh PDF segmentation failed");
        check(fresh.resolveWordBoundaries(number, &font).isEmpty(), "Fresh PDF segmentation omitted the ownership rule");
        auto worldBox = [](const Apath& path) { auto world=path.transform.map(path.path); world.translate(path.pos); return world.boundingRect(); };
        for (int target : {wi, wi+1}) {
            const auto expected = worldBox(before.lines[li].words[target == wi ? wi+1 : wi].paths[target == wi ? scenario[4] : scenario[3]]);
            bool found = false;
            for (const auto& path : fresh.page.lines[li].words[target].paths) {
                const auto box = worldBox(path);
                if (QLineF(box.topLeft(), expected.topLeft()).length() < 0.01 &&
                    QLineF(box.bottomRight(), expected.bottomRight()).length() < 0.01) found = true;
            }
            check(found, "Fresh PDF segmentation assigned a boundary mark incorrectly");
        }
    }
    qInfo().noquote() << QJsonDocument(changes).toJson(QJsonDocument::Compact);
    qInfo() << "PASS: all 604 pages; two exchanges; exact ink conservation; idempotence; transformed controls; fresh PDF segmentation";
}
