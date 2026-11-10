// Vector materialization only. Structural probes are generated independently
// from raw ink by hah_structure.py. No contour-walk matcher is called here.
#include "../cutter/detail.h"
#include "../lettersegmentationcache.h"
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QPainter>

using namespace MedinaCutter;

static QPointF point(const QJsonValue& v){const auto a=v.toArray();return {a[0].toDouble(),a[1].toDouble()};}
static QJsonArray xy(const QPointF& p){return {p.x(),p.y()};}
static QJsonObject pathJson(const QPainterPath& p){
    QJsonArray commands;
    for(int i=0;i<p.elementCount();++i){auto e=p.elementAt(i);
        if(e.isMoveTo())commands.append(QJsonArray{"M",e.x,e.y});
        else if(e.isLineTo())commands.append(QJsonArray{"L",e.x,e.y});
        else if(e.isCurveTo()){auto b=p.elementAt(++i),c=p.elementAt(++i);commands.append(QJsonArray{"C",e.x,e.y,b.x,b.y,c.x,c.y});}}
    return {{"commands",commands},{"fillRule",p.fillRule()==Qt::WindingFill?"nonzero":"evenodd"}};
}
static QJsonObject pointJson(const PathExtrema& p){return {{"xy",xy(p.point)},{"time",p.time},{"path",p.pathIndex}};}
static QJsonObject cutJson(const Stretching& c){return {{"top",pointJson(c.topBaseLine)},{"bottom",pointJson(c.bottomBaseLine)},{"referenceWidth",c.referenceWidth}};}
static QString mask(const QPainterPath& path,const QTransform& transform,const QRect& rect){
    QImage image(rect.size(),QImage::Format_Grayscale8);image.fill(0);QPainter p(&image);p.setPen(Qt::NoPen);p.setBrush(Qt::white);p.translate(-rect.topLeft());p.setTransform(transform,true);p.drawPath(path);p.end();
    QByteArray bytes;QBuffer b(&bytes);b.open(QIODevice::WriteOnly);image.save(&b,"PNG");return QString::fromLatin1(bytes.toBase64());
}
static QJsonObject bodyJson(const QPainterPath& path,const QTransform& transform,const QRect& rect){
    auto json=pathJson(path);auto box=path.boundingRect();json.insert("bounds",QJsonArray{box.x(),box.y(),box.width(),box.height()});json.insert("mask",mask(path,transform,rect));
    const double s=112/std::max(box.width(),box.height());QTransform norm;norm.translate(64-s*box.center().x(),64-s*box.center().y());norm.scale(s,s);json.insert("normalizedMask",mask(path,norm,{0,0,128,128}));return json;
}
static bool snap(const QPainterPath& ink,const QJsonObject& probe,double maxDistance,Stretching& cut){
    auto line=probe.value("line").toArray();auto center=point(probe.value("center"));const double stroke=probe.value("stroke").toDouble();
    auto hits=FindInterections(ink,QLineF(point(line[0]),point(line[1])),true);const auto direction=point(line[1])-point(line[0]);
    std::sort(hits.begin(),hits.end(),[&](const auto&a,const auto&b){return QPointF::dotProduct(a.point,direction)<QPointF::dotProduct(b.point,direction);});
    hits.erase(std::unique(hits.begin(),hits.end(),[](const auto&a,const auto&b){return QLineF(a.point,b.point).length()<1e-6;}),hits.end());
    double best=std::numeric_limits<double>::infinity();bool found=false;
    for(size_t i=1;i<hits.size();++i){const auto a=hits[i-1],b=hits[i];if(!interiorChord(ink,a,b))continue;
        const auto v=b.point-a.point;const double t=std::clamp(QPointF::dotProduct(center-a.point,v)/QPointF::dotProduct(v,v),0.0,1.0);
        const double dist=QLineF(center,a.point+t*v).length();if(dist>maxDistance*stroke||dist>=best)continue;
        cut.topBaseLine=a;cut.bottomBaseLine=b;cut.referenceWidth=stroke;best=dist;found=true;
    }
    return found;
}
struct Candidate{Stretching right,left;QJsonObject json;};

int main(int argc,char**argv){
    QApplication app(argc,argv);Q_INIT_RESOURCE(application);
    if(argc!=5)qFatal("usage: hah_structure_candidates SOURCE_JSON PROBES_JSON CONFIG_JSON OUTPUT_JSON");
    const auto read=[](const char* name){QFile f(name);if(!f.open(QIODevice::ReadOnly))qFatal("Cannot read input");return QJsonDocument::fromJson(f.readAll()).object();};
    auto source=read(argv[1]);const auto probes=read(argv[2]),config=read(argv[3]);const auto matrix=source.value("pdfToRaster").toArray();
    QTransform transform(matrix[0].toDouble(),matrix[1].toDouble(),matrix[2].toDouble(),matrix[3].toDouble(),matrix[4].toDouble(),matrix[5].toDouble());
    QMap<QString,QJsonObject> requests;for(const auto& v:probes.value("words").toArray())requests.insert(v.toObject().value("id").toString(),v.toObject());QJsonArray words;
    for(const auto& value:source.value("words").toArray()){
        auto row=value.toObject();auto ink=letterSegmentationPath(row.value("ink").toObject());auto box=row.value("rasterBounds").toArray();QRect rect(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());
        WordResultInfo::SubWordInfo dummy;SearchContext context{.transformedPath=ink,.subword=dummy};context.bbox=ink.boundingRect();QVector<PathExtrema> extrema;ShapeItem::getExtrema(ink,context.subPaths,extrema);
        for(const auto& p:extrema)if(p.pathIndex==0){context.timeSorted.insert(p);context.xSorted.insert(p);context.ySorted.insert(p);}context.setOrientation();
        auto request=requests.value(row.value("id").toString());std::vector<Candidate> candidates;QJsonObject rejected;
        const auto reject=[&](const QString& reason){rejected.insert(reason,rejected.value(reason).toInt()+1);};
        for(const auto& proposed:request.value("probes").toArray()){
            const auto proposal=proposed.toObject();Stretching right{},left{};
            const double limit=config.value("cut").toObject().value("maxCenterDistance").toDouble(.5);
            if(!snap(ink,proposal.value("rightProbe").toObject(),limit,right)||!snap(ink,proposal.value("leftProbe").toObject(),limit,left)){reject("no-interior-normal-chord");continue;}
            bool accepted=false;QPainterPath body;std::vector<GlyphInfo> partition;
            // Orient endpoints by contour ownership, including horizontal ligature cuts.
            for(int flip=0;flip<4 && !accepted;++flip){auto r=right,l=left;if(flip&1)std::swap(r.topBaseLine,r.bottomBaseLine);if(flip&2)std::swap(l.topBaseLine,l.bottomBaseLine);
                auto pieces=cutRegions(ink,{}, {r,l},context.orientation,true);if(pieces.size()!=3)continue;
                bool covers=true;for(const auto& p:proposal.value("requiredInk").toArray())if(!pieces[1].path.contains(point(p))){covers=false;break;}
                if(!covers)continue;accepted=true;right=r;left=l;body=pieces[1].path;partition=pieces;
            }
            if(!accepted){reject("order-or-structural-ink-coverage");continue;}
            const auto same=[](const Stretching&a,const Stretching&b){return QLineF(a.topBaseLine.point,b.topBaseLine.point).length()<1e-5&&QLineF(a.bottomBaseLine.point,b.bottomBaseLine.point).length()<1e-5;};
            if(std::any_of(candidates.begin(),candidates.end(),[&](const auto& c){return same(c.right,right)&&same(c.left,left);})){reject("duplicate");continue;}
            auto json=bodyJson(body,transform,rect);json.insert("right",cutJson(right));json.insert("left",cutJson(left));json.insert("id",static_cast<int>(candidates.size()));json.insert("structuralFeatures",proposal.value("features"));json.insert("structuralParameters",proposal.value("parameters"));
            json.insert("routes",QJsonArray{QJsonObject{{"template","hah-open-structure-medi"},{"generator","intrinsic-structure"},{"anchor",QJsonObject{{"xy",proposal.value("features").toObject().value("lipTip")}}}}});
            QJsonArray masks;for(const auto& piece:partition)masks.append(mask(piece.path,transform,rect));json.insert("partitionMasks",masks);candidates.push_back({right,left,json});
        }
        QJsonArray output;for(const auto& c:candidates){auto json=c.json;QJsonArray following;const auto order=getJoinTime(c.left,context.orientation);for(size_t i=0;i<candidates.size();++i)if(order.isStrictLeft(candidates[i].right.topBaseLine.time)&&order.isStrictLeft(candidates[i].right.bottomBaseLine.time))following.append(static_cast<int>(i));json.insert("following",following);output.append(json);}
        row.insert("candidates",output);row.insert("structuralDetections",request.value("structures"));row.insert("structuralGraph",request.value("graph"));row.insert("structuralRejections",request.value("rejections"));row.insert("vectorRejections",rejected);words.append(row);
    }
    source.insert("words",words);source.insert("generator","intrinsic-structure");source.insert("humanReviewed",false);source.insert("provenance","machine-experimental");
    QFile out(argv[4]);if(!out.open(QIODevice::WriteOnly))qFatal("Cannot write output");out.write(QJsonDocument(source).toJson(QJsonDocument::Compact));
}
