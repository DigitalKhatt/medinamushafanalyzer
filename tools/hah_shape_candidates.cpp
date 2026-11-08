// Isolated experiment: export intrinsic medial-hah candidates. Never writes live caches.
// Including the engine keeps the experiment on the exact production geometry code.
#include "../segmentsubword.cpp"
#include "../lettersegmentationcache.h"
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QPainter>

static QJsonArray xy(const QPointF& p) { return {p.x(), p.y()}; }
static QJsonObject pathJson(const QPainterPath& path) {
    QJsonArray commands;
    for (int i=0; i<path.elementCount(); ++i) {
        auto e=path.elementAt(i);
        if(e.isMoveTo()) commands.append(QJsonArray{"M",e.x,e.y});
        else if(e.isLineTo()) commands.append(QJsonArray{"L",e.x,e.y});
        else if(e.isCurveTo()) {
            auto b=path.elementAt(++i), c=path.elementAt(++i);
            commands.append(QJsonArray{"C",e.x,e.y,b.x,b.y,c.x,c.y});
        }
    }
    return {{"commands",commands},{"fillRule",path.fillRule()==Qt::WindingFill?"nonzero":"evenodd"}};
}
static QJsonObject pointJson(const PathExtrema& p) {
    return {{"xy",xy(p.point)},{"time",p.time},{"path",p.pathIndex},
        {"sharp",p.isSharp},{"xExtremum",p.isX},{"yExtremum",p.isY}};
}
static QJsonObject cutJson(const Stretching& cut) {
    return {{"top",pointJson(cut.topBaseLine)},{"bottom",pointJson(cut.bottomBaseLine)},
        {"referenceWidth",cut.referenceWidth}};
}
static QString png(const QImage& image) {
    QByteArray bytes; QBuffer b(&bytes); b.open(QIODevice::WriteOnly); image.save(&b,"PNG");
    return QString::fromLatin1(bytes.toBase64());
}
static QImage draw(const QPainterPath& path,const QTransform& transform,const QRect& rect) {
    QImage image(rect.size(),QImage::Format_Grayscale8); image.fill(0);
    QPainter p(&image);p.setPen(Qt::NoPen);p.setBrush(Qt::white);
    p.translate(-rect.topLeft());p.setTransform(transform,true);p.drawPath(path);p.end();
    return image;
}
static QJsonObject bodyJson(const QPainterPath& path,const QTransform& transform,const QRect& rect) {
    const auto box=path.boundingRect();
    QJsonObject result=pathJson(path);
    result.insert("bounds",QJsonArray{box.x(),box.y(),box.width(),box.height()});
    result.insert("mask",png(draw(path,transform,rect)));
    // Preserve aspect ratio and chirality; only translation and uniform scale are removed.
    const double scale=112/std::max(box.width(),box.height());
    QTransform norm;norm.translate(64-scale*box.center().x(),64-scale*box.center().y());norm.scale(scale,scale);
    result.insert("normalizedMask",png(draw(path,norm,QRect(0,0,128,128))));
    return result;
}
struct Candidate { Stretching right,left; QPainterPath body; QJsonObject json; };

int main(int argc,char**argv) {
    QApplication app(argc,argv);Q_INIT_RESOURCE(application);
    if(argc!=6) qFatal("usage: hah_shape_candidates PAGE BASELINE_JSON VECTOR_JSON OUTPUT_JSON MODE");
    const int page=QString(argv[1]).toInt();
    const auto read=[](const QString& name){QFile f(name);if(!f.open(QIODevice::ReadOnly))qFatal("Cannot read input");return QJsonDocument::fromJson(f.readAll()).object();};
    const auto baseline=read(argv[2]), vector=read(argv[3]);
    const auto matrix=vector.value("pdfToRaster").toArray();
    QTransform transform(matrix[0].toDouble(),matrix[1].toDouble(),matrix[2].toDouble(),matrix[3].toDouble(),matrix[4].toDouble(),matrix[5].toDouble());
    QFile f(QString("pages/page%1.dat").arg(page));if(!f.open(QIODevice::ReadOnly))qFatal("Missing page");
    QDataStream ds(&f);PageAnalysisResult analysis;ds>>analysis.page;
    QJsonArray words;
    const auto config=PageAnalysisResult::letterForms();
    for(const auto& value:baseline.value("subwords").toArray()) {
        const auto row=value.toObject();QJsonArray targets;
        for(const auto& item:row.value("letters").toArray()) {
            auto letter=item.toObject();
            if(letter.value("position")=="medial" && QStringLiteral("جحخ").contains(letter.value("character").toString())) targets.append(letter);
        }
        if(targets.isEmpty())continue;
        const int li=row.value("line").toInt(),wi=row.value("word").toInt(),si=row.value("subword").toInt();
        auto& word=analysis.page.lines[li-1].words[wi-1];auto& sub=word.wordResultInfo.subWords[si-1];
        if(sub.paths.empty())continue;auto shape=word.paths[sub.paths.front()];
        auto ink=shape.transform.map(shape.path);ink.translate(shape.pos);ink.setFillRule(Qt::WindingFill);
        const auto rect=transform.mapRect(ink.boundingRect()).toAlignedRect().adjusted(-2,-2,2,2);
        WordResultInfo::SubWordInfo dummy;SearchContext context{.transformedPath=ink,.subword=dummy};context.bbox=ink.boundingRect();
        QVector<PathExtrema> extrema;ShapeItem::getExtrema(ink,context.subPaths,extrema);
        QJsonArray landmarks;for(const auto& p:extrema){landmarks.append(pointJson(p));if(p.pathIndex==0){context.timeSorted.insert(p);context.xSorted.insert(p);context.ySorted.insert(p);}}
        if(context.xSorted.size()<2)continue;context.setOrientation();context.nextIter=std::next(context.xSorted.begin());
        std::vector<Candidate> candidates;
        for(const auto& formValue:config.value("medial").toArray()) {
            const auto form=formValue.toObject();if(!form.value("current").toString().contains(QStringLiteral("ح")))continue;
            const auto settings=form.value("contourBody").toObject();if(settings.isEmpty())continue;
            const auto strict=contourBodyBoundaries(context,settings);
            // Try every intrinsic x-extremum: the shape scorer, not a flank-distance cutoff,
            // decides whether the resulting whole body resembles a medial hah.
            for(const auto& anchor:context.timeSorted) {
                if(!anchor.isX)continue;
                context.form=settings.value("right").toObject();context.res={};if(!searchContourWalk(context,&anchor))continue;
                auto right=context.res;
                context.form=settings.value("left").toObject();context.res={};if(!searchContourWalk(context,&anchor))continue;
                auto left=context.res;
                auto order=getJoinTime(right,context.orientation);
                if(!order.isStrictLeft(left.topBaseLine.time)||!order.isStrictLeft(left.bottomBaseLine.time))continue;
                auto pieces=cutRegions(ink,{}, {right,left},context.orientation,true);
                if(pieces.size()!=3 || !pieces[1].path.boundingRect().adjusted(-1e-6,-1e-6,1e-6,1e-6).contains(anchor.point))continue;
                const auto same=[&](const Stretching& a,const Stretching& b){return QLineF(a.topBaseLine.point,b.topBaseLine.point).length()<1e-5 && QLineF(a.bottomBaseLine.point,b.bottomBaseLine.point).length()<1e-5;};
                bool isStrict=std::any_of(strict.begin(),strict.end(),[&](const auto& p){return same(p.first,right)&&same(p.second,left);});
                const QJsonObject route{{"template",form.value("id")},{"originalAnchorChecks",isStrict},{"anchor",pointJson(anchor)}};
                auto existing=std::find_if(candidates.begin(),candidates.end(),[&](const auto& p){return same(p.right,right)&&same(p.left,left);});
                if(existing!=candidates.end()){auto routes=existing->json.value("routes").toArray();routes.append(route);existing->json.insert("routes",routes);continue;}
                auto body=bodyJson(pieces[1].path,transform,rect);
                body.insert("right",cutJson(right));body.insert("left",cutJson(left));body.insert("routes",QJsonArray{route});
                // Raw three-way partition is exported for conservation checks. No pixels repaired.
                QJsonArray masks;for(const auto& piece:pieces)masks.append(png(draw(piece.path,transform,rect)));
                body.insert("partitionMasks",masks);
                body.insert("id",static_cast<int>(candidates.size()));
                candidates.push_back({right,left,pieces[1].path,body});
            }
        }
        QJsonArray outputCandidates;
        for(const auto& candidate:candidates) {
            auto json=candidate.json;QJsonArray following;
            auto order=getJoinTime(candidate.left,context.orientation);
            for(size_t i=0;i<candidates.size();++i)if(order.isStrictLeft(candidates[i].right.topBaseLine.time)&&order.isStrictLeft(candidates[i].right.bottomBaseLine.time)) following.append(static_cast<int>(i));
            json.insert("following",following);outputCandidates.append(json);
        }
        QJsonArray baselineBodies;
        for(const auto& item:row.value("bodies").toArray()) baselineBodies.append(bodyJson(letterSegmentationPath(item.toObject()),transform,rect));
        QJsonObject result=row;result.insert("bodies",baselineBodies);result.insert("targets",targets);
        result.insert("id",QString("page%1:l%2-w%3-s%4").arg(page,3,10,QLatin1Char('0')).arg(li).arg(wi).arg(si));
        result.insert("ink",pathJson(ink));result.insert("sourceMask",png(draw(ink,transform,rect)));
        result.insert("rasterBounds",QJsonArray{rect.x(),rect.y(),rect.width(),rect.height()});
        result.insert("landmarks",landmarks);result.insert("candidates",outputCandidates);words.append(result);
    }
    QJsonObject output{{"schemaVersion",1},{"page",page},{"mode",QString(argv[5])},{"provenance","machine-experimental"},
        {"humanReviewed",false},{"pdfToRaster",matrix},{"raster",vector.value("raster")},{"sourceSha256",vector.value("sourceSha256")},
        {"cacheVersion",baseline.value("version")},{"words",words}};
    QFile out(argv[4]);if(!out.open(QIODevice::WriteOnly))qFatal("Cannot write output");out.write(QJsonDocument(output).toJson(QJsonDocument::Compact));
    qInfo()<<"Exported page"<<page<<words.size()<<"subwords";
}
