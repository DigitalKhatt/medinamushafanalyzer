// The open zigzag kaf is located from its own elbow and two return branches.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }
static SearchContext geometry(QPainterPath ink, WordResultInfo::SubWordInfo& dummy) {
    ink.setFillRule(Qt::WindingFill);
    SearchContext c{.transformedPath = ink, .subword = dummy}; c.bbox = ink.boundingRect();
    QVector<PathExtrema> points; ShapeItem::getExtrema(ink, c.subPaths, points);
    for (const auto& p : points) if (!p.pathIndex) {
        c.timeSorted.insert(p); c.xSorted.insert(p); c.ySorted.insert(p);
    }
    c.setOrientation(); c.nextIter = std::next(c.xSorted.begin()); return c;
}
static auto openCandidates(SearchContext& c, const QJsonObject& settings) {
    auto pairs = contourBodyBoundaries(c, settings);
    std::erase_if(pairs, [&](const auto& pair) {
        const auto pieces = cutRegions(c.transformedPath, {}, {pair.first, pair.second}, c.orientation, true);
        return pieces.size() != 3 || toSubpaths(pieces[1].path).size() != 1;
    });
    return pairs;
}
static bool same(const Stretching& a, const Stretching& b, const QTransform& map = {}) {
    return QLineF(map.map(a.topBaseLine.point), b.topBaseLine.point).length() < 1e-4 &&
        QLineF(map.map(a.bottomBaseLine.point), b.bottomBaseLine.point).length() < 1e-4;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Invalid bundled forms");
    QJsonObject form;
    for (const auto& v : config.value("medial").toArray())
        if (v.toObject().value("id") == "kaf-zigzag-medi") form = v.toObject();
    check(form.value("body").toObject().value("counters").toInt(-1) == 0, "Open zigzag kaf must reject counters");
    const auto settings = pairedRouteSettings(form);
    check(!settings.isEmpty(), "Missing zigzag kaf contour routes");
    check(settings.value("anchor").toObject().value("flanks").toArray()[1].toObject().value("insideBody").toBool(),
          "The defining rising arm must remain inside kaf");
    auto invalid = config, badForm = form, badSettings = form.value("body").toObject().value("routes").toObject();
    auto badAnchor = badSettings.value("anchor").toObject(); auto badFlanks = badAnchor.value("flanks").toArray();
    auto badFlank = badFlanks[1].toObject(); badFlank.insert("insideBody", "yes"); badFlanks[1] = badFlank;
    badAnchor.insert("flanks", badFlanks); badSettings.insert("anchor", badAnchor); auto badBody = badForm.value("body").toObject(); badBody.insert("routes",badSettings); badForm.insert("body",badBody);
    invalid.insert("medial", QJsonArray{badForm});
    check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid insideBody setting accepted");
    QFile f("pages/page4.dat"); check(f.open(QIODevice::ReadOnly), "Page 4 fixture missing");
    PageAnalysisResult analysis; QDataStream stream(&f); stream >> analysis.page;
    auto& word = analysis.page.lines[2].words[0]; auto& sub = word.wordResultInfo.subWords[0];
    const auto shape = word.paths[sub.paths.front()];
    const auto inPage = [&](const QPainterPath& p) { auto q = shape.transform.map(p); q.translate(shape.pos); return q; };
    auto ink = inPage(shape.path); ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy; auto c = geometry(ink, dummy);
    const auto pairs = openCandidates(c, settings);
    check(pairs.size() == 1, "Zigzag kaf is absent or includes the following meem loop");
    const auto [right, left] = pairs.front();
    check(right.topBaseLine.time > 24 && right.topBaseLine.time < 25 &&
          left.topBaseLine.time == 4 && left.bottomBaseLine.time > 16 && left.bottomBaseLine.time < 17,
          "Kaf boundaries missed its own incoming branch or outgoing elbow");
    const auto curves = baseContourCurves(ink);
    const auto tangent = curves[4].pt2() - curves[4].pt1();
    check(std::abs(QPointF::dotProduct(left.bottomBaseLine.point - left.topBaseLine.point, tangent)) < 1e-5,
          "Kaf exit is not normal to its return stroke");
    Stretching unrelated{}; unrelated.topBaseLine.point = {-100, 300}; unrelated.bottomBaseLine.point = {-80, 400};
    dummy.joins.insert(unrelated); c.baseText = QStringLiteral("اختبار"); c.pairedStrokeWidth = 1000;
    const auto independent = openCandidates(c, settings);
    check(independent.size() == 1 && same(right, independent[0].first) && same(left, independent[0].second),
          "Kaf routes depend on a neighboring letter or cut");
    for (double scale : {.7, 1.6}) {
        const QTransform map(scale, 0, 0, scale, 37, -21); auto scaled = geometry(map.map(ink), dummy);
        auto rules = settings; auto anchor = rules.value("anchor").toObject(); auto flanks = anchor.value("flanks").toArray();
        for (int i = 0; i < flanks.size(); ++i) {
            auto flank = flanks[i].toObject();
            for (const auto* key : {"minX", "maxX", "minY", "maxY"})
                if (flank.contains(key)) flank.insert(key, flank.value(key).toDouble() * scale);
            flanks[i] = flank;
        }
        anchor.insert("flanks", flanks); rules.insert("anchor", anchor);
        auto entry = rules.value("right").toObject(); auto steps = entry.value("top").toArray();
        auto last = steps.last().toObject(); last.insert("minY", last.value("minY").toDouble() * scale);
        steps[steps.size()-1] = last; entry.insert("top", steps); rules.insert("right", entry);
        const auto result = openCandidates(scaled, rules);
        check(result.size() == 1 && same(right, result[0].first, map) && same(left, result[0].second, map),
              "Kaf changed under translation or page-unit scaling");
    }
    check(analysis.segmentSubword(3, 2, 0, 0, nullptr, true), "Beh-kaf-meem remains partial");
    check(sub.baseGlyphs.size() == 3 && sub.matchedTemplates.contains("kaf-zigzag-medi"), "Kaf body was not assigned");
    check(toSubpaths(sub.baseGlyphs[1].path).size() == 1 && toSubpaths(sub.baseGlyphs[2].path).size() == 2,
          "Kaf swallowed the final meem counter");
    std::vector<QPainterPath> masks;
    for (const auto& b : sub.baseGlyphs) masks.push_back(inPage(b.path));
    for (const auto& p : sub.kashidaPaths) if (!p.isEmpty()) masks.push_back(inPage(p));
    QPainterPathStroker stroker; stroker.setWidth(.001); stroker.setCurveThreshold(1e-5); const auto band = stroker.createStroke(ink);
    for (double y = c.bbox.top()+.013; y < c.bbox.bottom(); y += .07)
        for (double x = c.bbox.left()+.017; x < c.bbox.right(); x += .07) {
            const QPointF p(x,y); int owners = 0; for (const auto& mask : masks) owners += mask.contains(p);
            check(owners == int(ink.contains(p)) || (owners <= 1 && band.contains(p)), "Kaf partition lost or duplicated ink");
        }
    const auto bodies = sub.baseGlyphs; const auto connectors = sub.kashidaPaths;
    check(analysis.segmentSubword(3, 2, 0, 0, nullptr, true) && connectors == sub.kashidaPaths, "Nondeterministic connectors");
    for (size_t i = 0; i < bodies.size(); ++i) check(bodies[i].path == sub.baseGlyphs[i].path, "Nondeterministic masks");
    for (const auto at : std::array<std::array<int,3>,2>{{{5,6,2},{9,10,1}}}) {
        check(analysis.segmentSubword(3, at[0]-1, at[1]-1, at[2]-1, nullptr, true), "Other zigzag kaf remains partial");
        check(analysis.page.lines[at[0]-1].words[at[1]-1].wordResultInfo.subWords[at[2]-1]
              .matchedTemplates.contains("kaf-zigzag-medi"), "Missing zigzag template");
    }
    for (const auto at : std::array<std::array<int,4>,3>{{{29,7,1,1},{85,8,9,1},{545,10,10,2}}}) {
        QFile file(QString("pages/page%1.dat").arg(at[0])); check(file.open(QIODevice::ReadOnly), "Kaf check page missing");
        PageAnalysisResult other; QDataStream data(&file); data >> other.page;
        auto& w = other.page.lines[at[1]-1].words[at[2]-1]; auto& s = w.wordResultInfo.subWords[at[3]-1];
        const auto& sh = w.paths[s.paths.front()];
        check(other.segmentSubword(at[0]-1, at[1]-1, at[2]-1, at[3]-1, nullptr, true), "Other-page kaf remains partial");
        const int index = PageAnalysisResult::bodyLetters(s.text).indexOf(QChar(0x0643));
        check(index > 0 && s.matchedTemplates.contains("kaf-zigzag-medi") &&
              std::abs(sh.transform.map(s.baseGlyphs[index].path).boundingRect().top() -
                       sh.transform.map(sh.path).boundingRect().top()) < 1e-5,
              "Kaf's upper arm was assigned to connecting ink or its neighbor");
    }
    // This attachment needs a different entry route. An elbow alone must not
    // make it complete while assigning the entire kaf arm to the initial hah.
    QFile negativeFile("pages/page585.dat"); check(negativeFile.open(QIODevice::ReadOnly), "Attached kaf fixture missing");
    PageAnalysisResult negative; QDataStream negativeStream(&negativeFile); negativeStream >> negative.page;
    auto& nw = negative.page.lines[14].words[2]; auto& ns = nw.wordResultInfo.subWords[1];
    const auto& sh = nw.paths[ns.paths.front()]; auto ni = sh.transform.map(sh.path); ni.translate(sh.pos);
    WordResultInfo::SubWordInfo nd; auto nc = geometry(ni, nd);
    auto unguarded = settings; auto anchor = unguarded.value("anchor").toObject(); auto flanks = anchor.value("flanks").toArray();
    auto flank = flanks[1].toObject(); flank.remove("insideBody"); flanks[1] = flank; anchor.insert("flanks", flanks);
    unguarded.insert("anchor", anchor);
    check(!openCandidates(nc, unguarded).empty() && openCandidates(nc, settings).empty(),
          "Kaf candidate that loses its defining upper arm was not rejected");

    QFile tightFile("pages/page135.dat"); check(tightFile.open(QIODevice::ReadOnly), "Tight qaf–kaf fixture missing");
    PageAnalysisResult tight; QDataStream tightStream(&tightFile); tightStream >> tight.page;
    QFile fontFile("input/font.dat"); check(fontFile.open(QIODevice::ReadOnly), "Native font fixture missing");
    AFont font; QDataStream fontStream(&fontFile); fontStream >> font;
    PageAnalysisResult::initQuranText();
    tight.resolveWordBoundaries(135, &font);
    check(tight.segmentSubword(134, 9, 0, 1, &font, true, &config), "Tight qaf–kaf–meem remains partial");
    const auto& tightWord = tight.page.lines[9].words[0];
    const auto& tightSubword = tightWord.wordResultInfo.subWords[1];
    check(tightSubword.templateTrace.value(0).startsWith("Fixed anchors"), "Tight qaf–kaf–meem used the existing search");
    check(tightSubword.matchedTemplates.contains("kaf-zigzag-medi"), "Tight qaf–kaf–meem used another kaf form");
    check(tightSubword.kashidaCuts.size() == 2 &&
          QLineF(tightSubword.kashidaCuts.front().first.center(),
                 tightSubword.kashidaCuts.front().second.center()).length() < .06 &&
          !tightSubword.kashidaPaths.back().isEmpty(), "Tight qaf–kaf seam or kaf–meem connector moved");
    check(coversOriginalInk(tightWord, tightSubword), "Tight qaf–kaf–meem lost source ink");
    QFile hahFile("pages/page338.dat");
    check(hahFile.open(QIODevice::ReadOnly), "Ḥāʾ–kāf fixture missing");
    PageAnalysisResult hah;
    QDataStream hahStream(&hahFile); hahStream >> hah.page;
    hah.resolveWordBoundaries(338, &font);
    auto& hahWord = hah.page.lines[8].words[7];
    auto& hahSubword = hahWord.wordResultInfo.subWords.front();
    check(hah.segmentSubword(337, 8, 7, 0, &font, true, &config) &&
          hahSubword.templateTrace.value(0).startsWith("Fixed anchors") &&
          hahSubword.letterDiagnostics.first().toObject().value("template") == "hah-init" &&
          !hahSubword.kashidaPaths.front().isEmpty() && coversOriginalInk(hahWord, hahSubword),
          "Ḥāʾ–kāf must preserve the head and the separate connector");
    const auto hahShape = hahWord.bodyShape(hahSubword);
    WordResultInfo::SubWordInfo empty;
    auto hahSource = geometry(hahShape.transform.map(hahShape.path).translated(hahShape.pos), empty);
    const auto head = bodyConstraintForm(config, namedLetterForm(config, "hah-init"));
    check(declaredSharedBoundary(hahSource, head) &&
          QLineF(hahSubword.kashidaCuts.front().first.p1(), hahSource.res.topBaseLine.point).length() < 1e-5 &&
          QLineF(hahSubword.kashidaCuts.front().first.p2(), hahSource.res.bottomBaseLine.point).length() < 1e-5,
          "Ḥāʾ–kāf moved the cut away from ḥāʾ’s own boundary");

    QFile turnsFile("pages/page89.dat");
    check(turnsFile.open(QIODevice::ReadOnly), "Two-sided kāf turn fixture missing");
    PageAnalysisResult turns;
    QDataStream turnsStream(&turnsFile); turnsStream >> turns.page;
    check(turns.segmentSubword(88, 11, 1, 0, nullptr, true, &config), "Two-sided kāf turn did not assemble");
    const auto& turnsWord = turns.page.lines[11].words[1];
    const auto& turnsSubword = turnsWord.wordResultInfo.subWords.front();
    check(turnsSubword.kashidaCuts.size() == 2 && turnsSubword.kashidaPaths.size() == 2 &&
          !turnsSubword.kashidaPaths.front().isEmpty() && !turnsSubword.kashidaPaths.back().isEmpty(),
          "Kāf turns removed an adjacent connector");
    const auto rightTurn = turnsSubword.kashidaCuts.front().second;
    const auto leftTurn = turnsSubword.kashidaCuts.back().first;
    check(std::abs(rightTurn.dx() + rightTurn.dy()) < 1e-5 &&
          std::abs(leftTurn.dx() - leftTurn.dy()) < 1e-5 &&
          rightTurn.length() > .1 && leftTurn.length() > .1 &&
          coversOriginalInk(turnsWord, turnsSubword), "Kāf missed an entry or exit turn");

    struct FlankFixture { int page, line, word, subword, entryJoin; double entry, exit; };
    for (const auto& at : {FlankFixture{8, 12, 8, 1, 3, 40.4364, 5.19098},
                           FlankFixture{27, 11, 7, 2, 0, 26.8867, 4.18924},
                           FlankFixture{29, 4, 6, 1, 0, 36.3448, 9.16779}}) {
        QFile file(QString("pages/page%1.dat").arg(at.page));
        check(file.open(QIODevice::ReadOnly), "Rounded kāf flank fixture missing");
        PageAnalysisResult rounded;
        QDataStream stream(&file); stream >> rounded.page;
        check(rounded.segmentSubword(at.page-1, at.line-1, at.word-1, at.subword-1, nullptr, true, &config),
              "Rounded kāf flank did not assemble");
        const auto& word = rounded.page.lines[at.line-1].words[at.word-1];
        const auto& sub = word.wordResultInfo.subWords[at.subword-1];
        check(sub.joins.size() == size_t(at.entryJoin+2) && sub.kashidaPaths.size() == sub.joins.size() &&
              !sub.kashidaPaths[at.entryJoin].isEmpty() && !sub.kashidaPaths.back().isEmpty(),
              "Rounded kāf lost an adjacent connector");
        check(std::abs(std::next(sub.joins.begin(), at.entryJoin)->topLeft.time - at.entry) < .001 &&
              std::abs(sub.joins.rbegin()->topRight.time - at.exit) < .001 && coversOriginalInk(word, sub),
              "Rounded kāf missed its bend or changed the left turn");
    }
    struct EntryTurnFixture {
        int page, line, word, subword, join;
        const char* form;
        double upperTime, lowerTime;
    };
    for (const auto& at : {
             EntryTurnFixture{149, 6, 6, 2, 1, "kaf-fork-medi", 41.0496, 21.8788},
             EntryTurnFixture{280, 4, 2, 1, 0, "kaf-fork-ascender-medi", 24.2836, 11.3439}}) {
        QFile file(QString("pages/page%1.dat").arg(at.page));
        check(file.open(QIODevice::ReadOnly), "Missing kāf entry turn fixture");
        PageAnalysisResult analysis;
        QDataStream stream(&file);
        stream >> analysis.page;
        analysis.resolveWordBoundaries(at.page, &font);
        check(analysis.segmentSubword(at.page - 1, at.line - 1, at.word - 1, at.subword - 1, &font, true, &config),
              "Medial kāf entry turn did not assemble");
        const auto& word = analysis.page.lines[at.line - 1].words[at.word - 1];
        const auto& sub = word.wordResultInfo.subWords[at.subword - 1];
        check(sub.joins.size() > size_t(at.join) && !sub.kashidaPaths[at.join].isEmpty() &&
              sub.letterDiagnostics.at(at.join + 1).toObject().value("template") == at.form &&
              coversOriginalInk(word, sub), "Medial kāf turn changed the body form or lost connecting ink");
        const auto& entry = *std::next(sub.joins.begin(), at.join);
        check(std::abs(entry.topLeft.time - at.upperTime) < .001 &&
              std::abs(entry.bottomLeft.time - at.lowerTime) < .001,
              "Medial kāf entry stopped away from the turn");
    }
    qInfo("Zigzag medial kaf passed: intrinsic routes, normal exit, preserved counter, independent geometry, scale, determinism and conserved ink.");
}
