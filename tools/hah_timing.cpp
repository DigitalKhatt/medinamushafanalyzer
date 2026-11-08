// Read-only timing harness for the native cutter. Never writes page/cache data.
#include "../segmentsubword.cpp"
#include "../lettersegmentationcache.h"
#include <QApplication>
#include <QElapsedTimer>

int main(int argc,char**argv) {
    QApplication app(argc,argv); Q_INIT_RESOURCE(application);
    if(argc!=5)qFatal("usage: hah_timing GEOMETRY_DIRECTORY INPUTS_JSON OUTPUT_JSON REPEATS");
    const auto read=[](const QString& name){QFile f(name);if(!f.open(QIODevice::ReadOnly))qFatal("Cannot read input");return QJsonDocument::fromJson(f.readAll()).object();};
    const auto inputs=read(argv[2]);const int repeats=QString(argv[4]).toInt();
    const auto config=PageAnalysisResult::letterForms();
    if(config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool())qFatal("Tracker must remain disabled");
    QJsonArray results;
    for(const auto pageValue:inputs.value("pages").toArray()) {
        const int page=pageValue.toInt();
        QFile f(QString("pages/page%1.dat").arg(page));if(!f.open(QIODevice::ReadOnly))qFatal("Missing source page");
        PageAnalysisResult analysis;QDataStream ds(&f);ds>>analysis.page;
        const auto geometry=read(QString("%1/page%2.json").arg(argv[1]).arg(page,3,10,QLatin1Char('0')));
        for(const auto value:geometry.value("words").toArray()) {
            const auto row=value.toObject();const int li=row.value("line").toInt()-1,wi=row.value("word").toInt()-1,si=row.value("subword").toInt()-1;
            auto& word=analysis.page.lines[li].words[wi];auto& sub=word.wordResultInfo.subWords[si];
            const auto original=sub;const auto shape=word.paths[sub.paths.front()];
            auto ink=shape.transform.map(shape.path);ink.translate(shape.pos);ink.setFillRule(Qt::WindingFill);
            if(ink!=letterSegmentationPath(row.value("ink").toObject()))qFatal("Geometry differs from structural input");
            QJsonArray samples;std::vector<GlyphInfo> referenceBodies;std::vector<QPainterPath> referenceConnectors;
            bool referenceComplete=false,stable=true;int count=0;
            // Refresh=true explicitly bypasses the saved segmentation. Resetting
            // the subword and reading page files are outside the timed region.
            for(int repeat=-1;repeat<repeats;++repeat) {
                sub=original;QElapsedTimer timer;timer.start();
                const bool complete=analysis.segmentSubword(page-1,li,wi,si,nullptr,true,&config);
                const auto ns=timer.nsecsElapsed();
                if(repeat<0){referenceBodies=sub.baseGlyphs;referenceConnectors=sub.kashidaPaths;referenceComplete=complete;}
                else {
                    samples.append(static_cast<double>(ns));
                    stable=stable&&referenceComplete==complete&&referenceBodies.size()==sub.baseGlyphs.size()&&referenceConnectors==sub.kashidaPaths;
                    if(referenceBodies.size()==sub.baseGlyphs.size())for(size_t i=0;i<referenceBodies.size();++i)stable=stable&&referenceBodies[i].path==sub.baseGlyphs[i].path;
                }
                count=static_cast<int>(sub.baseGlyphs.size());
            }
            if(!stable)qFatal("Native output changed between repetitions");
            results.append(QJsonObject{{"id",row.value("id")},{"nanoseconds",samples},{"stable",stable},{"complete",referenceComplete},{"bodyCount",count}});
        }
    }
    QJsonObject result{{"words",results},{"repeats",repeats},{"warmupsPerWord",1},{"trackerEnabled",false},{"refresh",true}};
    QFile out(argv[3]);if(!out.open(QIODevice::WriteOnly))qFatal("Cannot write benchmark output");out.write(QJsonDocument(result).toJson());
}
