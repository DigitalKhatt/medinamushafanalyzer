// Cached candidates must equal a fresh search after every relevant input edit.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

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

static void checkRecoveryPriority(const QJsonObject& config, AFont& font) {
    struct Fixture {
        int page, line, word, subword;
        QStringList forms;
        int join = -1;
        bool hasConnector = false;
    };
    const Fixture fixtures[] = {
        {29, 14, 6, 1, {"seen-init", "beh-upright-medi", "beh-tooth-medi", "lam-bowl-fina"}},
        {416, 7, 8, 2, {"seen-init", "beh-tooth-medi", "hah-flat-medi", "waw-fina"}},
        {411, 7, 9, 3, {"lam-init", "beh-bent-medi", "kaf-bowl-fina"}},
        {6, 6, 10, 2, {"lam-hah-stacked-init", "hah-returning-lips-medi", "kaf-zigzag-medi", "beh-tooth-medi",
                       "meem-counter-fina"}, 0, true},
        {293, 7, 2, 2, {"lam-hah-stacked-init", "hah-compact-medi", "seen-compact-medi", "beh-tooth-medi",
                        "yeh-first-turn-fina"}, 0},
        {527, 14, 3, 2, {"lam-hah-stacked-init", "hah-compact-medi", "reh-rising-tail-fina"}, 0},
        {92, 6, 7, 3, {"heh-init", "meem-tailed-rim-fina"}, 0},
        {103, 6, 3, 1, {"lam-init", "feh-descending-rim-medi", "yeh-first-turn-fina"}, 1},
        {161, 4, 7, 2, {"lam-init", "meem-medi", "hah-flat-medi", "reh-rising-tail-fina"}, 2},
    };
    for (const auto& f : fixtures) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing recovery priority fixture");
        PageAnalysisResult analysis;
        QDataStream stream(&file);
        stream >> analysis.page;
        analysis.resolveWordBoundaries(f.page, &font);
        auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        if (word.wordResultInfo.subWords.empty())
            analysis.detectSubWords(f.page - 1, f.line - 1, f.word - 1, &font);
        check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Recovery priority lost a complete partition");
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        QStringList selected;
        for (const auto& letter : sub.letterDiagnostics) selected.append(letter.toObject().value("template").toString());
        check(selected == f.forms, "Recovery priority changed the selected body forms");
        if (f.join >= 0)
            check(sub.kashidaPaths.size() > size_t(f.join) &&
                  (!sub.kashidaPaths[f.join].isEmpty()) == f.hasConnector,
                  "Recovery priority changed the connector at the checked join");
    }
}

static void checkFixedAnchorSequence(const QJsonObject& config) {
    auto draft = config;
    draft.insert("anchorSearch", QJsonArray{QJsonArray{"lam-init", "beh-tooth-medi", "kaf-bowl-fina"}});
    check(PageAnalysisResult::prepareLetterForms(draft).isEmpty(), "Valid anchor sequence was rejected");
    auto invalid = draft;
    invalid.insert("anchorSearch", QJsonArray{QJsonArray{"lam-init", "kaf-bowl-fina", "beh-tooth-medi"}});
    check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Anchor sequence accepted a form in the wrong position");
    auto legacy = draft;
    legacy.remove("anchorSearch");

    QFile fontFile("input/font.dat"), pageFile("pages/page2.dat");
    check(fontFile.open(QIODevice::ReadOnly) && pageFile.open(QIODevice::ReadOnly), "Missing anchor sequence fixtures");
    AFont font;
    QDataStream fontStream(&fontFile); fontStream >> font;
    PageAnalysisResult::initQuranText();
    PageAnalysisResult analysis;
    QDataStream pageStream(&pageFile); pageStream >> analysis.page;
    analysis.resolveWordBoundaries(2, &font);

    struct Fixture { int line, word, subword; bool anchored; };
    for (const auto& f : {Fixture{7, 1, 3, true}, Fixture{6, 1, 2, false}}) {
        auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        const auto segment = [&](const QJsonObject& forms) {
            return analysis.segmentSubword(1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &forms);
        };
        check(segment(legacy), "Missing original partition for anchor search fixture");
        const auto expected = word.wordResultInfo.subWords[f.subword - 1];
        if (!f.anchored)
            check(expected.letterDiagnostics.last().toObject().value("template") == "kaf-return-fina",
                  "Deferred final form was not reached");
        check(segment(draft), "Anchor search lost a complete partition");
        const auto& actual = word.wordResultInfo.subWords[f.subword - 1];
        check(sameAnchoredPartition(actual, expected), "Anchor search changed the letter or connector geometry");
        const bool usedAnchors = !actual.templateTrace.isEmpty() && actual.templateTrace.front().startsWith("Fixed anchors");
        check(usedAnchors == f.anchored, "Anchor search did not select the expected path");
    }
    checkRecoveryPriority(legacy, font);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    checkFixedAnchorSequence(config);
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
            auto rules = config;
            auto medial = rules.value("medial").toArray();
            for (auto entry = medial.begin(); entry != medial.end(); ++entry) {
                auto rule = entry->toObject();
                if (rule.value("id") != "lam-medi") continue;
                auto policy = rule.value("cuts").toObject();
                policy.insert("shortExit", false);
                rule.insert("cuts", policy);
                *entry = rule;
                break;
            }
            rules.insert("medial", medial);
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
