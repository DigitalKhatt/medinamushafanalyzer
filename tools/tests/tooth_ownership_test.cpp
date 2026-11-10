#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool passed, const char* message) { if (!passed) qFatal("%s", message); }

int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    QFile file("pages/page13.dat");
    check(file.open(QIODevice::ReadOnly), "Page 13 is required");
    PageAnalysisResult analysis; QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[1].words[5];
    auto& sub = word.wordResultInfo.subWords[2];
    const auto& shape = word.paths[sub.paths.front()];
    const auto inPage = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path); p.translate(shape.pos); return p;
    };
    auto ink = inPage(shape.path); ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy;
    SearchContext context{.transformedPath=ink,.subword=dummy}; context.bbox=ink.boundingRect();
    QVector<PathExtrema> extrema; ShapeItem::getExtrema(ink,context.subPaths,extrema);
    for (const auto& p : extrema) if (p.pathIndex == 0) {
        context.timeSorted.insert(p); context.xSorted.insert(p); context.ySorted.insert(p);
    }
    context.setOrientation(); context.nextIter=std::next(context.xSorted.begin());
    const auto config=PageAnalysisResult::letterForms();
    QJsonObject tooth, initial, final;
    for (const auto& value:config.value("initial").toArray())
        if (value.toObject().value("id")=="beh-init") initial=value.toObject();
    for (const auto& value:config.value("medial").toArray())
        if (value.toObject().value("id")=="beh-tooth-medi") tooth=value.toObject();
    for (const auto& value:config.value("final").toArray())
        if (value.toObject().value("id")=="meem-counter-fina") final=value.toObject();
    context.form=initial;
    check(declaredSharedBoundary(context, initial), "Fixture no longer exercises the misleading initial route");
    auto badInitial=context.res;
    const auto proposals=declaredToothBoundaries(context, tooth);
    check(proposals.size()==1 && std::abs(proposals[0].tip.time-3.65792430518215)<1e-6,
          "The meem loop was mistaken for the medial tooth");
    context.form=final;
    check(declaredSharedBoundary(context,final) && matchesBoundaryBody(context,final,context.res,false),
          "Actual final meem boundary lost its counter");
    // The old false tooth exit sliced the head away from the final meem.
    Stretching tailOnly{};
    tailOnly.topBaseLine.time=7.593144951908963; tailOnly.topBaseLine.point={109.05045929034863,116.10449969412296};
    tailOnly.bottomBaseLine.time=12.15542272728504; tailOnly.bottomBaseLine.point={112.25078216411562,117.97091109416246};
    check(!matchesBoundaryBody(context,final,tailOnly,false), "A replacement cut discarded the final body's counter");

    // Reconstruct the old accepted span independently of the strengthened
    // tooth detector, so the connector-order guard has its own regression.
    badInitial.fixedLeftBoundary=true;
    badInitial.topLeft.time=5.808166088164862; badInitial.topLeft.point={113.60794232279376,116.41362498916266};
    badInitial.bottomLeft.time=13.091164216903422; badInitial.bottomLeft.point={113.18645420248116,117.69770089112306};
    auto badLeft=badInitial;
    badLeft.topBaseLine=badInitial.topLeft; badLeft.bottomBaseLine=badInitial.bottomLeft;
    check(connectorContainsLeadingStroke(extrema,badInitial,badLeft,context.orientation),
          "A connector before the initial body was accepted");

    check(analysis.segmentSubword(12,1,5,2,nullptr,true), "Existing alternative failed to recover noon-teh-meem");
    check(sub.matchedTemplates.contains("beh-shallow-step-init") && sub.matchedTemplates.contains("beh-tooth-medi") &&
          sub.baseGlyphs.size()==3, "Expected three independently matched bodies");
    const auto initialBody=inPage(sub.baseGlyphs[0].path), toothBody=inPage(sub.baseGlyphs[1].path), finalBody=inPage(sub.baseGlyphs[2].path);
    const auto firstStroke=context.xSorted.begin()->point+QPointF(-.01,0);
    const auto toothInk=proposals[0].tip.point+.1*(proposals[0].bend.point-proposals[0].tip.point);
    check(ink.contains(firstStroke) && initialBody.contains(firstStroke), "Initial stroke is outside the initial body");
    check(toothBody.contains(toothInk), "Teh lost its own tooth");
    check(toSubpaths(toothBody).size()==1 && toSubpaths(finalBody).size()==2, "Meem's loop has the wrong letter owner");
    std::vector<QPainterPath> masks{initialBody,toothBody,finalBody};
    for (const auto& connector:sub.kashidaPaths) if (!connector.isEmpty()) {
        const auto p=inPage(connector);
        check(!p.contains(firstStroke) && !p.contains(toothInk), "Kashida contains an initial stroke or tooth");
        masks.push_back(p);
    }
    QPainterPathStroker edge; edge.setWidth(.001); edge.setCurveThreshold(1e-5);
    const auto band=edge.createStroke(ink); const auto bounds=ink.boundingRect();
    int errors=0, samples=0;
    for (double y=bounds.top()+.013;y<bounds.bottom();y+=.05)
        for (double x=bounds.left()+.017;x<bounds.right();x+=.05) {
            QPointF p(x,y);int owners=0;for(const auto& mask:masks)owners+=mask.contains(p);
            if (owners!=int(ink.contains(p))) {
                check(owners<=1 && band.contains(p), "Partition lost, fabricated or duplicated interior ink"); ++errors;
            }
            samples+=ink.contains(p);
        }
    check(errors<=std::max(3,samples/1000) && samples>1000, "Ink conservation tolerance exceeded");
    const auto bodies=sub.baseGlyphs; const auto connectors=sub.kashidaPaths;
    check(analysis.segmentSubword(12,1,5,2,nullptr,true) && sub.kashidaPaths==connectors,
          "Repeated segmentation changed connectors");
    for(size_t i=0;i<bodies.size();++i)check(bodies[i].path==sub.baseGlyphs[i].path,"Repeated segmentation changed a body");
    auto noInitial=config; noInitial.insert("initial",QJsonArray{});
    check(!analysis.segmentSubword(12,1,5,2,nullptr,true,&noInitial) &&
          sub.letterDiagnostics[0].toObject().value("status")=="no-template" &&
          (sub.kashidaPaths.empty() || sub.kashidaPaths[0].isEmpty()),
          "First kashida was emitted without a matched initial body");
    qInfo("Tooth ownership passed: initial stroke retained, closed-loop tooth rejected, final counter retained, correct fallback, no pre-initial kashida, ink conservation and determinism.");
}
