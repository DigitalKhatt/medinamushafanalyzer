// Native corpus regressions for eight previously unresolved shape families.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }
static QByteArray read(const QString& path) {
    QFile f(path); check(f.open(QIODevice::ReadOnly), "Missing native fixture"); return f.readAll();
}
static SearchContext geometry(QPainterPath ink, WordResultInfo::SubWordInfo& dummy) {
    ink.setFillRule(Qt::WindingFill);
    SearchContext c{.transformedPath=ink, .subword=dummy}; c.bbox=ink.boundingRect();
    QVector<PathExtrema> extrema; ShapeItem::getExtrema(ink,c.subPaths,extrema);
    for (const auto& p:extrema) if (!p.pathIndex) {
        c.timeSorted.insert(p); c.xSorted.insert(p); c.ySorted.insert(p);
    }
    c.setOrientation(); c.nextIter=std::next(c.xSorted.begin()); return c;
}
int main(int argc,char** argv) {
    QApplication app(argc,argv); Q_INIT_RESOURCE(application);
    auto config=PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(),"Invalid shape templates");
    check(!config["kashidaDefaults"].toObject()["trackingEnabled"].toBool(),"Tracker must be disabled");
    AFont font; QDataStream fs(read("input/font.dat")); fs>>font;
    PageAnalysisResult::initQuranText();
    struct Case {int p,l,w,s; const char* form;};
    const Case cases[]={{1,2,1,1,"seen-compact-medi"},{1,8,2,3,"beh-tooth-medi"},
        {4,12,4,1,"beh-tooth-medi"},{3,6,2,1,"seen-medi"},
        {5,14,3,1,"feh-descending-init"},{4,11,6,1,"alef-return-fina"},
        {5,15,6,1,"yeh-return-fina"},{19,4,2,2,"kaf-bent-neck-medi"},
        {11,6,2,2,"reh-hook-fina"},{5,15,2,1,"ain-fina"}};
    for (const auto at:cases) {
        PageAnalysisResult a; QDataStream stream(read(QString("pages/page%1.dat").arg(at.p))); stream>>a.page;
        a.resolveWordBoundaries(at.p,&font);
        auto& word=a.page.lines[at.l-1].words[at.w-1];
        if (word.wordResultInfo.subWords.empty()) a.detectSubWords(at.p-1,at.l-1,at.w-1,&font);
        auto& sub=word.wordResultInfo.subWords[at.s-1];
        qInfo("Checking page%d:l%d-w%d-s%d · %s",at.p,at.l,at.w,at.s,at.form);
        check(a.segmentSubword(at.p-1,at.l-1,at.w-1,at.s-1,&font,true),"Shape remains partial");
        check(!sub.provisional() && sub.matchedTemplates.contains(at.form),"Required intrinsic form did not match");
        check(sub.baseGlyphs.size()==PageAnalysisResult::bodyLetters(sub.text).size(),"Wrong body count");
        const auto& sh=word.paths[sub.paths.front()];
        const auto mapped=[&](const QPainterPath& p) {auto q=sh.transform.map(p);q.translate(sh.pos);return q;};
        auto ink=mapped(sh.path); ink.setFillRule(Qt::WindingFill);
        std::vector<QPainterPath> masks;
        for (const auto& b:sub.baseGlyphs) masks.push_back(mapped(b.path));
        for (const auto& p:sub.kashidaPaths) if (!p.isEmpty()) masks.push_back(mapped(p));
        QPainterPathStroker stroker; stroker.setWidth(.001); stroker.setCurveThreshold(1e-5);
        const auto band=stroker.createStroke(ink); const auto box=ink.boundingRect();
        for (double y=box.top()+.013;y<box.bottom();y+=.07)
            for (double x=box.left()+.017;x<box.right();x+=.07) {
                const QPointF p(x,y); int owners=0;for (const auto& m:masks) owners+=m.contains(p);
                check(owners==int(ink.contains(p)) || (owners<=1 && band.contains(p)),"Lost or duplicated ink");
            }
        if (at.p==3) check(!sub.kashidaPaths.front().isEmpty(),"Seen absorbed the incoming connector");
        if (at.p==4 && at.l==12) {
            WordResultInfo::SubWordInfo dummy;auto c=geometry(ink,dummy);
            const auto tip=std::find_if(c.timeSorted.begin(),c.timeSorted.end(),[](const auto& p){return p.time==17;});
            check(tip!=c.timeSorted.end() && masks[1].contains(tip->point+QPointF(0,.03)) &&
                  !masks[2].contains(tip->point+QPointF(0,.03)),"First tooth was skipped and assigned as a connector");
        }
        if (QString(at.form)=="alef-return-fina")
            check(sub.kashidaPaths.back().isEmpty(),"Folded ligature return was classified as kashida");
        if (QString(at.form)=="feh-descending-init") {
            check(toSubpaths(sub.baseGlyphs.front().path).size()==2,"Feh lost its own counter");
            check(sub.kashidaPaths.front().isEmpty(),"Feh-yeh ligature acquired a false connector");
        }
        if (QString(at.form)=="kaf-bent-neck-medi") {
            check(!sub.kashidaPaths.front().isEmpty(),"Contour-ordered kaf entry was silently dropped");
            check(std::abs(masks[1].boundingRect().top()-box.top())<1e-5,"Kaf lost its rising arm");
        }
        const auto bodies=sub.baseGlyphs; const auto connectors=sub.kashidaPaths;
        check(a.segmentSubword(at.p-1,at.l-1,at.w-1,at.s-1,&font,true) &&
              sub.kashidaPaths==connectors,"Nondeterministic connectors");
        for (size_t i=0;i<bodies.size();++i) check(bodies[i].path==sub.baseGlyphs[i].path,"Nondeterministic bodies");

        // New route-based final shapes use only the outline, even if a previous
        // letter's text, cut and stroke-width hint are replaced.
        if (QString(at.form)=="ain-fina" || QString(at.form)=="alef-return-fina" ||
            QString(at.form)=="reh-hook-fina" || QString(at.form)=="yeh-return-fina") {
            QJsonObject form; for (const auto& v:config["final"].toArray())
                if (v.toObject()["id"]==at.form) form=v.toObject();
            WordResultInfo::SubWordInfo dummy;auto c=geometry(ink,dummy);c.form=form;
            check(searchContourWalk(c),"Intrinsic route did not generate a cut"); const auto expected=c.res;
            c.baseText=QStringLiteral("اختبار");c.pairedStrokeWidth=1000;
            Stretching unrelated{};unrelated.topBaseLine.point={-100,300};unrelated.bottomBaseLine.point={-80,400};
            dummy.joins.insert(unrelated);c.res={};
            check(searchContourWalk(c) && c.res.topBaseLine.point==expected.topBaseLine.point &&
                  c.res.bottomBaseLine.point==expected.bottomBaseLine.point,"Route depends on neighboring geometry");
        }
    }
    auto invalid=config;auto finals=invalid["final"].toArray();auto f=finals.last().toObject();
    f.insert("sharedBoundary","yes");finals[finals.size()-1]=f;invalid.insert("final",finals);
    check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(),"Malformed shared boundary accepted");
    qInfo("Eight shape families passed: intrinsic cuts, complete bodies, deterministic masks, conserved ink and no false ligature connectors.");
}
