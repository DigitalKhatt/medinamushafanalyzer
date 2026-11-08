// Final feh owns its head and bowl; its incoming cut uses its own corner pair.
#include "../../segmentsubword.cpp"
#include <QApplication>
#include <array>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static SearchContext geometry(QPainterPath ink, WordResultInfo::SubWordInfo& dummy,
                              const QJsonObject& form) {
    ink.setFillRule(Qt::WindingFill);
    SearchContext context{.form=form, .transformedPath=ink, .subword=dummy};
    context.bbox=ink.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(ink,context.subPaths,extrema);
    for (const auto& p:extrema) if (p.pathIndex==0) {
        context.timeSorted.insert(p); context.xSorted.insert(p); context.ySorted.insert(p);
    }
    context.setOrientation(); context.nextIter=std::next(context.xSorted.begin());
    return context;
}

int main(int argc, char** argv) {
    QApplication app(argc,argv); Q_INIT_RESOURCE(application);
    auto config=PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Bundled forms failed validation");
    QJsonObject form;
    for (const auto& value:config.value("final").toArray())
        if (value.toObject().value("id")=="feh-fina") form=value.toObject();
    check(form.value("method")=="final-counter", "Feh must reuse the counter method");
    const auto settings=form.value("counter").toObject();
    const auto corner=settings.value("entryCorner").toObject();
    for (const auto bad:QJsonArray{QJsonObject{{"placement","wrong"}},
                                  QJsonObject{{"strokeOffsetX","half"}},
                                  QJsonObject{{"maxDistance",0}}}) {
        auto invalid=config, rule=form, bounds=settings, entry=corner;
        const auto fields=bad.toObject();
        for (auto it=fields.begin();it!=fields.end();++it) entry.insert(it.key(),it.value());
        bounds.insert("entryCorner",entry); rule.insert("counter",bounds);
        invalid.insert("final",QJsonArray{rule});
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid neck settings were accepted");
    }
    QFile file("pages/page13.dat"); check(file.open(QIODevice::ReadOnly), "Page 13 fixture is required");
    PageAnalysisResult analysis; QDataStream stream(&file); stream>>analysis.page;
    auto& word=analysis.page.lines[14].words[0]; auto& sub=word.wordResultInfo.subWords[0];
    const auto& shape=word.paths[sub.paths.front()];
    const auto inPage=[&](const QPainterPath& p) { auto q=shape.transform.map(p);q.translate(shape.pos);return q; };
    const auto ink=inPage(shape.path);
    WordResultInfo::SubWordInfo dummy;
    auto context=geometry(ink,dummy,form);
    auto unshifted=form, bounds=settings, entry=corner;
    entry.insert("strokeOffsetX",0); entry.insert("normalCut",false); bounds.insert("entryCorner",entry);
    unshifted.insert("counter",bounds); context.form=unshifted;
    check(searchFinalCounter(context), "Own corner pair was not found");
    const auto pair=context.res;
    check(pair.topBaseLine.time==2 && pair.bottomBaseLine.time==11,
          "Feh borrowed a neighbor's landmark instead of its reviewed corners 2 and 11");
    context.form=form;
    check(searchFinalCounter(context) && matchesBoundaryBody(context,form,context.res,false),
          "Offset feh boundary failed to preserve its counter");
    const auto cut=context.res;
    const QLineF seed(pair.topBaseLine.point,pair.bottomBaseLine.point), chord(cut.topBaseLine.point,cut.bottomBaseLine.point);
    // Intersecting the normal with curved flanks can move its midpoint very
    // slightly away from the shifted vertical section's center.
    check(std::abs(chord.center().x()-seed.center().x()-.5*seed.length())<.001*seed.length(),
          "Feh offset is not half the local neck width to the right");
    const auto curves=baseContourCurves(ink);
    const auto tangent=[&](const PathExtrema& p) {
        const int index=static_cast<int>(std::floor(p.time));const double t=p.time-index,u=1-t;
        const auto& c=curves[index];
        auto v=u*u*(c.pt2()-c.pt1())+2*u*t*(c.pt3()-c.pt2())+t*t*(c.pt4()-c.pt3());
        if(v.x()<0)v=-v;return v/std::hypot(v.x(),v.y());
    };
    check(std::abs(QPointF::dotProduct(chord.p2()-chord.p1(),
          tangent(cut.topBaseLine)+tangent(cut.bottomBaseLine)))<1e-5,
          "Offset cut is not normal to the local stroke");
    const auto same=[&](const Stretching& value,const QTransform& map=QTransform{}) {
        return QLineF(map.map(cut.topBaseLine.point),value.topBaseLine.point).length()<1e-5 &&
               QLineF(map.map(cut.bottomBaseLine.point),value.bottomBaseLine.point).length()<1e-5;
    };
    Stretching unrelated{}; unrelated.topBaseLine.point={-200,300};unrelated.bottomBaseLine.point={-100,500};
    dummy.joins.insert(unrelated);context.baseText=QStringLiteral("اختبار");context.pairedStrokeWidth=1000;
    check(searchFinalCounter(context)&&same(context.res), "Feh depends on neighboring text or cuts");
    for(double scale:{.7,1.6}) {
        const QTransform map(scale,0,0,scale,37,-21);auto scaled=geometry(map.map(ink),dummy,form);
        check(searchFinalCounter(scaled)&&same(scaled.res,map), "Feh cut changed under scale or translation");
    }
    auto withoutHole=geometry(context.subPaths.front(),dummy,form);
    check(!searchFinalCounter(withoutHole), "Feh was matched without its counter");
    auto missingCorner=geometry(ink,dummy,form);
    for(auto it=missingCorner.timeSorted.begin();it!=missingCorner.timeSorted.end();++it)
        if(it->time==11) { missingCorner.timeSorted.erase(it);break; }
    check(!searchFinalCounter(missingCorner), "Required corner pair fell back to a neighbor's neck");

    check(analysis.segmentSubword(12,14,0,0,nullptr,true), "Ghain-lam-feh remains partial");
    check(sub.baseGlyphs.size()==3 && sub.kashidaPaths.size()==2 &&
          std::all_of(sub.kashidaPaths.begin(),sub.kashidaPaths.end(),[](const auto& p){return !p.isEmpty();}),
          "Expected three bodies and two genuine connecting spans");
    const auto& finalJoin=*sub.joins.rbegin();auto retained=cut;
    retained.topBaseLine=finalJoin.topLeft;retained.bottomBaseLine=finalJoin.bottomLeft;
    check(finalJoin.fixedLeftBoundary&&same(retained), "Medial lam replaced feh's own boundary");
    const auto body=inPage(sub.baseGlyphs.back().path);
    check(toSubpaths(body).size()==2 && std::abs(body.boundingRect().left()-ink.boundingRect().left())<1e-5 &&
          std::abs(body.boundingRect().bottom()-ink.boundingRect().bottom())<1e-5,
          "Feh lost its closed head or full bowl");
    std::vector<QPainterPath> masks;
    for(const auto& b:sub.baseGlyphs)masks.push_back(inPage(b.path));
    for(const auto& p:sub.kashidaPaths)masks.push_back(inPage(p));
    QPainterPathStroker edge;edge.setWidth(.001);edge.setCurveThreshold(1e-5);
    const auto band=edge.createStroke(ink);
    const auto box=ink.boundingRect();
    for(double y=box.top()+.013;y<box.bottom();y+=.07)for(double x=box.left()+.017;x<box.right();x+=.07) {
        const QPointF p(x,y);int owners=0;for(const auto& mask:masks)owners+=mask.contains(p);
        check(owners==int(ink.contains(p)) || (owners<=1&&band.contains(p)), "Feh partition lost or duplicated ink");
    }
    const auto bodies=sub.baseGlyphs;
    const auto connectors=sub.kashidaPaths;
    check(analysis.segmentSubword(12,14,0,0,nullptr,true)&&connectors==sub.kashidaPaths,"Connectors are not deterministic");
    for(size_t i=0;i<bodies.size();++i)check(bodies[i].path==sub.baseGlyphs[i].path,"Bodies are not deterministic");
    for(const auto loc:std::vector<std::array<int,4>>{{15,5,5,2},{25,4,4,3},{63,1,1,2},{400,6,3,1}}) {
        QFile f(QString("pages/page%1.dat").arg(loc[0]));check(f.open(QIODevice::ReadOnly),"Held-out fixture missing");
        PageAnalysisResult a;QDataStream s(&f);s>>a.page;
        check(a.segmentSubword(loc[0]-1,loc[1]-1,loc[2]-1,loc[3]-1,nullptr,true),"Held-out final feh remains partial");
        const auto& result=a.page.lines[loc[1]-1].words[loc[2]-1].wordResultInfo.subWords[loc[3]-1];
        check(result.matchedTemplates.contains("feh-fina")&&toSubpaths(result.baseGlyphs.back().path).size()==2,
              "Held-out feh lost its head");
    }
    qInfo("Final feh passed: own corners 2/11, half-neck offset, stroke normal, head/bowl ownership, conserved ink, scale/translation invariance, deterministic masks and four held-out subwords.");
}
