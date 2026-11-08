// Read-only corpus audit. Run from a separate report runtime directory so the
// existing cache exporter cannot overwrite the native application's live cache.
#include "../segmentsubword.cpp"
#include "../lettersegmentationcache.h"
#include <QApplication>
#include <QElapsedTimer>
#include <iostream>

static QJsonObject reportPath(const QPainterPath& path) {
    QJsonArray commands;
    for(int i=0;i<path.elementCount();++i){const auto e=path.elementAt(i);
        if(e.isMoveTo())commands.append(QJsonArray{"M",e.x,e.y});
        else if(e.isLineTo())commands.append(QJsonArray{"L",e.x,e.y});
        else if(e.isCurveTo()){const auto b=path.elementAt(++i),c=path.elementAt(++i);commands.append(QJsonArray{"C",e.x,e.y,b.x,b.y,c.x,c.y});}}
    return {{"commands",commands},{"fillRule",path.fillRule()==Qt::WindingFill?"nonzero":"evenodd"}};
}

int main(int argc,char**argv) {
    QApplication app(argc,argv);Q_INIT_RESOURCE(application);
    if(argc!=4)qFatal("usage: export_incomplete_corpus SOURCE_ROOT OUTPUT_DIRECTORY COMMA_SEPARATED_PAGES");
    const QDir root(QString::fromLocal8Bit(argv[1]));const QString output=QString::fromLocal8Bit(argv[2]);
    if(QDir::cleanPath(QDir::currentPath())==QDir::cleanPath(root.absolutePath()))qFatal("Use an isolated working directory");
    const auto read=[](const QString& name){QFile file(name);if(!file.open(QIODevice::ReadOnly))qFatal("Cannot read source: %s",qPrintable(name));return file.readAll();};
    AFont font;QDataStream fontStream(read(root.filePath("input/font.dat")));fontStream>>font;
    if(fontStream.status()!=QDataStream::Ok)qFatal("Invalid source font");
    PageAnalysisResult::initQuranText();
    if(PageAnalysisResult::letterForms().value("kashidaDefaults").toObject().value("trackingEnabled").toBool())qFatal("Tracker must stay disabled");
    QDir().mkpath(output);
    for(const auto token:QString::fromLocal8Bit(argv[3]).split(',')) {
        const int page=token.toInt();if(page<1||page>604)qFatal("Invalid page");
        QElapsedTimer timer;timer.start();
        const auto bytes=read(root.filePath(QString("pages/page%1.dat").arg(page)));
        PageAnalysisResult source;QDataStream stream(bytes);stream>>source.page;
        if(stream.status()!=QDataStream::Ok)qFatal("Invalid page data");
        const auto repairs=source.resolveWordBoundaries(page,&font);
        const auto transcript=PageAnalysisResult::QuranTextByWord.value(page-1);
        QJsonArray wordInventory,lineInventory;
        for(int li=0;li<source.page.lines.size();++li){auto& line=source.page.lines[li];
            lineInventory.append(QJsonObject{{"line",li+1},{"sourceWords",line.words.size()},{"transcriptWords",transcript.value(li).size()}});
            for(int wi=0;wi<line.words.size();++wi){auto& word=line.words[wi];
                if(word.wordResultInfo.subWords.empty()&&wi<transcript.value(li).size())source.detectSubWords(page-1,li,wi,&font);
                const auto text=transcript.value(li).value(wi,word.text);
                wordInventory.append(QJsonObject{{"line",li+1},{"word",wi+1},{"text",text},{"subwords",static_cast<int>(word.wordResultInfo.subWords.size())},{"paths",word.paths.size()},{"inTranscript",wi<transcript.value(li).size()}});
            }
        }
        // The page view and exporter use the same cutter. Write the audited geometry once.
        auto cache=generateLetterSegmentation(source,page,font);
        QJsonArray rows;
        for(const auto value:cache.value("subwords").toArray()){
            auto row=value.toObject();const int li=row.value("line").toInt()-1,wi=row.value("word").toInt()-1,si=row.value("subword").toInt()-1;
            const auto& word=source.page.lines[li].words[wi];const auto& sub=word.wordResultInfo.subWords[si];
            row.insert("wordText",word.text);row.insert("baseText",PageAnalysisResult::bodyLetters(sub.text));
            row.insert("sourcePathCount",static_cast<int>(sub.paths.size()));
            if(!sub.paths.empty()&&sub.paths.front()>=0&&sub.paths.front()<word.paths.size()){
                const auto shape=word.bodyShape(sub);auto ink=shape.transform.map(shape.path);ink.translate(shape.pos);ink.setFillRule(Qt::WindingFill);
                row.insert("sourceInk",reportPath(ink));
            }
            rows.append(row);
        }
        cache.insert("subwords",rows);cache.insert("sourceWords",wordInventory);cache.insert("sourceLines",lineInventory);
        cache.insert("sourceFileSha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()));
        cache.insert("wordBoundaryRepairs",repairs);cache.insert("regenerationMilliseconds",static_cast<double>(timer.elapsed()));
        cache.insert("provenance","machine-template-audit");cache.insert("humanReviewed",false);
        QString error;const auto filename=QString("%1/page%2.json").arg(output).arg(page,3,10,QLatin1Char('0'));
        if(!saveLetterSegmentation(filename,cache,&error))qFatal("Cannot save report: %s",qPrintable(error));
        std::cout<<"page"<<page<<" "<<rows.size()<<" subwords "<<timer.elapsed()<<" ms"<<std::endl;
    }
}
