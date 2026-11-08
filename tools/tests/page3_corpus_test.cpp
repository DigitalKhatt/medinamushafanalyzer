#include "pageanalysisresult.h"
#include <QApplication>
#include <QFile>

static void check(bool passed, const QString& message) {
    if (!passed) qFatal("%s", qPrintable(message));
}

static QImage mask(const QPainterPath& path, const QRectF& box) {
    constexpr double scale = 16;
    QImage image(int(std::ceil(box.width() * scale)) + 4,
                 int(std::ceil(box.height() * scale)) + 4, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.translate(2, 2);
    painter.scale(scale, scale);
    painter.translate(-box.topLeft());
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(path);
    return image;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    check(!config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool(),
          "The page check must run with the tracker disabled");
    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 cache is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page 3 cache");
    int total = 0, connected = 0, partitionErrors = 0;
    for (int li = 0; li < analysis.page.lines.size(); ++li) {
        auto& line = analysis.page.lines[li];
        for (int wi = 0; wi < line.words.size(); ++wi) {
            auto& word = line.words[wi];
            for (int si = 0; si < word.wordResultInfo.subWords.size(); ++si) {
                const auto id = QString("page003:l%1-w%2-s%3").arg(li + 1).arg(wi + 1).arg(si + 1);
                check(analysis.segmentSubword(2, li, wi, si, nullptr, true), id + ": still partial");
                const auto& sub = word.wordResultInfo.subWords[si];
                QString letters;
                for (const auto ch : sub.text) if (!ch.isMark()) letters += ch;
                ++total;
                if (letters.size() < 2) continue;
                ++connected;
                check(sub.baseGlyphs.size() == letters.size(), id + ": wrong number of bodies");
                const auto& shape = word.paths[sub.paths.front()];
                auto source = shape.path;
                source.setFillRule(Qt::WindingFill);
                const auto box = source.boundingRect();
                const auto sourceMask = mask(source, box);
                QVector<QImage> parts;
                for (const auto& body : sub.baseGlyphs) parts.append(mask(body.path, box));
                for (const auto& kashida : sub.kashidaPaths)
                    if (!kashida.isEmpty()) parts.append(mask(kashida, box));
                int ink = 0, missing = 0, extra = 0, overlap = 0;
                for (int y = 0; y < sourceMask.height(); ++y) {
                    const auto original = reinterpret_cast<const QRgb*>(sourceMask.constScanLine(y));
                    for (int x = 0; x < sourceMask.width(); ++x) {
                        int count = 0;
                        for (const auto& part : parts)
                            count += qRed(reinterpret_cast<const QRgb*>(part.constScanLine(y))[x]) != 0;
                        const bool present = qRed(original[x]) != 0;
                        ink += present;
                        missing += present && count == 0;
                        extra += !present && count != 0;
                        overlap += count > 1;
                    }
                }
                // Separate rasterizations can differ on a shared curve boundary.
                const int tolerance = std::max(8, ink / 500);
                if (missing > tolerance || extra > tolerance || overlap > tolerance) {
                    ++partitionErrors;
                    qWarning().noquote() << id + QString(": ink partition error: %1 missing, %2 extra, %3 overlap / %4 ink")
                        .arg(missing).arg(extra).arg(overlap).arg(ink);
                }

                // These two newly supported necks previously clipped the waw
                // head while assigning its tail to the connecting stroke.
                if ((li == 7 && wi == 7 && si == 0) || (li == 11 && wi == 1 && si == 2)) {
                    const auto outline = shape.transform.map(source).boundingRect();
                    const auto tail = shape.transform.map(sub.baseGlyphs.back().path).boundingRect();
                    check(std::abs(tail.left() - outline.left()) < 1e-6 &&
                          std::abs(tail.bottom() - outline.bottom()) < 1e-6,
                          id + ": final waw lost part of its tail");
                }
                if ((li == 6 && wi == 8 && si == 0) || (li == 8 && wi == 7 && si == 0)) {
                    // In both qaf-yeh-lam controls, the final lam owns the
                    // only tall ascender, not just its bowl's short return.
                    auto world = shape.transform.map(source);
                    std::vector<QPainterPath> paths;
                    QVector<PathExtrema> extrema;
                    ShapeItem::getExtrema(world, paths, extrema);
                    const auto crest = std::min_element(extrema.begin(), extrema.end(), [](const auto& a, const auto& b) {
                        return a.point.y() < b.point.y();
                    });
                    const auto body = shape.transform.map(sub.baseGlyphs.back().path);
                    check(crest != extrema.end() && body.contains(crest->point + QPointF(0, .05)) &&
                          std::abs(body.boundingRect().left() - world.boundingRect().left()) < 1e-6 &&
                          std::abs(body.boundingRect().bottom() - world.boundingRect().bottom()) < 1e-6,
                          id + ": final lam lost its ascender or bowl to the kashida");
                }
                const auto bodies = sub.baseGlyphs;
                const auto kashidas = sub.kashidaPaths;
                const auto diagnostics = sub.letterDiagnostics;
                check(analysis.segmentSubword(2, li, wi, si, nullptr, true), id + ": repeat became partial");
                check(sub.baseGlyphs.size() == bodies.size() && sub.kashidaPaths == kashidas &&
                      sub.letterDiagnostics == diagnostics, id + ": repeated segmentation changed");
                for (size_t i = 0; i < bodies.size(); ++i)
                    check(sub.baseGlyphs[i].path == bodies[i].path, id + ": repeated body mask changed");
            }
        }
    }
    check(total == 307 && connected == 151, "Page 3 corpus coverage changed unexpectedly");
    check(partitionErrors == 0, "Body masks do not partition the source ink");
    qInfo("Page 3 passed: 307/307 complete, 151 connected subwords, ink conservation, nonoverlap, deterministic masks and final-waw tail ownership; tracker disabled.");
}
