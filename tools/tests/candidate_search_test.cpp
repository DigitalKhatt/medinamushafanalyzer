// Cached candidates must equal a fresh search after every relevant input edit.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static QByteArray snapshot(const std::vector<MedialCandidate>& candidates) {
    QByteArray bytes;
    for (const auto& c : candidates) {
        appendCandidateBoundary(bytes, c.right); appendCandidateBoundary(bytes, c.left);
        appendGeometryKey(bytes, c.sharedJoins);
        appendGeometryKey(bytes, bool(c.hollowCuts));
        if (c.hollowCuts) {
            appendCandidateBoundary(bytes, c.hollowCuts->first);
            appendCandidateBoundary(bytes, c.hollowCuts->second);
        }
        for (const auto& p : {c.lamCrest, c.lamFork}) {
            appendGeometryKey(bytes, bool(p));
            if (p) appendGeometryAnchor(bytes, *p);
        }
    }
    return bytes;
}

static QByteArray snapshot(const std::vector<ToothBody>& candidates) {
    QByteArray bytes;
    for (const auto& c : candidates) {
        appendGeometryAnchor(bytes, c.tip); appendGeometryAnchor(bytes, c.bend);
        appendCandidateBoundary(bytes, c.left);
        appendGeometryKey(bytes, bool(c.right));
        if (c.right) appendCandidateBoundary(bytes, *c.right);
        appendGeometryKey(bytes, c.curvedAxis.size());
        for (const auto& p : c.curvedAxis) appendGeometryPoint(bytes, p);
    }
    return bytes;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    CutGeometryMemoScope memo;
    cutGeometryMemo->profiling = true;
    int lamPairs = 0, toothPairs = 0;
    struct Fixture { int page, line, word, sub; };
    for (const auto f : {Fixture{3,1,5,1}, Fixture{600,2,4,2}, Fixture{286,2,2,1}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Candidate fixture missing");
        PageAnalysisResult analysis;
        QDataStream in(&file); in >> analysis.page;
        check(analysis.segmentSubword(f.page-1,f.line-1,f.word-1,f.sub-1,nullptr,true),
              "Candidate fixture failed segmentation");
        const auto& word = analysis.page.lines[f.line-1].words[f.word-1];
        const auto& sub = word.wordResultInfo.subWords[f.sub-1];
        const auto shape = word.bodyShape(sub);
        WordResultInfo::SubWordInfo empty;
        SearchContext context{.transformedPath=shape.transform.map(shape.path).translated(shape.pos), .subword=empty};
        OutlineSearchCache cache;
        check(preparePassOutline(context, cache), "Candidate outline preparation failed");
        context.outlineCache = &cache;
        context.bbox = context.transformedPath.boundingRect();
        context.nextIter = context.xSorted.begin();
        context.pairedStrokeWidth = .5;
        const auto letters = PageAnalysisResult::bodyLetters(sub.text);
        for (int i=1; i+1<letters.size(); ++i) {
            if (letters[i] != QChar(0x0644)) continue;
            auto incoming = *std::next(sub.joins.begin(),i-1), outgoing = *std::next(sub.joins.begin(),i);
            const auto entry = QLineF(incoming.topBaseLine.point,incoming.bottomBaseLine.point).center();
            const auto exit = QLineF(outgoing.topBaseLine.point,outgoing.bottomBaseLine.point).center();
            const double stroke = incoming.referenceWidth > 0 ? incoming.referenceWidth :
                QLineF(incoming.topBaseLine.point,incoming.bottomBaseLine.point).length();
            const auto form = resolvedBodyForm(config,namedLetterForm(config,"lam-medi"));
            const auto verify = [&](const QJsonObject& rules, const QJsonObject& declaration, const Stretching& before,
                                    QPointF at, double width, bool open, bool recovery,
                                    const std::optional<Stretching>& shared) {
                const auto evidence = recognizedMedialLamBodies(context,declaration.value("body").toObject());
                const auto expected = computeMedialLamCuts(context,rules,declaration,*evidence,before,at,exit,
                                                            width,open,recovery,true,shared);
                for (int repeat=0; repeat<2; ++repeat) {
                    const auto actual = assembleMedialLamCuts(context,rules,declaration,*evidence,before,at,exit,
                                                               width,open,recovery,true,shared);
                    check(snapshot(actual)==snapshot(expected), "Cached lam candidates differ from a fresh search");
                }
                lamPairs += expected.size();
            };
            verify(config,form,incoming,entry,stroke,false,false,{});
            verify(config,form,incoming,entry,stroke,false,true,{});
            verify(config,form,incoming,entry,stroke,true,true,{});
            verify(config,form,incoming,entry+QPointF(.1,0),stroke,false,true,{});
            verify(config,form,incoming,entry,stroke*1.01,false,true,{});
            verify(config,form,incoming,entry,stroke,false,true,outgoing);
            auto shifted = incoming;
            shifted.topBaseLine.point.rx() += .01;
            verify(config,form,shifted,entry,stroke,false,true,{});
            auto edited = form;
            auto cuts = edited.value("cuts").toObject(); cuts.insert("forkOffset",.35); edited.insert("cuts",cuts);
            verify(config,edited,incoming,entry,stroke,false,true,{});
            auto rules = config; rules.insert("_disableLamShortExit",true);
            verify(rules,form,incoming,entry,stroke,false,true,{});
            context.pairedStrokeWidth = .7;
            verify(config,form,incoming,entry,stroke,false,true,{});
        }
        for (const auto* id : {"beh-tooth-medi","beh-upright-medi","beh-bent-medi"}) {
            auto form = resolvedBodyForm(config,namedLetterForm(config,id));
            const auto verify = [&](const QJsonObject& declaration) {
                const auto expected = computeToothBoundaries(context,declaration);
                for (int repeat=0; repeat<2; ++repeat) {
                    const auto actual = declaredToothBoundaries(context,declaration);
                    const auto a = snapshot(actual), b = snapshot(expected);
                    if (a != b) {
                        int first=0;
                        while(first<std::min(a.size(),b.size()) && a[first]==b[first]) ++first;
                        qWarning() << f.page << id << repeat << actual.size() << expected.size() << "byte" << first
                                   << a.mid(first,32).toHex() << b.mid(first,32).toHex();
                    }
                    check(a==b,"Cached tooth candidates differ from a fresh search");
                }
                toothPairs += expected.size();
            };
            verify(form);
            auto cuts = form.value("cuts").toObject(); cuts.insert("leftOffset",.65); form.insert("cuts",cuts);
            verify(form);
            auto body = form.value("body").toObject(); body.insert("maxPairDistance",1.1); form.insert("body",body);
            verify(form);
        }
        // Reusing the same cache with moved source geometry cannot retain old coordinates.
        context.transformedPath.translate(3,-2);
        check(preparePassOutline(context,cache), "Moved outline preparation failed");
        context.bbox = context.transformedPath.boundingRect();
        context.nextIter = context.xSorted.begin();
        const auto form = resolvedBodyForm(config,namedLetterForm(config,"beh-tooth-medi"));
        check(snapshot(declaredToothBoundaries(context,form))==snapshot(computeToothBoundaries(context,form)),
              "A source edit reused stale tooth coordinates");
    }
    check(lamPairs>0 && toothPairs>0, "Candidate tests only exercised empty results");
    bool reused = false;
    for (const auto& s : cutGeometryMemo->forms) reused |= s.requests>s.builds;
    check(reused, "No candidate inventory was reused");
    qInfo("Candidate search passed: %d lam pairs, %d tooth pairs; policies, placements, recovery and source edits.",lamPairs,toothPairs);
}
