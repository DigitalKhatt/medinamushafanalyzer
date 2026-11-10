// Open medial hah forms must not consume the closed heads that follow them.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

int main(int argc, char** argv) {
    QApplication app(argc,argv); Q_INIT_RESOURCE(application);
    const auto config=PageAnalysisResult::letterForms();
    QJsonObject stacked;
    for (const auto& value:config.value("medial").toArray()) {
        const auto form=value.toObject();
        if (form.value("current")==QStringLiteral("جحخ") && !pairedRouteSettings(form).isEmpty())
            check(form.value("body").toObject().value("counters").toInt(-1)==0,"Open hah form lacks a zero-counter constraint");
        if (form.value("id")=="hah-stacked-medi") stacked=form;
    }
    for (const auto bad:QJsonArray{-1,.5,"0",true}) {
        auto invalid=config, rule=stacked;
        auto body=rule.value("body").toObject();body.insert("counters",bad);rule.insert("body",body);invalid.insert("medial",QJsonArray{rule});
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(),"Invalid medial counter count accepted");
    }
    for (int page:{13,24}) {
        QFile file(QString("pages/page%1.dat").arg(page));check(file.open(QIODevice::ReadOnly),"Missing fixture page");
        PageAnalysisResult analysis;QDataStream stream(&file);stream>>analysis.page;
        const int line=page==13?9:13;
        auto& word=analysis.page.lines[line].words[3];auto& sub=word.wordResultInfo.subWords[0];
        const auto& shape=word.paths[sub.paths.front()];
        const auto inPage=[&](const QPainterPath& p){auto q=shape.transform.map(p);q.translate(shape.pos);return q;};
        auto ink=inPage(shape.path);ink.setFillRule(Qt::WindingFill);
        WordResultInfo::SubWordInfo dummy;
        SearchContext context{.transformedPath=ink,.subword=dummy};context.bbox=ink.boundingRect();
        QVector<PathExtrema> extrema;ShapeItem::getExtrema(ink,context.subPaths,extrema);
        for(const auto& p:extrema)if(p.pathIndex==0){context.timeSorted.insert(p);context.xSorted.insert(p);context.ySorted.insert(p);}
        context.setOrientation();context.nextIter=std::next(context.xSorted.begin());
        check(contourBodyBoundaries(context,pairedRouteSettings(stacked)).empty(),
              "Wrapped stacked-hah cut excludes its own defining anchor");
        auto unguarded=config;
        unguarded.insert("retryUnresolvedForms", false);
        auto forms=unguarded.value("medial").toArray();
        for(int i=0;i<forms.size();++i){auto f=forms[i].toObject();f.remove("bodyCounters");if(f.contains("body")){auto b=f.value("body").toObject();b.remove("counters");f.insert("body",b);}forms[i]=f;}
        unguarded.insert("medial",forms);
        check(!analysis.segmentSubword(page-1,line,3,0,nullptr,true,&unguarded) &&
              sub.matchedTemplates.contains("hah-wedge-medi"),"Fixture no longer exposes a closed head claiming hah");
        check(analysis.segmentSubword(page-1,line,3,0,nullptr,true),"Yukhafafu remains partial");
        check(sub.baseGlyphs.size()==4 && sub.kashidaPaths.size()==3 &&
              sub.matchedTemplates.contains("hah-flat-medi") && sub.matchedTemplates.contains("feh-head-medi") &&
              sub.matchedTemplates.contains("feh-fina") && !sub.matchedTemplates.contains("hah-wedge-medi"),
              "Wrong templates own the four bodies");
        for(const auto& diagnostic:sub.letterDiagnostics)
            check(diagnostic.toObject().value("status")=="matched","Complete hides an unmatched body");
        std::vector<QPainterPath> bodies,masks;
        for(const auto& body:sub.baseGlyphs)bodies.push_back(inPage(body.path));
        check(toSubpaths(bodies[1]).size()==1,"Open kha owns a feh counter");
        std::vector<QRectF> originalCounters;
        for(size_t i=1;i<context.subPaths.size();++i)originalCounters.push_back(context.subPaths[i].boundingRect());
        std::sort(originalCounters.begin(),originalCounters.end(),[](const auto& a,const auto& b){return a.center().x()>b.center().x();});
        check(originalCounters.size()==2,"Expected two original feh counters");
        for(int i=0;i<2;++i){const auto contours=toSubpaths(bodies[i+2]);
            check(contours.size()==2,"Feh lost its closed head");
            const auto actual=contours[1].boundingRect(),expected=originalCounters[i];
            check(QLineF(actual.topLeft(),expected.topLeft()).length()<1e-5 &&
                  QLineF(actual.bottomRight(),expected.bottomRight()).length()<1e-5,"Feh heads have the wrong owners");
        }
        check(std::abs(bodies.back().boundingRect().left()-ink.boundingRect().left())<1e-5,
              "Final feh lost its bowl");
        masks=bodies;
        for(const auto& connector:sub.kashidaPaths){check(!connector.isEmpty(),"Missing genuine connecting span");masks.push_back(inPage(connector));}
        QPainterPathStroker edge;edge.setWidth(.001);edge.setCurveThreshold(1e-5);const auto band=edge.createStroke(ink);
        const auto bounds=ink.boundingRect();int samples=0;
        for(double y=bounds.top()+.013;y<bounds.bottom();y+=.07)for(double x=bounds.left()+.017;x<bounds.right();x+=.07){
            const QPointF p(x,y);int owners=0;for(const auto& mask:masks)owners+=mask.contains(p);
            check(owners==int(ink.contains(p)) || (owners<=1&&band.contains(p)),"Ink partition lost, duplicated or fabricated ink");
            samples+=ink.contains(p);
        }
        check(samples>1000,"Ink conservation sample is too small");
        const auto previousBodies=sub.baseGlyphs;const auto previousConnectors=sub.kashidaPaths;
        check(analysis.segmentSubword(page-1,line,3,0,nullptr,true)&&previousConnectors==sub.kashidaPaths,"Connectors changed on repeat");
        for(size_t i=0;i<previousBodies.size();++i)check(previousBodies[i].path==sub.baseGlyphs[i].path,"Body changed on repeat");
    }
    qInfo("Medial hah ownership passed on pages 13 and 24: open kha retained, counter heads rejected, wrapped anchor cuts rejected, both feh heads and final bowl owned correctly, conserved ink and deterministic masks.");
}
