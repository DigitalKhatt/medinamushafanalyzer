#include "proposalexport.h"
#include "pathmask.h"
#include "afont.h"
#include "lettersegmentationcache.h"
#include "markassignment.h"
#include "quran.h"
#include "PDFDoc.h"
#include "CustomOutputDev.h"
#include "nativeinputversion.h"
#include <QRegularExpression>
#include <stdexcept>

namespace {
QByteArray readFile(const QString& name) {
    QFile file(name);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(("Cannot read " + name).toStdString());
    return file.readAll();
}
QString digest(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
QSizeF pdfCropSize(const QString& name) {
    PDFDoc pdf(std::make_unique<GooString>(name.toStdString()));
    if (!pdf.isOk() || pdf.getNumPages()!=1) throw std::runtime_error("Cannot read source AI/PDF");
    const QSizeF crop(pdf.getPageCropWidth(1), pdf.getPageCropHeight(1));
    if (crop.isEmpty()) throw std::runtime_error("Invalid PDF crop box");
    return crop;
}
QJsonArray pdfViewBox(const QSizeF& crop) {
    constexpr double cssPixelsPerPoint = 96.0 / 72.0;
    return {0, 0, crop.width()*cssPixelsPerPoint, crop.height()*cssPixelsPerPoint};
}
QJsonObject pageRaster(const QJsonArray& viewBox) {
    return {{"width",qRound(viewBox[2].toDouble()*4)},{"height",qRound(viewBox[3].toDouble()*4)},
            {"scale",4},{"threshold",170},{"chroma",45},{"viewBox",viewBox}};
}
QJsonArray pdfToRaster(const QSizeF& crop, int width, int height) {
    return {width/crop.width(),0,0,height/crop.height(),0,0};
}
QString artworkPath(int page) {
    return QString("input/1441-AI-hafs/%1___Hafs39__DM.ai").arg(page,3,10,QChar('0'));
}
void writeJson(const QString& name, const QJsonObject& value) {
    QSaveFile file(name);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(value).toJson(QJsonDocument::Compact)) < 0 || !file.commit())
        throw std::runtime_error(("Cannot write " + name).toStdString());
}
QJsonArray box(const QRectF& b) { return {b.x(), b.y(), b.width(), b.height()}; }
QPainterPath worldPath(const Apath& shape) {
    auto path = shape.transform.map(shape.path);
    path.translate(shape.pos);
    return path;
}
QString signature(const QPainterPath& path) {
    const auto b=path.boundingRect();
    if (b.isEmpty()) return QString(64, '0');
    QImage image(16,16,QImage::Format_RGB32); image.fill(Qt::white);
    QPainter painter(&image);
    painter.scale(16/b.width(),16/b.height()); painter.translate(-b.left(),-b.top());
    painter.fillPath(path,Qt::black); painter.end();
    QString bits;
    for (int y=0;y<16;++y) for (int x=0;x<16;x+=4) {
        int value=0;
        for (int k=0;k<4;++k) value=(value<<1) | (qRed(image.pixel(x+k,y))<128);
        bits+=QString::number(value,16);
    }
    return bits;
}
// Rasterize the full filled path, including counters and disconnected contours.
// The crop is only an allocation optimization; run offsets are page coordinates.
QJsonArray runs(const QPainterPath& path, int width, int height, qreal sx, qreal sy) {
    const auto scaled = QTransform::fromScale(sx, sy).map(path);
    const auto b = scaled.boundingRect().toAlignedRect().adjusted(-2,-2,2,2).intersected(QRect(0,0,width,height));
    QJsonArray result;
    visitSourceInk(scaled, b, [&](int x, int y, int length) {
        result.append(y * width + x);
        result.append(length);
    });
    return result;
}
QList<int> parsePages(const QString& selection) {
    QSet<int> numbers;
    for (const auto& part : selection.split(',')) {
        auto limits = (part == "all" ? QString("1-604") : part).split('-');
        bool ok1=false, ok2=false;
        int first=limits[0].toInt(&ok1), last=limits.size()==1 ? first : limits.value(1).toInt(&ok2);
        if (!ok1 || (limits.size()!=1 && !ok2) || limits.size()>2 || first<1 || last>604 || last<first)
            throw std::runtime_error("Pages must be 1-604, comma-separated pages/ranges, or all");
        for (int n=first;n<=last;++n) numbers.insert(n);
    }
    auto values=numbers.values(); std::sort(values.begin(),values.end()); return values;
}
}
static QJsonObject transcriptPage(int number) {
        const QStringList positions={"isolated","final","medial","initial","unknown"};
            const auto id=QString("page%1").arg(number,3,10,QChar('0'));
            const auto& lines=PageAnalysisResult::QuranTextByWord[number-1];
            QJsonArray words;
            for (int li=0; li<lines.size(); ++li) {
                for (int wi=0; wi<lines[li].size(); ++wi) {
                    const auto& text=lines[li][wi];
                    const auto joins=PageAnalysisResult::arabic_joining(text);
                    QJsonArray characters;
                    for (int ci=0; ci<text.size(); ++ci) {
                        const auto prop=PageAnalysisResult::charProps.constFind(text[ci].unicode());
                        if (prop==PageAnalysisResult::charProps.cend())
                            throw std::runtime_error("Transcript character has no native contour count");
                        characters.append(QJsonObject{{"text",QString(text[ci])},
                            {"position",positions.value((int)joins[ci].joining,"unknown")},
                            {"nativeContourCount",int(prop->nb_paths[joins[ci].joining])},
                            {"nativeLowerContourCount",PageAnalysisResult::expectedLowerPathCount(text[ci].unicode(),joins[ci].joining)}});
                    }
                    words.append(QJsonObject{{"id",QString("%1:l%2-w%3").arg(id).arg(li+1).arg(wi+1)},
                        {"line",li+1},{"ordinal",wi+1},{"text",text},{"characters",characters}});
                }
            }
        return {{"pageId",id},{"rawText",QString::fromUtf8(qurantext[number-1])},{"words",words}};
}

int exportReviewTranscript(const QString& output, const QString& selection) {
    try {
        PageAnalysisResult::initQuranText();
        QJsonArray pages;
        for (int number : parsePages(selection)) pages.append(transcriptPage(number));
        writeJson(output,{{"schemaVersion",1},{"producer","medinamushafanalyzer-QuranTextByWord-v1"},
            {"source","quran.cpp via PageAnalysisResult::initQuranText()"},
            {"contourCountSource","PageAnalysisResult::charProps, indexed by native joining form; includes the base body"},
            {"lowerContourCountSource","PageAnalysisResult::expectedLowerPathCount, shared with repairSubwordBoundaries"},
            {"pages",pages}});
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}

int exportPageRasters(const QString& output, const QString& selection) {
    try {
        QJsonArray pages;
        for (int page : parsePages(selection)) {
            const auto path = artworkPath(page);
            const auto crop = pdfCropSize(path);
            const auto raster = pageRaster(pdfViewBox(crop));
            const auto transform = pdfToRaster(crop, raster.value("width").toInt(), raster.value("height").toInt());
            pages.append(QJsonObject{{"id",QString("page%1").arg(page,3,10,QChar('0'))},
                                     {"sha256",digest(readFile(path))},{"raster",raster},{"pdfToRaster",transform}});
        }
        writeJson(output, {{"schemaVersion",1},{"sourceFormat","ai-pdf"},{"pages",pages}});
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

int exportReviewProposals(const QString& output, const QString& svgDirectory, const QString& selection,
                         bool includeTemplateScores, const QString& markOutput, bool fresh) {
    try {
        AFont font;
        const auto fontBytes=readFile("input/font.dat");
        QDataStream fontStream(fontBytes); fontStream >> font;
        if (fontStream.status()!=QDataStream::Ok || font.glyphs.isEmpty()) throw std::runtime_error("Invalid font template cache");
        QStringList names;
        QJsonObject templateCounts;
        for (auto it=font.glyphs.begin();it!=font.glyphs.end();++it) {
            if (!names.contains(it.key())) names.append(it.key());
            templateCounts[it.key()] = templateCounts[it.key()].toInt()+1;
        }
        QDir().mkpath(output);
        if (!markOutput.isEmpty()) QDir().mkpath(markOutput);
        writeJson(output+"/templates.json", {{"sha256",digest(fontBytes)},{"counts",templateCounts}});
        PageAnalysisResult::initQuranText();
        QJsonArray index, failures;
        for (int number : parsePages(selection)) {
            const QString id=QString("page%1").arg(number,3,10,QChar('0'));
            try {
                const QString pdfName=artworkPath(number);
                const auto crop=pdfCropSize(pdfName);
                const double pw=crop.width(), ph=crop.height();
                const auto pdfHash=digest(readFile(pdfName));
                auto sourceHash=pdfHash;
                // Keep the same mask grid as the SVG export, in 96 dpi CSS units.
                QJsonArray viewBox=pdfViewBox(crop);
                if (!svgDirectory.isEmpty()) {
                    const auto svgBytes=readFile(svgDirectory+"/"+id+".svg");
                    sourceHash=digest(svgBytes);
                    QXmlStreamReader xml(svgBytes);
                    viewBox=QJsonArray();
                    while (!xml.atEnd()) {
                        xml.readNext();
                        if (xml.isStartElement() && xml.name()=="svg") {
                            for (auto value:xml.attributes().value("viewBox").toString().split(QRegularExpression("[ ,]+"),Qt::SkipEmptyParts)) viewBox.append(value.toDouble());
                            break;
                        }
                    }
                    if (viewBox.size()!=4 || viewBox[0].toDouble()!=0 || viewBox[1].toDouble()!=0) throw std::runtime_error("Unsupported SVG viewBox");
                }
                const auto raster=pageRaster(viewBox);
                const int width=raster.value("width").toInt(),height=raster.value("height").toInt();
                QByteArray cacheBytes;
                PageAnalysisResult result;
                if (fresh) {
                    if (result.loadPage(number, &font, false) < 0 || result.page.lines.isEmpty())
                        throw std::runtime_error("Fresh source segmentation failed");
                } else {
                    cacheBytes=readFile(QString("pages/page%1.dat").arg(number));
                    QDataStream cache(cacheBytes); cache >> result.page;
                    if (cache.status()!=QDataStream::Ok || result.page.lines.isEmpty()) throw std::runtime_error("Invalid page cache");
                }
                const auto boundaryRepairs=result.resolveWordBoundaries(number, &font);
                QJsonArray words, paths;
                for (int li=0;li<result.page.lines.size();++li) {
                    auto& line=result.page.lines[li];
                    for (int wi=0;wi<line.words.size();++wi) {
                        auto& word=line.words[wi];
                        QString wordId=QString("l%1-w%2").arg(li+1).arg(wi+1);
                        QJsonArray subwords, wordPaths;
                        QMap<int,int> membership;
                        for (int si=0;si<(int)word.wordResultInfo.subWords.size();++si) {
                            const auto& sub=word.wordResultInfo.subWords[si];
                            QJsonArray ids; QRectF bounds;
                            for (int pi:sub.paths) {
                                if (pi<0 || pi>=word.paths.size()) continue;
                                ids.append(wordId+QString("-p%1").arg(pi));
                                bounds=bounds.united(worldPath(word.paths[pi]).boundingRect());
                                // Duplicate membership is exported as ambiguous, never silently overwritten.
                                membership[pi]=membership.contains(pi) ? -1 : si;
                            }
                            auto b=QTransform::fromScale(width/pw,height/ph).mapRect(bounds);
                            QJsonArray chars;
                            auto joins=PageAnalysisResult::arabic_joining(sub.text);
                            const QStringList positions={"isolated","final","medial","initial","unknown"};
                            for (int ci=0;ci<sub.text.size();++ci)
                                chars.append(QJsonObject{{"text",QString(sub.text[ci])},{"position",positions.value((int)joins[ci].joining,"unknown")}});
                            subwords.append(QJsonObject{{"id",wordId+QString("-s%1").arg(si+1)},{"text",sub.text},{"characters",chars},{"pathIds",ids},{"bbox",box(b)}});
                        }
                        for (int pi=0;pi<word.paths.size();++pi) {
                            const auto& shape=word.paths[pi];
                            auto world=worldPath(shape);
                            QJsonObject scores;
                            // Font bank matching already used by the legacy analyzer. Scores are
                            // geometric distances, not probabilities; transcript assignment follows.
                            if (includeTemplateScores) for (const auto& name:names) {
                                double score=1;
                                font.checkGlyph(shape.path,name,CompareMethod::AREA,1.1,&score);
                                if (std::isfinite(score) && score<0.65) scores[name]=score;
                            }
                            const auto pathId=wordId+QString("-p%1").arg(pi);
                            const int si=membership.value(pi,-1);
                            paths.append(QJsonObject{{"id",pathId},{"wordId",wordId},{"line",li+1},
                                {"subwordId",si>=0 ? QJsonValue(wordId+QString("-s%1").arg(si+1)) : QJsonValue()},
                                {"legacyType",(int)shape.type},{"contourCount",1+shape.nbpath},
                                {"signature",signature(world)},
                                {"bbox",box(QTransform::fromScale(width/pw,height/ph).mapRect(world.boundingRect()))},
                                {"runs",runs(world,width,height,width/pw,height/ph)},{"templateScores",scores}});
                            wordPaths.append(pathId);
                        }
                        const auto expected=PageAnalysisResult::QuranTextByWord.value(number-1).value(li).value(wi);
                        words.append(QJsonObject{{"id",wordId},{"line",li+1},{"index",wi+1},{"text",word.text},{"baseline",line.baseline*height/ph},
                            {"transcriptMatches",word.text==expected && !expected.isEmpty()},
                            {"legacyFlags",(int)word.wordResultInfo.state},{"subwords",subwords},{"pathIds",wordPaths}});
                    }
                }
                QJsonObject doc{{"schemaVersion",1},{"pageId",id},{"sourceSha256",sourceHash},
                    {"raster",raster},
                    {"provenance",QJsonObject{{"producer","medinamushafanalyzer-vector-v1"},{"pdfSha256",pdfHash},
                        {"cacheSha256",fresh ? QJsonValue() : QJsonValue(digest(cacheBytes))},{"templateSha256",digest(fontBytes)},
                        {"wordBoundaryVersion",1},{"wordBoundaryRepairs",boundaryRepairs},
                        {"subwordBoundaryVersion",1},{"subwordBoundaryRepairs",result.subwordBoundaryRepairs},
                        {"waqfTypeVersion",1},{"waqfTypeRepairs",result.waqfTypeRepairs},
                        {"pdfCropSize",QJsonArray{pw,ph}},{"pdfToRaster",pdfToRaster(crop,width,height)},
                        {"cacheOrigin",fresh ? "fresh AI/PDF segmentation; no page cache read or written" : "legacy cache; geometry must be verified against source SVG"}}},
                    {"words",words},{"paths",paths}};
                if (svgDirectory.isEmpty()) {
                    auto provenance=doc["provenance"].toObject();
                    provenance["sourceFormat"]="ai-pdf";
                    doc["provenance"]=provenance;
                }
                writeJson(output+"/"+id+".raw.json",doc);
                if (!markOutput.isEmpty())
                    writeJson(markOutput+"/"+id+".assignment.json",assignNativeMarks(doc,transcriptPage(number)));
                index.append(QJsonObject{{"pageId",id},{"words",words.size()},{"paths",paths.size()}});
                std::cout << id.toStdString() << ": " << words.size() << " words, " << paths.size() << " paths" << std::endl;
            } catch (const std::exception& error) {
                failures.append(QJsonObject{{"pageId",id},{"error",error.what()}});
                std::cerr << id.toStdString() << ": " << error.what() << std::endl;
            }
        }
        writeJson(output+"/raw-manifest.json",{{"pages",index},{"failures",failures}});
        return failures.isEmpty()?0:1;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}

int exportReviewBaselines(const QString& output, const QString& selection) {
    try {
        QJsonObject pages;
        for (int number:parsePages(selection)) {
            const auto bytes=readFile(QString("pages/page%1.dat").arg(number));
            APage page;QDataStream stream(bytes);stream >> page;
            if (stream.status()!=QDataStream::Ok) throw std::runtime_error("Invalid baseline cache");
            QJsonObject baselines;
            for (int i=0;i<page.lines.size();++i)
                if (std::isfinite(page.lines[i].baseline)) baselines[QString::number(i+1)]=page.lines[i].baseline;
            pages[QString("page%1").arg(number,3,10,QChar('0'))]=QJsonObject{{"cacheSha256",digest(bytes)},{"pdfBaselines",baselines}};
        }
        writeJson(output,{{"schemaVersion",1},{"pages",pages}});
        return 0;
    } catch (const std::exception& error) {std::cerr << error.what() << std::endl;return 1;}
}

int exportLetterTraining(const QString& output, const QString& selection) {
    try {
        AFont font;
        QDataStream fontStream(readFile("input/font.dat")); fontStream >> font;
        if (fontStream.status() != QDataStream::Ok) throw std::runtime_error("Invalid font cache");
        PageAnalysisResult::initQuranText();
        QDir().mkpath(output);
        QJsonArray manifest;
        for (int number : parsePages(selection)) {
            const auto id = QString("page%1").arg(number, 3, 10, QChar('0'));
            const auto rawBytes = readFile("output/vector-review/" + id + ".raw.json");
            const auto raw = QJsonDocument::fromJson(rawBytes).object();
            const auto provenance = raw.value("provenance").toObject();
            const auto raster = raw.value("raster").toObject();
            const auto transform = provenance.value("pdfToRaster").toArray();
            const auto bytes = readFile(QString("pages/page%1.dat").arg(number));
            if (digest(bytes) != provenance.value("cacheSha256").toString() || transform.size() != 6)
                throw std::runtime_error((id + ": source provenance mismatch").toStdString());
            PageAnalysisResult source;
            QDataStream stream(bytes); stream >> source.page;
            if (stream.status() != QDataStream::Ok) throw std::runtime_error("Invalid page cache");
            source.resolveWordBoundaries(number, &font);
            auto cache = loadLetterSegmentation(source, number, font, nullptr, true);
            QJsonArray examples;
            const int width = raster.value("width").toInt(), height = raster.value("height").toInt();
            const double sx = transform[0].toDouble(), sy = transform[3].toDouble();
            for (const auto& value : cache.value("subwords").toArray()) {
                const auto item = value.toObject();
                const int li = item.value("line").toInt()-1, wi = item.value("word").toInt()-1,
                          si = item.value("subword").toInt()-1;
                const auto& word = source.page.lines[li].words[wi];
                const auto& sub = word.wordResultInfo.subWords[si];
                if (sub.paths.empty()) continue;
                const auto path = worldPath(word.bodyShape(sub));
                QJsonArray regions;
                for (const auto& body : item.value("bodies").toArray())
                    regions.append(QJsonObject{{"role", "body"}, {"letter", body.toObject().value("letter")},
                        {"labelSource", body.toObject().value("labelSource")},
                        {"reviewRequired", body.toObject().value("reviewRequired").toBool()},
                        {"runs", runs(letterSegmentationPath(body.toObject()), width, height, sx, sy)}});
                for (const auto& join : item.value("joins").toArray()) {
                    const auto kashida = join.toObject().value("kashida").toObject();
                    if (!kashida.isEmpty()) regions.append(QJsonObject{{"role", "kashida"},
                        {"runs", runs(letterSegmentationPath(kashida), width, height, sx, sy)}});
                }
                auto example = item;
                example.remove("bodies"); example.remove("joins");
                example.insert("id", QString("%1:l%2-w%3-s%4").arg(id).arg(li+1).arg(wi+1).arg(si+1));
                example.insert("sourceRuns", runs(path, width, height, sx, sy));
                example.insert("regions", regions);
                examples.append(example);
            }
            QJsonObject document{{"schemaVersion", 1}, {"page", id}, {"provenance", "machine-weak"},
                {"sourceSha256", raw.value("sourceSha256")}, {"sourceCacheSha256", digest(bytes)},
                {"rawProposalSha256", digest(rawBytes)}, {"pdfToRaster", transform}, {"raster", raster},
                {"templateSha256", cache.value("letterFormsSha256")}, {"cacheVersion", cache.value("version")},
                {"examples", examples}};
            writeJson(output + "/" + id + ".json", document);
            manifest.append(QJsonObject{{"page", id}, {"examples", examples.size()}});
            std::cout << id.toStdString() << ": " << examples.size() << " subwords" << std::endl;
        }
        writeJson(output + "/manifest.json", {{"schemaVersion", 1}, {"provenance", "machine-weak"}, {"pages", manifest}});
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}

namespace {
// Keep editable Béziers in catalog raster coordinates, including counter winding.
QJsonObject catalogOutline(const QPainterPath& path) {
    QString d;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        d += e.isMoveTo() ? 'M' : e.isLineTo() ? 'L' : e.isCurveTo() ? 'C' : ' ';
        d += QString::number(e.x, 'g', 10) + ' ' + QString::number(e.y, 'g', 10);
    }
    return {{"d",d}, {"fillRule",path.fillRule() == Qt::WindingFill ? "nonzero" : "evenodd"}};
}

using InkSpans = QVector<QPair<int,int>>;

QVector<QPainterPath> catalogContours(const QPainterPath& path) {
    QVector<QPainterPath> result;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        if (e.isMoveTo()) {
            result.append(QPainterPath(QPointF(e.x, e.y)));
        } else if (e.isLineTo()) {
            result.last().lineTo(e.x, e.y);
        } else if (e.isCurveTo()) {
            const auto control = path.elementAt(++i), end = path.elementAt(++i);
            result.last().cubicTo(e.x, e.y, control.x, control.y, end.x, end.y);
        }
    }
    return result;
}

QJsonObject catalogPageArt(const QString& pdfName, const PageAnalysisResult& page, int number,
                           const QSize& size, const QTransform& toRaster) {
    PDFDoc pdf(std::make_unique<GooString>(pdfName.toStdString()));
    CustomOutputDev device;
    device.startDoc(&pdf);
    pdf.displayPageSlice(&device, 1, 72, 72, 0, false, true, false, -1, -1, -1, -1);
    QJsonArray paint, headings, ayahs;
    QJsonObject opening;
    int frame = -1;
    QVector<QPair<QRectF, int>> markers;
    for (const auto& item : device.getPaths()) {
        auto world = item.transform.map(item.path);
        world.translate(item.pos);
        const auto path = toRaster.map(world);
        const auto bounds = path.boundingRect();
        if (bounds.isEmpty()) continue;
        const auto color = item.brush.color();
        const bool green = color.green() - color.red() > 12 && color.green() - color.blue() > 8;
        if (green && bounds.width() > size.width() * .45 && bounds.height() > size.height() * .45) {
            const auto parts = catalogContours(path);
            auto area = [](const QPainterPath& p) { const auto b = p.boundingRect(); return b.width() * b.height(); };
            auto outer = std::max_element(parts.cbegin(), parts.cend(), [&](const auto& a, const auto& b) { return area(a) < area(b); });
            if (frame != -1 || parts.size() < 2 || path.fillRule() != Qt::OddEvenFill)
                throw std::runtime_error("Unexpected native page frame geometry");
            QPainterPath holes;
            holes.setFillRule(Qt::OddEvenFill);
            for (auto it = parts.cbegin(); it != parts.cend(); ++it) {
                if (it == outer) continue;
                if (!bounds.contains(it->boundingRect())) throw std::runtime_error("Native frame opening lies outside the frame");
                holes.addPath(*it);
            }
            opening = catalogOutline(holes);
            opening["bounds"] = box(holes.boundingRect());
            frame = paint.size();
        } else if (green && bounds.width() > .015 * size.width() && bounds.width() < .06 * size.width() &&
                   bounds.height() > 1.15 * bounds.width() && bounds.height() < 1.6 * bounds.width()) {
            markers.append({bounds, paint.size()});
        }
        auto outline = catalogOutline(path);
        outline["fill"] = color.name();
        if (color.alpha() != 255) outline["opacity"] = color.alphaF();
        paint.append(outline);
    }
    if (frame == -1) throw std::runtime_error("Native page frame is missing");
    for (auto it = page.headingPaths.cbegin(); it != page.headingPaths.cend(); ++it) {
        const auto text = PageAnalysisResult::quranText[number-1].value(it.key());
        QVector<QJsonObject> paths;
        for (const auto& path : it.value()) paths.append(catalogOutline(toRaster.map(path)));
        std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
            const auto ad = a.value("d").toString(), bd = b.value("d").toString();
            return ad != bd ? ad < bd : a.value("fillRule").toString() < b.value("fillRule").toString();
        });
        QJsonArray shapes;
        for (const auto& outline : paths) shapes.append(outline);
        headings.append(QJsonObject{{"kind", text.startsWith(QStringLiteral("سُورَةُ")) ? "surah-title" : "basmala"},
                                    {"line",it.key()+1},{"text",text},{"paths",shapes}});
    }
    const auto isNumber = [](const QString& text) {
        return !text.isEmpty() && std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.isDigit(); });
    };
    for (int l = 0; l < page.page.lines.size(); ++l) {
        const auto& line = page.page.lines[l];
        for (int w = 0; w < line.words.size(); ++w) {
            const auto& word = line.words[w];
            const auto digits = word.text.endsWith(QChar(0x06de)) ? word.text.chopped(1) : word.text;
            if (!isNumber(digits)) continue;
            QRectF bounds;
            QJsonArray shapes;
            auto addOutline = [&](const Apath& item) {
                const auto path = toRaster.map(worldPath(item));
                shapes.append(catalogOutline(path));
                bounds = bounds.united(path.boundingRect());
            };
            if (digits == word.text) {
                for (const auto& item : word.paths) addOutline(item);
            } else {
                // A hizb sign may share the transcript token, but owns separate ink.
                for (const auto& subword : word.wordResultInfo.subWords)
                    if (isNumber(subword.text))
                        for (int index : subword.paths) addOutline(word.paths[index]);
            }
            if (shapes.isEmpty()) throw std::runtime_error("Native ayah number has no outline");
            QJsonObject verse{{"kind","ayah"},{"line",l+1},{"word",w+1},{"text",digits},{"paths",shapes}};
            for (const auto& marker : markers) {
                if (marker.first.contains(bounds.center())) {
                    verse["marker"] = marker.second;
                    break;
                }
            }
            ayahs.append(verse);
        }
    }
    return {{"frame",frame},{"opening",opening},{"paint",paint},{"headings",headings},{"ayahs",ayahs}};
}

InkSpans unionInk(const QList<QJsonArray>& masks) {
    InkSpans spans;
    for (const auto& mask : masks) {
        for (int i = 0; i + 1 < mask.size(); i += 2) {
            const int start = mask[i].toInt();
            spans.append({start, start + mask[i+1].toInt()});
        }
    }
    std::sort(spans.begin(), spans.end());
    InkSpans joined;
    for (const auto& span : spans) {
        if (!joined.isEmpty() && span.first <= joined.last().second)
            joined.last().second = std::max(joined.last().second, span.second);
        else
            joined.append(span);
    }
    return joined;
}

QVector<QPainterPath> originalContours(const QPainterPath& path) {
    QVector<QPainterPath> contours;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        if (e.isMoveTo()) {
            contours.append(QPainterPath(QPointF(e.x,e.y)));
        } else if (e.isLineTo()) {
            contours.last().lineTo(e.x,e.y);
        } else if (e.isCurveTo()) {
            const auto b = path.elementAt(++i);
            const auto end = path.elementAt(++i);
            contours.last().cubicTo(e.x,e.y,b.x,b.y,end.x,end.y);
        }
    }
    return contours;
}

// A disconnected mark splitter can give one source path several component
// IDs. Recover the component from the actual contours, including any counter,
// and accept it only when the complete mark paints exactly its assigned ink.
QJsonArray validateCatalogMarks(const QJsonArray& marks, const QHash<QString,QPainterPath>& originals,
                                const QSize& size, QJsonObject& paths) {
    static const QRegularExpression component(R"(^(.+-p[0-9]+)-(?:stroke|dot-pair|damma)-[0-9]+$)");
    QJsonArray unresolved;
    for (const auto& entry : marks) {
        const auto mark = entry.toObject();
        const auto ids = mark.value("paths").toArray();
        const auto expected = unionInk({mark.value("runs").toArray()});
        QList<QJsonArray> known;
        QStringList missing;
        for (const auto& id : ids) {
            const auto name = id.toString();
            if (paths.contains(name)) known.append(paths.value(name).toObject().value("runs").toArray());
            else missing.append(name);
        }
        if (!expected.isEmpty() && missing.isEmpty() && unionInk(known) == expected)
            continue;
        if (!expected.isEmpty() && missing.size() == 1) {
            const auto match = component.match(missing.front());
            const auto original = originals.constFind(match.captured(1));
            if (match.hasMatch() && original != originals.cend()) {
                const auto contours = originalContours(*original);
                QJsonObject recovered;
                // Current split marks have two to four contours. Bound the
                // subset search so complex paths cannot stall a page export.
                if (contours.size() <= 8) for (int bits = 1; bits < (1 << contours.size()); ++bits) {
                    QPainterPath candidate;
                    for (int i = 0; i < contours.size(); ++i)
                        if (bits & (1 << i)) candidate.addPath(contours[i]);
                    candidate.setFillRule(original->fillRule());
                    const auto ink = runs(candidate,size.width(),size.height(),1,1);
                    if (ink.isEmpty()) continue;
                    auto combined = known;
                    combined.append(ink);
                    if (unionInk(combined) != expected) continue;
                    if (!recovered.isEmpty()) {
                        recovered = {};
                        break;
                    }
                    recovered = catalogOutline(candidate);
                    recovered.insert("runs",ink);
                }
                if (!recovered.isEmpty()) {
                    paths.insert(missing.front(),recovered);
                    continue;
                }
            }
        }
        unresolved.append(mark.value("id"));
    }
    return unresolved;
}

// Assign the original painted pixels jointly. Two separately thresholded masks can
// both claim (or both lose) an antialiased pixel along their common cut.
QString catalogParts(const QJsonObject& cut, const QJsonObject& source, const QSize& raster,
                     const QTransform& toRaster, QJsonArray& parts) {
    if (!cut.value("complete").toBool() && !cut.value("provisional").toBool()) return "incomplete";
    const auto owners = source.value("owners").toArray();
    const auto letters = source.value("letters").toString();
    const auto bodies = cut.value("bodies").toArray();
    const auto diagnostics = cut.value("letters").toArray();
    QVector<int> indices;
    for (int i = 0; i < letters.size(); ++i)
        if (letters[i].unicode() != 0x0640) indices.append(i);
    if (letters.size() != owners.size() || bodies.size() != indices.size()) return "letter-count";
    const auto bounds = cut.value("bounds").toArray();
    if (bounds.size() != 4) return "missing-body";
    const auto crop = toRaster.mapRect(QRectF(bounds[0].toDouble(), bounds[1].toDouble(),
        bounds[2].toDouble(), bounds[3].toDouble())).toAlignedRect().adjusted(-2,-2,2,2)
        .intersected(QRect(QPoint(0,0), raster));
    if (crop.isEmpty()) return "empty-body";
    QVector<QImage> coverage;
    auto add = [&](const QJsonObject& geometry, QJsonObject part) {
        const auto scaled = toRaster.map(letterSegmentationPath(geometry));
        coverage.append(sourceInkCoverage(scaled, crop));
        auto outline = catalogOutline(scaled);
        if (part.value("role") == "kashida") {
            QJsonArray contours;
            for (const auto& poly : scaled.toSubpathPolygons()) {
                QJsonArray points;
                for (const auto& p : poly) points.append(QJsonArray{p.x(),p.y()});
                contours.append(points);
            }
            outline.insert("contours",contours);
        }
        part.insert("outline",outline);
        parts.append(part);
    };
    const bool provisional = cut.value("provisional").toBool();
    for (int i = 0; i < bodies.size(); ++i) {
        const auto body = bodies[i].toObject();
        if (body.value("letter").toString() != letters.mid(indices[i],1)) return "letter-order";
        const auto diagnostic = diagnostics.at(i).toObject();
        const auto variant = diagnostic.value("variant");
        add(body, {{"role","body"}, {"owner",owners[indices[i]]},
                   {"variant",variant.isUndefined() ? diagnostic.value("template") : variant},
                   {"metrics",QJsonObject{{"quality",provisional ? "provisional" : "template"}}}});
    }
    for (const auto& value : cut.value("joins").toArray()) {
        const auto join = value.toObject();
        if (!join.contains("kashida")) continue;
        const int after = join.value("afterBody").toInt();
        if (after < 1 || after >= indices.size()) return "join-order";
        const int previous = indices[after-1], next = indices[after];
        // An explicitly transcribed tatweel owns the connecting stroke and its marks.
        if (next == previous+2 && letters[previous+1].unicode() == 0x0640) {
            add(join.value("kashida").toObject(), {{"role","body"}, {"owner",owners[previous+1]},
                {"variant","tatweel"}, {"metrics",QJsonObject{{"quality",provisional ? "provisional" : "template"}}}});
            continue;
        }
        if (next != previous+1) return "tatweel-without-stroke";
        QJsonArray caps;
        for (const auto& segment : join.value("endpoints").toArray()) {
            QJsonArray line;
            for (const auto& endpoint : segment.toArray()) {
                const auto p = endpoint.toArray();
                const auto mapped = toRaster.map(QPointF(p[0].toDouble(),p[1].toDouble()));
                line.append(QJsonArray{mapped.x(),mapped.y()});
            }
            caps.append(line);
        }
        add(join.value("kashida").toObject(), {{"role","kashida"}, {"owner",owners[previous]},
            {"nextOwner",owners[next]}, {"caps",caps}});
    }
    int bodyCount = 0;
    for (const auto& part : parts) bodyCount += part.toObject().value("role") == "body";
    if (bodyCount != owners.size()) return "tatweel-without-stroke";

    const auto sourceRuns = source.value("runs").toArray();
    QVector<QJsonArray> assigned(parts.size());
    QVector<int> lastEnd(parts.size(), -1);
    for (int i = 0; i < sourceRuns.size(); i += 2) {
        const int start = sourceRuns[i].toInt(), end = start + sourceRuns[i+1].toInt();
        for (int pixel = start; pixel < end; ++pixel) {
            const int x = pixel % raster.width() - crop.x(), y = pixel / raster.width() - crop.y();
            if (x < 0 || x >= crop.width() || y < 0 || y >= crop.height()) return "source-geometry";
            int best = -1, opacity = 0;
            for (int k = 0; k < coverage.size(); ++k) {
                const int alpha = qAlpha(coverage[k].pixel(x,y));
                if (alpha > opacity) { best = k; opacity = alpha; }
            }
            if (best < 0) return "uncovered-ink";
            auto& spans = assigned[best];
            if (lastEnd[best] == pixel && pixel % raster.width() != 0) {
                spans[spans.size()-1] = spans.last().toInt() + 1;
            } else {
                spans.append(pixel);
                spans.append(1);
            }
            lastEnd[best] = pixel + 1;
        }
    }
    for (int i = parts.size()-1; i >= 0; --i) {
        auto part = parts[i].toObject();
        if (assigned[i].isEmpty()) {
            if (part.value("role") == "body") return "empty-body-part";
            parts.removeAt(i); // A subpixel connector owns no pixels on the catalog raster.
            continue;
        }
        part.insert("runs", assigned[i]);
        parts[i] = part;
    }
    return {};
}
} // namespace

namespace {
// Only the resolved source ink is cached. Letter masks, joins and template
// decisions are deliberately absent: every run still executes the cutter.
bool restoreNativeInput(const QString& filename, const QString& key, PageAnalysisResult& page) {
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream envelope(&file);
    QString savedKey, checksum;
    QByteArray payload;
    envelope >> savedKey >> checksum >> payload;
    if (envelope.status() != QDataStream::Ok || savedKey != key || digest(payload) != checksum) return false;
    QDataStream data(payload);
    data >> page.page >> page.headingPaths;
    for (auto& line : page.page.lines) for (auto& word : line.words)
        for (auto& sub : word.wordResultInfo.subWords)
            data >> sub.secondaryBodyPath >> sub.bodyCounter;
    return data.status() == QDataStream::Ok && data.atEnd() && !page.page.lines.isEmpty();
}

void saveNativeInput(const QString& filename, const QString& key, const PageAnalysisResult& page) {
    QByteArray payload;
    QDataStream data(&payload, QIODevice::WriteOnly);
    data << page.page << page.headingPaths;
    for (const auto& line : page.page.lines) for (const auto& word : line.words)
        for (const auto& sub : word.wordResultInfo.subWords)
            data << sub.secondaryBodyPath << sub.bodyCounter;
    if (!QDir().mkpath(QFileInfo(filename).absolutePath())) return;
    QSaveFile file(filename);
    if (!file.open(QIODevice::WriteOnly)) return;
    QDataStream envelope(&file);
    envelope << key << digest(payload) << payload;
    if (data.status() == QDataStream::Ok && envelope.status() == QDataStream::Ok) file.commit();
}
} // namespace

int segmentCatalogPage(const QString& request, const QString& output) {
    try {
        QElapsedTimer timer;
        timer.start();
        qint64 previous = 0;
        QJsonObject timings;
        const auto elapsed = [&](const char* stage) {
            const auto now = timer.nsecsElapsed();
            timings.insert(stage, (now - previous) / 1e6);
            previous = now;
        };
        const auto input = QJsonDocument::fromJson(readFile(request)).object();
        const int number = input.value("page").toInt();
        const auto map = input.value("pdfToRaster").toArray();
        const auto raster = input.value("raster").toObject();
        const QSize size(raster.value("width").toInt(), raster.value("height").toInt());
        if (number < 1 || number > 604 || size.isEmpty() || map.size() != 6 || !input.value("subwords").isObject())
            throw std::runtime_error("Invalid catalog page coordinates or subwords");
        const auto pdfName = QString("input/1441-AI-hafs/%1___Hafs39__DM.ai").arg(number,3,10,QChar('0'));
        if (digest(readFile(pdfName)) != input.value("pdfSha256").toString()) throw std::runtime_error("Catalog PDF hash mismatch");
        const QTransform toRaster(map[0].toDouble(), map[1].toDouble(), map[2].toDouble(), map[3].toDouble(),
                                  map[4].toDouble(), map[5].toDouble());
        elapsed("input");
        AFont font;
        const auto fontBytes = readFile("input/font.dat");
        QDataStream stream(fontBytes); stream >> font;
        if (stream.status() != QDataStream::Ok) throw std::runtime_error("Invalid geometric font");
        font.cacheAreaComparisons();
        PageAnalysisResult::initQuranText();
        elapsed("font");
        PageAnalysisResult page;
        const QString inputKey = digest(QByteArray(NativeInputVersion) + qVersion() +
            QByteArray::number(number) + input.value("pdfSha256").toString().toUtf8() + digest(fontBytes).toUtf8());
        const auto cacheDirectory = input.value("inputCache").toString("output/native-input-cache");
        const auto cacheFile = cacheDirectory + "/" + inputKey + ".dat";
        const bool cacheEnabled = !cacheDirectory.isEmpty() && !qEnvironmentVariableIsSet("MEDINA_DISABLE_INPUT_CACHE");
        const bool cacheHit = cacheEnabled && restoreNativeInput(cacheFile, inputKey, page);
        if (!cacheHit) {
            if (page.loadPage(number, &font, false) < 0 || page.page.lines.isEmpty()) throw std::runtime_error("Cannot segment native page");
            page.resolveWordBoundaries(number, &font);
            if (cacheEnabled) saveNativeInput(cacheFile, inputKey, page);
        }
        elapsed("loadPage");
        if (input.value("artOnly").toBool()) {
            writeJson(output, {{"pageArt",catalogPageArt(pdfName,page,number,size,toRaster)}});
            return 0;
        }
        auto requested = input.value("subwords").toObject();
        const auto segmented = generateLetterSegmentation(page, number, font, &requested);
        elapsed("segmentation");
        QJsonObject results, paths;
        QHash<QString,QPainterPath> originals;
        const bool partsOnly = input.value("partsOnly").toBool();
        for (int l = 0; !partsOnly && l < page.page.lines.size(); ++l) {
            const auto& line = page.page.lines[l];
            for (int w = 0; w < line.words.size(); ++w) {
                const auto& word = line.words[w];
                for (int i = 0; i < word.paths.size(); ++i) {
                    const auto key = QString("l%1-w%2-p%3").arg(l+1).arg(w+1).arg(i);
                    const auto path = worldPath(word.paths[i]);
                    const auto scaled = toRaster.map(path);
                    originals.insert(key,scaled);
                    auto outline = catalogOutline(scaled);
                    outline.insert("runs",runs(scaled,size.width(),size.height(),1,1));
                    paths.insert(key,outline);
                }
            }
        }
        elapsed("sourceRaster");
        for (const auto& value : segmented.value("subwords").toArray()) {
            const auto cut = value.toObject();
            const int line = cut.value("line").toInt(), word = cut.value("word").toInt(), sub = cut.value("subword").toInt();
            const auto key = QString("l%1-w%2-s%3").arg(line).arg(word).arg(sub);
            if (!requested.contains(key)) continue;
            const auto item = requested.take(key).toObject();
            const auto& paths = page.page.lines[line-1].words[word-1].wordResultInfo.subWords[sub-1].paths;
            const auto pathId = paths.empty() ? QString() : QString("l%1-w%2-p%3").arg(line).arg(word).arg(paths.front());
            QJsonArray parts;
            const auto reason = paths.empty() || pathId != item.value("bodyPath").toString() ? QString("source-path")
                : catalogParts(cut, item, size, toRaster, parts);
            const auto status = !reason.isEmpty() ? reason : cut.value("provisional").toBool() ? QString("provisional") : QString("template");
            QJsonObject result{{"status",status}};
            if (reason.isEmpty()) result.insert("parts",parts);
            if (status != "template") {
                result.insert("letters", cut.value("letters"));
                result.insert("trace", cut.value("trace"));
            }
            results.insert(item.value("id").toString(),result);
        }
        for (const auto& item : requested)
            results.insert(item.toObject().value("id").toString(), QJsonObject{{"status","missing-native-subword"}});
        elapsed("partitionRaster");
        const auto unresolvedMarks = validateCatalogMarks(input.value("marks").toArray(),originals,size,paths);
        elapsed("marks");
        QJsonObject reply{{"results",results},{"paths",paths},{"unresolvedMarks",unresolvedMarks},
            {"inputCacheHit",cacheHit},{"inputVersion",NativeInputVersion},
            {"templateSha256",segmented.value("letterFormsSha256")},
            {"fontSha256",segmented.value("fontSha256")}, {"version",segmented.value("version")}};
        if (!partsOnly) reply["pageArt"] = catalogPageArt(pdfName,page,number,size,toRaster);
        elapsed("pageArt");
        reply["timingMs"] = timings;
        writeJson(output,reply);
        elapsed("write");
        if (qEnvironmentVariableIsSet("MEDINA_PROFILE")) {
            timings["total"] = timer.nsecsElapsed() / 1e6;
            writeJson(output + ".profile", {{"timingMs",timings}, {"subwords",segmented.value("profile")},
                {"inputCacheHit",cacheHit},{"inputVersion",NativeInputVersion}});
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}
