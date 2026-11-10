// Shared counter/route evidence belongs to the uncut ink, not a neighboring join.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static QByteArray snapshot(const SharedShapeEvidence& e) {
    QByteArray bytes; QDataStream out(&bytes, QIODevice::WriteOnly);
    const auto point = [&](const PathExtrema& p) { out << p.pathIndex << p.time << p.point << p.isSharp << p.isX << p.isY; };
    const auto points = [&](const auto& values) { out << quint64(values.size()); for (const auto& p : values) point(p); };
    const auto hole = [&](const CounterHole& h) { out << quint64(h.index) << h.box; };
    const auto corner = [&](const CounterCornerEvidence& c) {
        out << c.counter << c.maxDistance << c.upperLimit << c.upper << c.top << c.paired << quint64(c.corners.size());
        for (const auto& p : c.corners) { point(p.anchor); out << p.tangent << p.distance; points(p.oppositeCorners); }
    };
    out << e.source << quint64(e.holes.size());
    for (const auto& h : e.holes) hole(h);
    out << bool(e.initialNeck);
    if (e.initialNeck) { out << e.initialNeck->counter; points(e.initialNeck->valleys); points(e.initialNeck->normalAnchors); }
    out << bool(e.terminalCounter); if (e.terminalCounter) hole(*e.terminalCounter);
    out << bool(e.terminalContour); if (e.terminalContour) hole(*e.terminalContour);
    corner(e.terminalNeck); points(e.shoulders);
    out << bool(e.bridge);
    if (e.bridge) {
        out << e.bridge->hole; point(e.bridge->tip); out << quint64(e.bridge->pairs.size());
        for (const auto& pair : e.bridge->pairs) { point(pair.first); point(pair.second); }
    }
    out << quint64(e.loops.size());
    for (const auto& loop : e.loops) {
        hole(loop.anchor); out << loop.hole << loop.bandY << loop.valid; corner(loop.entry); corner(loop.exit);
    }
    out << quint64(e.rims.size());
    for (const auto& rim : e.rims) { hole(rim.counter); points(rim.anchors); }
    out << bool(e.route);
    if (e.route) {
        point(e.route->start); point(e.route->origin); point(e.route->anchor); point(e.route->topBefore); point(e.route->topEnd);
        out << e.route->forwardPrevious << e.route->topPrevious << e.route->endpointTolerance;
    }
    out << bool(e.head);
    if (e.head) {
        out << e.head->counter << bool(e.head->edge); if (e.head->edge) point(*e.head->edge);
        out << quint64(e.head->necks.size());
        for (const auto& neck : e.head->necks) {
            point(neck.crest); out << neck.distance << bool(neck.route);
            if (neck.route) {
                point(neck.route->start); point(neck.route->origin); point(neck.route->anchor);
                point(neck.route->topBefore); point(neck.route->topEnd);
                out << neck.route->forwardPrevious << neck.route->topPrevious << neck.route->endpointTolerance;
            }
        }
    }
    out << quint64(e.crestedHeads.size());
    for (const auto& head : e.crestedHeads) { out << head.counter; point(head.crest); point(head.rightEdge); point(head.leftEdge); }
    out << bool(e.bowlTurn); if (e.bowlTurn) { point(e.bowlTurn->tip); point(e.bowlTurn->turn); }
    out << quint64(e.outlines.size());
    for (const auto& shape : e.outlines) { out << shape.eye << quint64(shape.counters.size()); for (const auto& h : shape.counters) hole(h); }
    return bytes;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Invalid declared forms");
    QVector<QJsonObject> forms;
    QSet<QString> shapes;
    for (const auto* phase : {"initial", "medial", "final"}) for (const auto& value : config.value(phase).toArray()) {
        const auto form = resolvedBodyForm(config, value.toObject());
        if (!sharedShapeDescription(form.value("body").toObject())) continue;
        check(sharedShapeDescription(form.value("body").toObject()) && !form.contains("method") &&
              !form.contains("loopBoundaries") && !form.contains("counter") && !form.contains("bodyOutline"),
              "A declared form still dispatches through legacy geometry");
        forms.append(form); shapes.insert(form.value("body").toObject().value("shape").toString());
        auto invalid = config, bad = form, body = form.value("body").toObject();
        body.insert("normalCut", true); bad.insert("body", body);
        invalid.insert(phase, QJsonArray{bad});
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Cut policy accepted in body recognition");
        invalid = config; bad = form;
        auto policy = form.value("cuts").toObject(); policy.insert("boundaries", QJsonObject{{"right", "unknown"}, {"left", "free"}});
        bad.insert("cuts", policy); invalid.insert(phase, QJsonArray{bad});
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Unknown boundary policy accepted");
    }
    check(forms.size() >= 29 && shapes.size() >= 10, "Not all shared shapes exercised");
    struct Fixture { int page,line,word,sub; };
    const Fixture fixtures[] = {{3,2,9,1}, {6,15,4,1}, {600,2,5,1}, {230,11,3,1}};
    int checks = 0;
    for (const auto& f : fixtures) {
        QFile file(QString("pages/page%1.dat").arg(f.page)); check(file.open(QIODevice::ReadOnly), "Missing source fixture");
        PageAnalysisResult analysis; QDataStream in(&file); in >> analysis.page;
        const auto& word = analysis.page.lines[f.line-1].words[f.word-1];
        const auto shape = word.bodyShape(word.wordResultInfo.subWords[f.sub-1]);
        WordResultInfo::SubWordInfo empty;
        SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos), .subword = empty};
        check(readTerminalOutline(context), "Missing source outline");
        OutlineSearchCache cache; context.outlineCache = &cache;
        for (const auto& form : forms) {
            const auto body = form.value("body").toObject();
            const auto expected = snapshot(recognizeSharedShape(inkOutline(context), body));
            const auto saved = recognizedSharedShape(context, body);
            context.nextIter = context.xSorted.end(); context.pairedStrokeWidth = 10000;
            context.baseText = QStringLiteral("unrelated"); context.joinDefaults = {{"enabled", false}};
            context.form = {{"bottomProbe", "invalid"}, {"minRise", 1e9}};
            context.res.topBaseLine.point = {-1e6, 1e6}; empty.joins.insert(context.res);
            check(snapshot(recognizeSharedShape(inkOutline(context), body)) == expected,
                  "Neighbor/cursor/width state changed intrinsic evidence");
            auto edited = form;
            auto policy = form.value("cuts").toObject(); policy.insert("counter", QJsonObject{{"offset", 1000}});
            edited.insert("cuts", policy);
            check(recognizedSharedShape(context, edited.value("body").toObject()) == saved && snapshot(*saved) == expected,
                  "A policy edit changed cached recognition");
            ++checks;
        }
    }
    qInfo("Shared evidence passed: %d fresh recognition/neighbor/cache comparisons, %d declarations and %d shapes.", checks, forms.size(), shapes.size());
}
