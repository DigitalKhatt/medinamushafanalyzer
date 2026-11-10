// Flat kaf boundaries use its rounded fold, projected exit and narrow neck.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok,const char* message){if(!ok)qFatal("%s",message);}
static SearchContext geometry(QPainterPath ink,WordResultInfo::SubWordInfo& dummy){
    ink.setFillRule(Qt::WindingFill);
    SearchContext c{.transformedPath=ink,.subword=dummy};c.bbox=ink.boundingRect();
    QVector<PathExtrema> points;ShapeItem::getExtrema(ink,c.subPaths,points);
    for(const auto& p:points)if(p.pathIndex==0){c.timeSorted.insert(p);c.xSorted.insert(p);c.ySorted.insert(p);}
    c.setOrientation();c.nextIter=std::next(c.xSorted.begin());return c;
}
static bool same(const Stretching& a,const Stretching& b,const QTransform& map={}){
    return QLineF(map.map(a.topBaseLine.point),b.topBaseLine.point).length()<1e-4 &&
        QLineF(map.map(a.bottomBaseLine.point),b.bottomBaseLine.point).length()<1e-4;
}

int main(int argc,char**argv){
    QApplication app(argc,argv);Q_INIT_RESOURCE(application);
    auto config=PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(),"Bundled forms rejected");
    QJsonObject form;
    for(const auto& v:config.value("medial").toArray())if(v.toObject().value("id")=="kaf-flat-medi")form=v.toObject();
    check(form.value("body").toObject().value("counters").toInt(-1)==0,"Open kaf must reject counters");
    const auto settings=pairedRouteSettings(form);
    for(const auto bad:QJsonArray{QJsonObject{{"maxX","zero"}},QJsonObject{{"unknown",0}},
                                  QJsonObject{{"minDistance",-1}},QJsonObject{{"minX",2},{"maxX",1}},
                                  QJsonObject{{"flanks",QJsonObject{{"minY",2},{"maxY",1}}}},
                                  QJsonObject{{"flanks",QJsonObject{{"minY","zero"}}}}}){
        auto invalid=config,rule=form,body=form.value("cuts").toObject().value("routes").toObject(),right=body.value("right").toObject();
        right.insert("oppositeCorner",bad);body.insert("right",right);auto cuts = rule.value("cuts").toObject(); cuts.insert("routes",body); rule.insert("cuts",cuts);
        invalid.insert("medial",QJsonArray{rule});
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(),"Invalid opposing corner bounds accepted");
    }
    QFile file("pages/page13.dat");check(file.open(QIODevice::ReadOnly),"Page 13 fixture missing");
    PageAnalysisResult analysis;QDataStream stream(&file);stream>>analysis.page;
    auto& word=analysis.page.lines[14].words[4];auto& sub=word.wordResultInfo.subWords[0];
    const auto& shape=word.paths[sub.paths.front()];
    const auto inPage=[&](const QPainterPath& p){auto q=shape.transform.map(p);q.translate(shape.pos);return q;};
    auto ink=inPage(shape.path);ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy;auto c=geometry(ink,dummy);
    auto unshifted=settings,right=unshifted.value("right").toObject(),left=unshifted.value("left").toObject();
    right.insert("walkNormalOffset",0);left.insert("normalCut",false);left.insert("strokeOffsetX",0);
    unshifted.insert("right",right);unshifted.insert("left",left);
    const auto seeds=contourBodyBoundaries(c,unshifted);
    check(seeds.size()==1,"Expected one intrinsic flat kaf, not the neighboring feh loop");
    const auto [entry,exit]=seeds.front();
    check(entry.topBaseLine.time==26 && entry.bottomBaseLine.time==18,"Entry missed corners 26 and 18");
    auto edge=c.timeSorted.end();
    for(auto it=c.timeSorted.begin();it!=c.timeSorted.end();++it)
        if(std::abs(it->time-2.0198341493195167)<1e-5)edge=it;
    check(edge!=c.timeSorted.end() && std::abs(exit.topBaseLine.point.x()-edge->point.x())<1e-5 &&
          std::abs(exit.bottomBaseLine.point.x()-edge->point.x())<1e-5 && exit.topBaseLine.point.y()>edge->point.y(),
          "Exit is not projected below the rounded left edge");
    const auto pairs=contourBodyBoundaries(c,settings);check(pairs.size()==1,"Offset kaf boundaries missing or ambiguous");
    const auto [r,l]=pairs.front();const auto curves=baseContourCurves(ink);
    const double neck=QLineF(entry.topBaseLine.point,entry.bottomBaseLine.point).length();
    check(r.topBaseLine.time>25 && r.topBaseLine.time<26 &&
          std::abs(curves[25].getSubRange(r.topBaseLine.time-25,1).length(.00001)-neck*.5)<.001,
          "Right cut did not advance half the neck width toward the incoming connector");
    const QLineF oldExit(exit.topBaseLine.point,exit.bottomBaseLine.point),newExit(l.topBaseLine.point,l.bottomBaseLine.point);
    check(std::abs(newExit.center().x()-oldExit.center().x()+.5*oldExit.length())<.02*oldExit.length(),
          "Left cut did not move half a local stroke outward from the projection");
    const double t=r.topBaseLine.time-25,u=1-t;const auto& curve=curves[25];
    const auto tangent=u*u*(curve.pt2()-curve.pt1())+2*u*t*(curve.pt3()-curve.pt2())+t*t*(curve.pt4()-curve.pt3());
    check(std::abs(QPointF::dotProduct(r.bottomBaseLine.point-r.topBaseLine.point,tangent))<1e-5,
          "Incoming cut is not normal to its stroke");
    Stretching unrelated{};unrelated.topBaseLine.point={-100,300};unrelated.bottomBaseLine.point={-200,500};
    dummy.joins.insert(unrelated);c.baseText=QStringLiteral("اختبار");c.pairedStrokeWidth=1000;
    const auto independent=contourBodyBoundaries(c,settings);
    check(independent.size()==1&&same(r,independent[0].first)&&same(l,independent[0].second),"Kaf depends on neighboring landmarks");
    for(double scale:{.7,1.6}){const QTransform map(scale,0,0,scale,37,-21);auto scaled=geometry(map.map(ink),dummy);
        const auto result=contourBodyBoundaries(scaled,settings);
        check(result.size()==1&&same(r,result[0].first,map)&&same(l,result[0].second,map),"Kaf boundaries changed under scale/translation");}
    // A closed SVG can begin anywhere, including within the half-stroke walk.
    for(int offset:{1,26}) {
        QPainterPath rotated;rotated.setFillRule(Qt::WindingFill);rotated.moveTo(curves[offset].pt1());
        for(size_t i=0;i<curves.size();++i){const auto& b=curves[(i+offset)%curves.size()];rotated.cubicTo(b.pt2(),b.pt3(),b.pt4());}
        rotated.closeSubpath();const auto paths=toSubpaths(ink);
        for(size_t i=1;i<paths.size();++i)rotated.addPath(paths[i]);
        if(offset==26) {
            // Exercise the arclength walk across the seam directly. The
            // legacy extrema extractor omits a corner exactly at MoveTo.
            auto start=entry.topBaseLine,limit=start;start.time=0;limit.time=-double(curves.size());
            PathExtrema advanced{};QPointF direction;
            check(advanceOnContour(rotated,start,limit,false,neck*.5,advanced,direction)&&
                  QLineF(advanced.point,r.topBaseLine.point).length()<1e-5,"Half-stroke walk cannot cross the SVG seam");
        } else {
            auto moved=geometry(rotated,dummy);const auto result=contourBodyBoundaries(moved,settings);
            check(result.size()==1&&same(r,result[0].first)&&same(l,result[0].second),"Kaf changed when the SVG contour start moved");
        }
    }
    check(analysis.segmentSubword(12,14,4,0,nullptr,true),"Beh-kaf-feh-reh remains partial");
    check(sub.baseGlyphs.size()==4&&sub.kashidaPaths.size()==3&&sub.matchedTemplates.contains("kaf-flat-medi")&&
          sub.matchedTemplates.contains("feh-head-medi")&&sub.matchedTemplates.contains("reh-fina"),"Kaf did not unblock feh and reh");
    std::vector<QPainterPath> masks;for(const auto& b:sub.baseGlyphs)masks.push_back(inPage(b.path));
    check(toSubpaths(masks[1]).size()==1&&toSubpaths(masks[2]).size()==2&&
          std::abs(masks[3].boundingRect().left()-ink.boundingRect().left())<1e-5,"Incorrect kaf/feh/tail ownership");
    for(const auto& p:sub.kashidaPaths){check(!p.isEmpty(),"Genuine connector missing");masks.push_back(inPage(p));}
    QPainterPathStroker edgeBand;edgeBand.setWidth(.001);edgeBand.setCurveThreshold(1e-5);const auto band=edgeBand.createStroke(ink);
    for(double y=c.bbox.top()+.013;y<c.bbox.bottom();y+=.07)for(double x=c.bbox.left()+.017;x<c.bbox.right();x+=.07){
        const QPointF p(x,y);int owners=0;for(const auto& mask:masks)owners+=mask.contains(p);
        check(owners==int(ink.contains(p))||(owners<=1&&band.contains(p)),"Kaf partition lost, duplicated or fabricated ink");}
    const auto bodies=sub.baseGlyphs;const auto connectors=sub.kashidaPaths;
    check(analysis.segmentSubword(12,14,4,0,nullptr,true)&&connectors==sub.kashidaPaths,"Nondeterministic connectors");
    for(size_t i=0;i<bodies.size();++i)check(bodies[i].path==sub.baseGlyphs[i].path,"Nondeterministic bodies");
    for(const auto at:std::array<std::array<int,4>,9>{{{3,9,3,2},{3,11,5,2},{13,7,6,1},
                                                    {63,1,8,2},{13,5,7,1},{182,12,5,2},
                                                    {2,3,4,2},{8,3,2,2},{48,7,2,3}}}) {
        QFile f(QString("pages/page%1.dat").arg(at[0]));check(f.open(QIODevice::ReadOnly),"Kaf regression page missing");
        PageAnalysisResult heldout;QDataStream s(&f);s>>heldout.page;
        check(heldout.segmentSubword(at[0]-1,at[1]-1,at[2]-1,at[3]-1,nullptr,true),"Another flat kaf remains partial");
        const auto& result=heldout.page.lines[at[1]-1].words[at[2]-1].wordResultInfo.subWords[at[3]-1];
        check(result.matchedTemplates.contains("kaf-flat-medi"),"Expected intrinsic kaf template missing");
        check(result.baseGlyphs.size()==size_t(PageAnalysisResult::bodyLetters(result.text).size()),"Kaf changed the body count");
    }
    qInfo("Flat medial kaf passed: outer fold projection, 18/26 neck pair, half-stroke offsets, normal cut, neighbor independence, scale/translation, correct four bodies/three connectors and conserved ink.");
}
