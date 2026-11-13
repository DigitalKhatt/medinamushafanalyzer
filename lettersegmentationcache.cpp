#include "lettersegmentationcache.h"
#include "pageanalysisresult.h"
#include "afont.h"

namespace {

// Bump when a change to the segmenter or exported geometry invalidates old results.
constexpr int CacheVersion = 178;

QJsonArray point(const QPointF& p) {
    return {p.x(), p.y()};
}

QJsonObject encodePath(const QPainterPath& path) {
    QJsonArray commands;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        if (e.isMoveTo()) commands.append(QJsonArray{"M", e.x, e.y});
        else if (e.isLineTo()) commands.append(QJsonArray{"L", e.x, e.y});
        else if (e.isCurveTo()) {
            const auto second = path.elementAt(++i);
            const auto end = path.elementAt(++i);
            commands.append(QJsonArray{"C", e.x, e.y, second.x, second.y, end.x, end.y});
        }
    }
    return {{"fillRule", path.fillRule() == Qt::WindingFill ? "nonzero" : "evenodd"},
            {"commands", commands}};
}

QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

} // namespace

QPainterPath letterSegmentationPath(const QJsonObject& data) {
    QPainterPath path;
    path.setFillRule(data.value("fillRule").toString() == "nonzero" ? Qt::WindingFill : Qt::OddEvenFill);
    for (const auto& item : data.value("commands").toArray()) {
        const auto command = item.toArray();
        const auto op = command.at(0).toString();
        if (op == "M" && command.size() == 3) path.moveTo(command.at(1).toDouble(), command.at(2).toDouble());
        else if (op == "L" && command.size() == 3) path.lineTo(command.at(1).toDouble(), command.at(2).toDouble());
        else if (op == "C" && command.size() == 7)
            path.cubicTo(command.at(1).toDouble(), command.at(2).toDouble(),
                         command.at(3).toDouble(), command.at(4).toDouble(),
                         command.at(5).toDouble(), command.at(6).toDouble());
    }
    return path;
}

bool saveLetterSegmentation(const QString& filename, const QJsonObject& data, QString* error) {
    QSaveFile file(filename);
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) return true;
    if (error) *error = file.errorString();
    return false;
}

static QJsonObject segmentationMetadata(const PageAnalysisResult& source, int pageNumber, AFont& font) {
    QByteArray sourceBytes;
    QDataStream stream(&sourceBytes, QIODevice::WriteOnly);
    stream << source.page;
    const auto sourceHash = sha256(sourceBytes);
    QByteArray fontBytes;
    QDataStream fontStream(&fontBytes, QIODevice::WriteOnly);
    fontStream << font;
    const auto fontHash = sha256(fontBytes);
    const auto configHash = sha256(QJsonDocument(PageAnalysisResult::letterForms()).toJson(QJsonDocument::Compact));
    return {{"version", CacheVersion}, {"page", pageNumber}, {"coordinateSpace", "analyzer-page"},
            {"pageDataSha256", sourceHash}, {"letterFormsSha256", configHash},
            {"templateConfig", PageAnalysisResult::letterForms()}, {"fontSha256", fontHash}};
}

static QJsonObject encodeSubword(const Word& word, const WordResultInfo::SubWordInfo& sub,
                                 int line, int wordIndex, int subword, bool complete) {
    QJsonObject entry{{"line", line}, {"word", wordIndex},
        {"subword", subword}, {"text", sub.text}, {"complete", complete},
        {"templates", QJsonArray::fromStringList(sub.matchedTemplates)},
        {"trace", QJsonArray::fromStringList(sub.templateTrace)},
        {"letters", sub.letterDiagnostics}};
    QJsonArray bodies, joins;
    if (!sub.paths.empty() && sub.paths.front() >= 0 && sub.paths.front() < word.paths.size()) {
        const auto shape = word.bodyShape(sub);
        const auto inPage = [&](const QPainterPath& path) {
            auto mapped = shape.transform.map(path);
            mapped.translate(shape.pos);
            return mapped;
        };
        const auto bounds = inPage(shape.path).boundingRect();
        entry.insert("bounds", QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()});
        const auto letters = PageAnalysisResult::bodyLetters(sub.text);
        for (size_t bodyIndex = 0; bodyIndex < sub.baseGlyphs.size(); ++bodyIndex) {
            auto body = encodePath(inPage(sub.baseGlyphs[bodyIndex].path));
            if (complete && sub.baseGlyphs.size() == static_cast<size_t>(letters.size())) {
                body.insert("letter", letters.mid(static_cast<int>(bodyIndex), 1));
                body.insert("labelSource", "template");
            }
            bodies.append(body);
        }
        size_t joinIndex = 0;
        int afterBody = 1;
        const auto appendPenLifts = [&]() {
            while (afterBody < sub.letterDiagnostics.size() &&
                   sub.letterDiagnostics[afterBody].toObject().value("boundaryKind") == "separate-contours") {
                joins.append(QJsonObject{{"afterBody", afterBody++}, {"boundaryKind", "separate-contours"},
                                         {"endpoints", QJsonArray{}}});
            }
        };
        for (const auto& join : sub.joins) {
            appendPenLifts();
            QJsonObject cut{{"afterBody", afterBody++}, {"anchor", QJsonArray{point(join.topBaseLine.point), point(join.bottomBaseLine.point)}}};
            if (join.counterEntry.pathIndex > 0) {
                QJsonArray segments;
                for (const auto& segment : join.cutSegments())
                    segments.append(QJsonArray{point(segment.p1()), point(segment.p2())});
                cut.insert("anchor", segments.first());
                cut.insert("endpoints", segments);
                if (!join.counterBendWeights.isNull()) cut.insert("entryBridgeSegmentCount",2);
                cut.insert("boundaryKind", "shared-counter-ligature");
                cut.insert("landmarks", QJsonArray{
                    QJsonArray{join.topBaseLine.pathIndex, join.topBaseLine.time, join.counterEntry.pathIndex, join.counterEntry.time},
                    QJsonArray{join.counterExit.pathIndex, join.counterExit.time, join.bottomBaseLine.pathIndex, join.bottomBaseLine.time}
                });
            }
            if (joinIndex < sub.kashidaPaths.size() && !sub.kashidaPaths[joinIndex].isEmpty()) {
                const auto& ends = sub.kashidaCuts[joinIndex];
                cut.insert("kashida", encodePath(inPage(sub.kashidaPaths[joinIndex])));
                cut.insert("endpoints", QJsonArray{QJsonArray{point(ends.first.p1()), point(ends.first.p2())},
                                                   QJsonArray{point(ends.second.p1()), point(ends.second.p2())}});
            }
            joins.append(cut);
            ++joinIndex;
        }
        appendPenLifts();
    }
    entry.insert("bodies", bodies);
    entry.insert("joins", joins);
    return entry;
}

static QJsonObject segmentPage(const PageAnalysisResult& source, int pageNumber, AFont& font, QJsonObject result,
                               const QJsonObject* requestedSubwords = nullptr,
                               const CatalogCutCheck& check = {}) {
    PageAnalysisResult analysis;
    analysis.page = source.page;
    QJsonArray subwords;
    const bool profiling = qEnvironmentVariableIsSet("MEDINA_PROFILE");
    QJsonArray profile;
    const auto transcript = PageAnalysisResult::QuranTextByWord.value(pageNumber - 1);
    for (int lineIndex = 0; lineIndex < analysis.page.lines.size(); ++lineIndex) {
        auto& line = analysis.page.lines[lineIndex];
        for (int wordIndex = 0; wordIndex < line.words.size(); ++wordIndex) {
            auto& word = line.words[wordIndex];
            if (word.wordResultInfo.subWords.empty() && wordIndex < transcript.value(lineIndex).size())
                analysis.detectSubWords(pageNumber - 1, lineIndex, wordIndex, &font);
            const auto& results = word.wordResultInfo.subWords;
            for (size_t index = 0; index < results.size(); ++index) {
                const auto key = QString("l%1-w%2-s%3").arg(lineIndex+1).arg(wordIndex+1).arg(index+1);
                if (requestedSubwords && !requestedSubwords->contains(key)) continue;
                QElapsedTimer timer;
                if (profiling) timer.start();
                const auto accept = check ? AnchorPartitionCheck{[&](const auto& candidate) {
                    return check(key, encodeSubword(word, candidate, lineIndex + 1, wordIndex + 1, int(index + 1), true));
                }} : AnchorPartitionCheck{};
                const bool complete = analysis.segmentSubword(pageNumber - 1, lineIndex, wordIndex,
                                                              int(index), &font, true, nullptr, accept);
                if (profiling) profile.append(QJsonObject{
                    {"id",QString("page%1:l%2-w%3-s%4").arg(pageNumber,3,10,QChar('0'))
                        .arg(lineIndex+1).arg(wordIndex+1).arg(index+1)},
                    {"milliseconds",timer.nsecsElapsed()/1e6}, {"search",letterSearchProfile()}});
                const auto& sub = results[index];
                subwords.append(encodeSubword(word, sub, lineIndex + 1, wordIndex + 1, int(index + 1), complete));
            }
        }
    }
    result.insert("subwords", subwords);
    if (profiling) result.insert("profile", profile);
    return result;
}

QJsonObject generateLetterSegmentation(const PageAnalysisResult& source, int pageNumber, AFont& font,
                                       const QJsonObject* requestedSubwords, const CatalogCutCheck& check) {
    return segmentPage(source, pageNumber, font, segmentationMetadata(source, pageNumber, font),
                       requestedSubwords, check);
}

QJsonObject loadLetterSegmentation(const PageAnalysisResult& source, int pageNumber,
                                   AFont& font, bool* cacheHit, bool forceRecompute) {
    const auto metadata = segmentationMetadata(source, pageNumber, font);
    const auto directory = QStringLiteral("output/segmentation-cache");
    const auto filename = QString("%1/page%2.json").arg(directory).arg(pageNumber, 3, 10, QLatin1Char('0'));
    QFile file(filename);
    if (!forceRecompute && file.open(QIODevice::ReadOnly)) {
        const auto cached = QJsonDocument::fromJson(file.readAll()).object();
        if (cached.value("version") == metadata.value("version") && cached.value("page") == metadata.value("page") &&
            cached.value("pageDataSha256") == metadata.value("pageDataSha256") &&
            cached.value("letterFormsSha256") == metadata.value("letterFormsSha256") &&
            cached.value("fontSha256") == metadata.value("fontSha256") && cached.value("subwords").isArray()) {
            if (cacheHit) *cacheHit = true;
            return cached;
        }
        file.close();
    }
    if (cacheHit) *cacheHit = false;
    const auto result = segmentPage(source, pageNumber, font, metadata);
    if (QDir().mkpath(directory)) saveLetterSegmentation(filename, result);
    return result;
}
