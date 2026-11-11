// Native corpus regressions for eight previously unresolved shape families.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

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
    struct Case {
        int p, l, w, s;
        const char* form;
        int upperCorner = -1, lowerCorner = -1;
        bool finalKashida = false;
    };
    const Case cases[]={{1,2,1,1,"seen-compact-medi"},{1,8,2,3,"beh-tooth-medi"},
        {4,12,4,1,"beh-tooth-medi"},{3,6,2,1,"seen-medi"},
        {5,14,3,1,"feh-descending-init"},{4,11,6,1,"alef-return-fina"},
        {62,1,11,1,"yeh-return-fina"},{19,4,2,2,"kaf-bent-neck-medi"},
        {11,6,2,2,"reh-hook-fina"},{5,15,2,1,"ain-fina"},
        {251,11,4,2,"qaf-fina",9,21},{4,5,1,3,"qaf-fina",2,14},
        {19,4,4,1,"qaf-fina",4,15},{70,3,1,2,"qaf-fina",13,24},
        {3,12,2,3,"waw-fina",-1,-1,true},{10,11,7,2,"waw-fina",6,13},{13,3,3,1,"waw-fina",6,12},
        {71,2,10,1,"waw-fina",4,11},{103,14,8,3,"waw-fina",6,13,true},
        {162,2,2,3,"waw-fina",4,10,true},{3,1,2,3,"noon-fina",2,12,true},
        {22,6,1,1,"noon-fina",-1,17,true},{272,11,9,2,"noon-fina",-1,26,true},
        {70,9,6,2,"noon-fina",-1,18,true},{559,1,1,3,"noon-fina",-1,13,true},
        {559,3,1,3,"noon-fina",-1,21,true}};
    for (const auto at:cases) {
        PageAnalysisResult a; QDataStream stream(read(QString("pages/page%1.dat").arg(at.p))); stream>>a.page;
        a.resolveWordBoundaries(at.p,&font);
        auto& word=a.page.lines[at.l-1].words[at.w-1];
        if (word.wordResultInfo.subWords.empty()) a.detectSubWords(at.p-1,at.l-1,at.w-1,&font);
        auto& sub=word.wordResultInfo.subWords[at.s-1];
        qInfo("Checking page%d:l%d-w%d-s%d · %s",at.p,at.l,at.w,at.s,at.form);
        check(a.segmentSubword(at.p-1,at.l-1,at.w-1,at.s-1,&font,true),"Shape remains partial");
        check(sub.matchedTemplates.contains(at.form),"Required intrinsic form did not match");
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
        if (at.p==3 && at.l==6) check(!sub.kashidaPaths.front().isEmpty(),"Seen absorbed the incoming connector");
        if (at.finalKashida) check(!sub.kashidaPaths.back().isEmpty(),"Final join lost its connector");
        if (at.p==3 && at.l==1)
            check(sub.letterDiagnostics.last().toObject().value("junctionPolicy")=="inward-corners",
                  "The open bowl cut did not use its contour junction");
        if (at.p==71) {
            const auto final = sub.letterDiagnostics.last().toObject();
            check(final.value("template")=="waw-fina" && final.value("cutPolicy")=="final-retry" &&
                  sub.kashidaPaths.back().isEmpty(),"Waw recovery changed the shared cut or exposed a second template");
        }
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
        if (at.upperCorner >= 0 || at.lowerCorner >= 0) {
            WordResultInfo::SubWordInfo dummy;
            const auto c = geometry(ink, dummy);
            const auto findCorner = [&](int time) {
                return std::find_if(c.timeSorted.begin(), c.timeSorted.end(),
                    [&](const auto& point) { return point.time == time; });
            };
            const auto top = findCorner(at.upperCorner), bottom = findCorner(at.lowerCorner);
            const auto& join = *sub.joins.rbegin();
            const QLineF seam = sub.kashidaPaths.back().isEmpty() ?
                QLineF(join.topBaseLine.point, join.bottomBaseLine.point) : sub.kashidaCuts.back().second;
            check((at.upperCorner < 0 || (top != c.timeSorted.end() && QLineF(seam.p1(), top->point).length() < 1e-6)) &&
                  (at.lowerCorner < 0 || (bottom != c.timeSorted.end() && QLineF(seam.p2(), bottom->point).length() < 1e-6)),
                  "Final bowl cut missed a source corner");
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
            const auto locate=[&] {return sharedShapeDescription(form.value("body").toObject()) ? declaredSharedBoundary(c,form) : searchContourWalk(c);};
            check(locate(),"Intrinsic route did not generate a cut"); const auto expected=c.res;
            c.baseText=QStringLiteral("اختبار");c.pairedStrokeWidth=1000;
            Stretching unrelated{};unrelated.topBaseLine.point={-100,300};unrelated.bottomBaseLine.point={-80,400};
            dummy.joins.insert(unrelated);c.res={};
            check(locate() && c.res.topBaseLine.point==expected.topBaseLine.point &&
                  c.res.bottomBaseLine.point==expected.bottomBaseLine.point,"Route depends on neighboring geometry");
        }
    }
    struct MeemJoin {
        int page, line, word, corner;
        bool shared;
        int subword = 1;
        const char* form = "meem-lower-flank-init";
        bool strokeNormal = false;
    };
    for (const auto& at : {MeemJoin{285,11,1,30,false,1,"meem-init"}, MeemJoin{377,8,7,15,false,1,"meem-init"},
                          MeemJoin{415,8,3,16,false},
                          MeemJoin{82,3,2,48,true}, MeemJoin{406,2,4,28,true}, MeemJoin{442,5,11,28,true},
                          MeemJoin{350,4,7,8,false,1,"meem-init",true},
                          MeemJoin{402,2,5,10,false,3,"meem-init",true}}) {
        PageAnalysisResult sample;
        QDataStream stream(read(QString("pages/page%1.dat").arg(at.page)));
        stream >> sample.page;
        sample.resolveWordBoundaries(at.page, &font);
        check(sample.segmentSubword(at.page-1, at.line-1, at.word-1, at.subword-1, &font, true),
              "Initial meem fixture remains partial");
        const auto& word = sample.page.lines[at.line-1].words[at.word-1];
        const auto& sub = word.wordResultInfo.subWords.at(at.subword-1);
        check(sub.letterDiagnostics.first().toObject().value("template") == at.form &&
              toSubpaths(sub.baseGlyphs.front().path).size() == 2 && coversOriginalInk(word, sub),
              "Initial meem lost its counter or original ink");
        check(sub.kashidaPaths.front().isEmpty() == at.shared, "Wrong meem attachment topology");
        const auto shape = word.bodyShape(sub);
        const auto curves = baseContourCurves(shape.transform.map(shape.path).translated(shape.pos));
        const auto& join = *sub.joins.begin();
        const auto cut = at.shared ? QLineF(join.topBaseLine.point, join.bottomBaseLine.point) :
            sub.kashidaCuts.front().first;
        check(QLineF(cut.p2(), curves.at(at.corner).pt1()).length() < 1e-5,
              "Initial meem missed the rim or the stacked arm junction");
        if (at.shared)
            check(std::abs(cut.dy()) < 1e-5, "Stacked meem arm did not share a horizontal cut with hah");
        if (at.strokeNormal) {
            constexpr double step = 1e-4;
            const int upperIndex = static_cast<int>(join.topRight.time);
            const double t = join.topRight.time - upperIndex;
            const auto& upper = curves.at(upperIndex);
            const auto& lower = curves.at(at.corner - 1);
            const auto a = QLineF(upper.pointAt(t - step), upper.pointAt(t + step)).unitVector();
            const auto b = QLineF(lower.pt4(), lower.pointAt(1 - step)).unitVector();
            const auto axis = (a.p2() - a.p1()) + (b.p2() - b.p1());
            check(std::abs(QPointF::dotProduct(axis, cut.p2() - cut.p1())) < 1e-3 * cut.length(),
                  "Initial meem cut is not normal to the two stroke flanks");
        }
    }
    struct AinJoin {
        int page, line, word, subword;
        int stackedCorner = -1;
    };
    for (const auto& at : {AinJoin{16,7,7,1}, AinJoin{202,10,7,1}, AinJoin{359,10,5,1},
                           AinJoin{544,15,11,1}, AinJoin{208,3,9,1,13}, AinJoin{453,15,8,1,31}}) {
        PageAnalysisResult sample;
        QDataStream stream(read(QString("pages/page%1.dat").arg(at.page)));
        stream >> sample.page;
        sample.resolveWordBoundaries(at.page, &font);
        check(sample.segmentSubword(at.page-1, at.line-1, at.word-1, at.subword-1, &font, true),
              "Initial ain fixture remains partial");
        const auto& word = sample.page.lines[at.line-1].words[at.word-1];
        const auto& sub = word.wordResultInfo.subWords.at(at.subword-1);
        check(sub.letterDiagnostics.first().toObject().value("template") == "ain-init" &&
              coversOriginalInk(word, sub), "Initial ain lost its body or source ink");
        const auto shape = word.bodyShape(sub);
        if (at.stackedCorner >= 0) {
            const auto curves = baseContourCurves(shape.transform.map(shape.path).translated(shape.pos));
            const auto& join = *sub.joins.begin();
            check(sub.kashidaPaths.front().isEmpty() &&
                  sub.letterDiagnostics.at(1).toObject().value("entryTemplate") == "hah-stacked-medi" &&
                  std::abs(join.topBaseLine.point.y() - join.bottomBaseLine.point.y()) < 1e-5 &&
                  QLineF(join.bottomBaseLine.point, curves.at(at.stackedCorner).pt1()).length() < 1e-5,
                  "Stacked ain must share the horizontal cut at the hah attachment");
        } else {
            const auto box = shape.transform.map(sub.baseGlyphs.front().path).boundingRect();
            check(!sub.kashidaPaths.front().isEmpty() && box.width() < 1.2 * box.height(),
                  "Initial ain absorbed the stroke after its rim");
        }
    }
    auto invalid=config;auto finals=invalid["final"].toArray();auto f=finals.last().toObject();
    f.insert("sharedBoundary","yes");finals[finals.size()-1]=f;invalid.insert("final",finals);
    check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(),"Malformed shared boundary accepted");
    qInfo("Native shape forms passed: intrinsic cuts, complete bodies, deterministic masks, conserved ink and no false ligature connectors.");
}
