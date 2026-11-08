// Read-only native microbenchmark: vector-area matching versus cached signatures.
#include "pageanalysisresult.h"
#include "afont.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <array>
#include <cmath>

struct Signature {
    std::array<quint16,16> rows{}, expanded{};
    double aspect=0, height=0;
    int area=0;
    void finish() {
        for (int y=0;y<16;++y) {
            quint16 row=rows[y] | (rows[y]<<1) | (rows[y]>>1);
            expanded[y] |= row;
            if (y) expanded[y-1] |= row;
            if (y<15) expanded[y+1] |= row;
            area += __builtin_popcount(rows[y]);
        }
    }
};
static Signature signature(const QPainterPath& path) {
    Signature s;auto b=path.boundingRect();if(b.isEmpty())return s;
    s.aspect=b.width()/b.height();s.height=b.height();
    QImage image(16,16,QImage::Format_RGB32);image.fill(Qt::white);
    QPainter painter(&image);painter.scale(16/b.width(),16/b.height());
    painter.translate(-b.left(),-b.top());painter.fillPath(path,Qt::black);painter.end();
    for(int y=0;y<16;++y)for(int x=0;x<16;++x)
        s.rows[y]=(s.rows[y]<<1)|(qRed(image.pixel(x,y))<128);
    s.finish();return s;
}
static bool match(const Signature& s,const Signature& t) {
    if(!s.area || !t.area || std::abs(std::log(s.aspect/t.aspect))>.12 ||
       s.height/t.height<.65 || s.height/t.height>1.5)return false;
    int intersection=0,unionArea=0,outside=0;
    for(int y=0;y<16;++y){
        intersection+=__builtin_popcount(s.rows[y]&t.rows[y]);
        unionArea+=__builtin_popcount(s.rows[y]|t.rows[y]);
        outside+=__builtin_popcount(s.rows[y]&quint16(~t.expanded[y]));
        outside+=__builtin_popcount(t.rows[y]&quint16(~s.expanded[y]));
    }
    return 1.-double(intersection)/unionArea<=.45 && double(outside)/(s.area+t.area)<=.025;
}
static APage readPage(int page) {
    QFile file(QString("pages/page%1.dat").arg(page));if(!file.open(QIODevice::ReadOnly))qFatal("Missing page");
    APage result;QDataStream stream(&file);stream>>result;
    if(stream.status()!=QDataStream::Ok)qFatal("Invalid page");return result;
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);Q_INIT_RESOURCE(application);
    AFont font;QFile file("input/font.dat");if(!file.open(QIODevice::ReadOnly))qFatal("Missing font");
    QDataStream stream(&file);stream>>font;
    if(stream.status()!=QDataStream::Ok)qFatal("Invalid font");
    for(auto entry:QList<QPair<QString,Apath>>{{"meem.waqf",readPage(130).lines[3].words[8].paths[13]},
                                            {"qaf.waqf.body",readPage(100).lines[13].words[9].paths[8]}}){
        AGlyph glyph;glyph.name=entry.first;
        glyph.path=QTransform::fromScale(66.1,-66.1).map(entry.second.path);
        auto box=glyph.path.boundingRect();glyph.path.translate(-box.left(),-box.top());
        font.glyphs.insert(entry.first,glyph);
    }
    QFile bankFile("tools/waqf_templates.json");if(!bankFile.open(QIODevice::ReadOnly))qFatal("Missing signature bank");
    auto bank=QJsonDocument::fromJson(bankFile.readAll()).object()["templates"].toArray();
    QStringList names;QVector<Signature> targets;
    for(auto value:bank){auto t=value.toObject();Signature s;s.aspect=t["aspect"].toDouble();s.height=t["heightPdf"].toDouble();
        auto hex=t["signature"].toString();for(int y=0;y<16;++y)s.rows[y]=hex.mid(y*4,4).toUShort(nullptr,16);
        s.finish();names.append(t["name"].toString());targets.append(s);}
    QVector<QPainterPath> local,world;
    for(int page:{18,100,130,238,445})for(auto line:readPage(page).lines)for(auto word:line.words)for(auto shape:word.paths){
        local.append(shape.path);auto path=shape.transform.map(shape.path);path.translate(shape.pos);world.append(path);}
    QVector<Signature> cached;for(const auto& p:world)cached.append(signature(p));
    constexpr int repeats=5;QElapsedTimer timer;int oldHits=0,newHits=0,cachedHits=0;
    timer.start();for(int r=0;r<repeats;++r)for(const auto& p:local)for(const auto& name:names)
        oldHits+=font.checkGlyphScaled(p,name)!=-1;
    auto oldNs=timer.nsecsElapsed();timer.restart();
    for(int r=0;r<repeats;++r)for(const auto& p:world){auto s=signature(p);for(const auto& t:targets)newHits+=match(s,t);}
    auto prepareNs=timer.nsecsElapsed();timer.restart();
    for(int r=0;r<repeats;++r)for(const auto& s:cached)for(const auto& t:targets)cachedHits+=match(s,t);
    auto cachedNs=timer.nsecsElapsed();
    QJsonObject result{{"sourcePages",QJsonArray{18,100,130,238,445}},{"paths",local.size()},{"templates",names.size()},
        {"repeats",repeats},{"comparisons",local.size()*names.size()*repeats},
        {"vectorAreaMs",oldNs/1e6},{"signatureIncludingRenderMs",prepareNs/1e6},{"cachedSignatureMs",cachedNs/1e6},
        {"speedupIncludingRender",double(oldNs)/prepareNs},{"speedupCached",double(oldNs)/cachedNs},
        {"vectorHits",oldHits},{"signatureHits",newHits},{"cachedHits",cachedHits},
        {"scope","Native C++ microbenchmark, same path/template workload. Includes bodies and negatives. Algorithms have different acceptance tests; hit counts are not accuracy measurements. No page parsing, I/O or model inference is timed."}};
    QTextStream(stdout)<<QJsonDocument(result).toJson();
    return newHits==cachedHits ? 0 : 1;
}
