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
    qInfo("Zigzag medial kaf passed: intrinsic routes, normal exit, preserved counter, independent geometry, scale, determinism and conserved ink.");
}
