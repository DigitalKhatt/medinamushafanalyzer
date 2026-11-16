#include "../../cutter/detail.h"
#include "../../pathmask.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

static SearchContext geometry(QPainterPath ink, WordResultInfo::SubWordInfo& dummy) {
    ink.setFillRule(Qt::WindingFill);
    SearchContext context{.transformedPath = ink, .subword = dummy};
    context.bbox = ink.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(ink, context.subPaths, extrema);
    for (const auto& point : extrema) if (point.pathIndex == 0) {
        context.timeSorted.insert(point); context.xSorted.insert(point); context.ySorted.insert(point);
    }
    context.setOrientation();
    context.nextIter = std::next(context.xSorted.begin());
    return context;
}

static bool same(const Stretching& a, const Stretching& b, const QTransform& mapping = {}) {
    return QLineF(mapping.map(a.topBaseLine.point), b.topBaseLine.point).length() < 1e-4 &&
        QLineF(mapping.map(a.bottomBaseLine.point), b.bottomBaseLine.point).length() < 1e-4;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    {
        QFile fixture("pages/page318.dat");
        check(fixture.open(QIODevice::ReadOnly), "Missing qaf–alef turn fixture");
        PageAnalysisResult page;
        QDataStream stream(&fixture);
        stream >> page.page;
        check(page.segmentSubword(317, 6, 5, 0, nullptr, true), "Qaf–alef remains partial");
        const auto& word = page.page.lines[6].words[5];
        const auto& sub = word.wordResultInfo.subWords[0];
        check(sub.matchedTemplates == QStringList{"qaf-init", "alef-fina"} &&
              sub.baseGlyphs.size() == 2 && sub.kashidaPaths.size() == 1 && !sub.kashidaPaths.front().isEmpty(),
              "Qaf–alef turned into a shared cut");
        const auto& turn = sub.kashidaCuts.front().second;
        check(std::abs(turn.dx() + turn.dy()) < 1e-5 && coversOriginalInk(word, sub),
              "The 45-degree cut must pass the ordinary ink-coverage check");
        auto missingConnector = sub;
        missingConnector.kashidaPaths.front() = {};
        check(!coversOriginalInk(word, missingConnector), "Coverage accepted genuinely missing connector ink");
    }
    {
        QFile fixture("pages/page518.dat");
        check(fixture.open(QIODevice::ReadOnly), "Missing Bézier turn fixture");
        PageAnalysisResult page;
        QDataStream stream(&fixture);
        stream >> page.page;
        check(page.segmentSubword(517, 11, 0, 0, nullptr, true), "Page 518 alef remains partial");
        const auto& word = page.page.lines[11].words[0];
        const auto& sub = word.wordResultInfo.subWords[0];
        check(sub.matchedTemplates.last() == "alef-fina" && sub.baseGlyphs.size() == 5 &&
              sub.kashidaCuts.size() == 4 && !sub.kashidaPaths.back().isEmpty() && coversOriginalInk(word, sub),
              "The alef turn must retain the neighboring bodies and connector");
        const auto shape = word.bodyShape(sub);
        WordResultInfo::SubWordInfo dummy;
        const auto context = geometry(shape.transform.map(shape.path).translated(shape.pos), dummy);
        const auto curves = baseContourCurves(context.transformedPath);
        const auto pointAt = [&](int index, double t) {
            return PathExtrema{0, index + t, curves.at(index).pointAt(t), false, false};
        };
        const auto inner = pointAt(34, .02);
        const auto& join = *sub.joins.rbegin();
        Stretching seamCut{};
        check(alefTurnCut(context, pointAt(32, .60), 45, .65, seamCut, &inner, -1) &&
              seamCut.topBaseLine.time == 33 &&
              same(seamCut, {.topBaseLine = join.topLeft, .bottomBaseLine = join.bottomLeft}),
              "The selected alef cut must start at the common Bézier endpoint");
        const auto outer = pointAt(0, .65);
        Stretching outerCut{};
        check(alefTurnCut(context, pointAt(2, .11), 45, .65, outerCut, &outer, 1) &&
              std::abs(outerCut.topBaseLine.time - 1.67454) < 1e-4 &&
              std::abs(sectionLine(outerCut).dx() + sectionLine(outerCut).dy()) < 1e-5 &&
              interiorChord(context.transformedPath, outerCut.topBaseLine, outerCut.bottomBaseLine),
              "The outer flank normal must cut across the ink at the turn");
    }
    {
        QFile fixture("pages/page197.dat");
        check(fixture.open(QIODevice::ReadOnly), "Missing horizontal stitch fixture");
        PageAnalysisResult page;
        QDataStream stream(&fixture);
        stream >> page.page;
        check(page.segmentSubword(196, 2, 0, 0, nullptr, true),
              "A horizontal stitch on the alef shaft must not replace the foot turn");
    }
    struct JoinedHamzaCase { int page, line, word; };
    for (const auto& sample : {JoinedHamzaCase{324, 4, 7}, JoinedHamzaCase{341, 7, 3}}) {
        QFile fixture(QString("pages/page%1.dat").arg(sample.page));
        check(fixture.open(QIODevice::ReadOnly), "Missing joined hamza fixture");
        PageAnalysisResult page;
        QDataStream stream(&fixture);
        stream >> page.page;
        const auto& word = page.page.lines[sample.line - 1].words[sample.word - 1];
        const auto source = word.bodyShape(word.wordResultInfo.subWords.front());
        const auto pieces = splitJoinedAlefHamza(source);
        check(pieces.has_value(), "The cutter must split the flagged joined hamza");
        const auto mask = [&](const QPainterPath& path) {
            const auto scaled = QTransform::fromScale(16. / 3., 16. / 3.)
                                    .map(source.transform.map(path).translated(source.pos));
            QRegion result;
            visitSourceInk(scaled, scaled.boundingRect().toAlignedRect(),
                           [&](int x, int y, int width) { result += QRect(x, y, width, 1); });
            return result;
        };
        const auto stem = mask(pieces->first), hamza = mask(pieces->second);
        check(!stem.isEmpty() && !hamza.isEmpty() && !stem.intersects(hamza) &&
              (stem | hamza) == mask(source.path), "The split must preserve and partition source ink");
        auto plainStem = source;
        plainStem.path = pieces->first;
        check(!splitJoinedAlefHamza(plainStem), "No split should be returned for an outline without the bridge");
    }
    const auto rootsAre = [](double a, double b, double c, double d, std::vector<double> expected,
                             RootSearch search = RootSearch::Standard) {
        std::array<double, 3> storage;
        auto roots = cubicRoots(a, b, c, d, storage, search);
        std::sort(roots.begin(), roots.end());
        check(roots.size() == expected.size(), "Wrong number of polynomial intersections");
        for (size_t i = 0; i < roots.size(); ++i)
            check(std::abs(roots[i] - expected[i]) < 1e-10, "Incorrect polynomial intersection");
    };
    for (double scale : {1e-9, 1.0, 1e9}) {
        rootsAre(0, scale, -scale, .1875 * scale, {.25, .75});
        rootsAre(1e-15 * scale, scale, -scale, .1875 * scale, {.25, .75});
        rootsAre(0, 0, 2 * scale, -scale, {.5});
        rootsAre(0, scale, -scale, .25 * scale, {.5});
        rootsAre(0, scale, 0, scale, {});
        rootsAre(0, 0, scale, -2 * scale, {});
        rootsAre(0, scale, -scale, 0, {0, 1});
        rootsAre(0, 0, 0, scale, {});
        rootsAre(scale, -1.5 * scale, .66 * scale, -.08 * scale, {.2, .5, .8});
    }
    // The vertical probe through the left lip of a medial ain crosses a
    // nearly quadratic Bézier. The analytic cubic solver loses this crossing.
    rootsAre(1.1368683772161603e-13, -.018000000000256478, -3.4799999999999045,
             1.9740053218317826, {.5655883044509782}, RootSearch::BracketMissedCrossing);
    rootsAre(1e-12, 1, -1, .1875, {.25, .75}, RootSearch::BracketAllCrossings);
    rootsAre(0, 0, 0, 0, {});
    QPainterPath shoulder;
    shoulder.moveTo(0, 0);
    shoulder.cubicTo(1, 0, 2, 2, 2, 2);
    shoulder.cubicTo(1, 3, 0, 4, 0, 4);
    shoulder.closeSubpath();
    std::vector<QPainterPath> contours;
    QVector<PathExtrema> landmarks;
    {
        CutGeometryMemoScope memo;
        memo.owned->preciseGeometry = true;
        readContourLandmarks(shoulder, contours, landmarks);
    }
    check(std::any_of(landmarks.begin(), landmarks.end(), [](const auto& point) {
        return point.time == 1 && point.isSharp && point.isX;
    }), "Missing contour corner at a repeated control point");
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject form;
    for (const auto& value : config.value("final").toArray())
        if (value.toObject().value("id") == "alef-fina") form = value.toObject();
    check(!form.isEmpty(), "Missing alef template");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[9].words[5];
    auto& sub = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[sub.paths.front()];
    auto ink = shape.transform.map(shape.path); ink.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(ink, dummy);
    const double y = std::prev(context.xSorted.end())->point.y() + context.bbox.height() * .25;
    const QLineF probe(context.bbox.left() - 1, y, context.bbox.right() + 1, y);
    const auto hits = FindInterections(ink, probe, true);
    check(hits.size() == 2, "The stem cross-section lost a contour edge");
    check(std::any_of(hits.begin(), hits.end(), [](const auto& hit) {
        return std::abs(hit.time - 2.744423) < 1e-5;
    }), "Quadratic stem edge intersection is missing");
    const auto project=[&](SearchContext& source, const QJsonObject& declaration, Stretching& result) {
        const auto evidence=recognizeUprightStem(inkOutline(source));
        return evidence && cutUprightStem(source,*evidence,declaration.value("cuts").toObject().value("stem").toObject(),result);
    };
    Stretching cut{};
    check(project(context, form, cut) && std::abs(cut.topBaseLine.time - 14.1344) < 1e-4 &&
          std::abs(cut.bottomBaseLine.time - 4.59083) < 1e-4, "Alef did not find its own turning foot");
    const QPointF normal = cut.topBaseLine.point - cut.bottomBaseLine.point;
    check(std::abs(normal.x() + normal.y()) < 1e-6 && interiorChord(ink, cut.topBaseLine, cut.bottomBaseLine),
          "Alef cut is not the interior normal at the 45-degree turn");
    context.baseText = QStringLiteral("بدون سياق"); context.pairedStrokeWidth = 1000;
    context.nextIter = context.xSorted.end();
    Stretching independent{};
    check(project(context, form, independent) && same(cut, independent), "Neighbor state changed the alef cut");
    for (double scale : {.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(ink), dummy);
        Stretching result{};
        check(project(scaled, form, result) && same(cut, result, mapping),
              "Alef cut changed under scale/translation");
    }
    check(analysis.segmentSubword(599, 9, 5, 0, nullptr, true), "Noon-alef is still partial");
    check(sub.matchedTemplates == QStringList{"beh-init", "alef-fina"} &&
          sub.baseGlyphs.size() == 2 && sub.kashidaPaths.size() == 1 && !sub.kashidaPaths[0].isEmpty(),
          "Expected existing templates, two bodies and one kashida");
    std::vector<QPainterPath> masks;
    for (const auto& body : sub.baseGlyphs) {
        auto p = shape.transform.map(body.path); p.translate(shape.pos); masks.push_back(p);
    }
    for (const auto& kashida : sub.kashidaPaths) {
        auto p = shape.transform.map(kashida); p.translate(shape.pos); masks.push_back(p);
    }
    check(masks[1].contains(QPointF(182.4, 321)) && masks[0].contains(QPointF(187.8, 327)) &&
          masks[2].contains(QPointF(186, 331)), "Alef stem, noon body or connecting bowl has the wrong owner");
    const auto box = ink.boundingRect().adjusted(-.1, -.1, .1, .1);
    int samples = 0;
    for (double yy = box.top() + .013; yy < box.bottom(); yy += .07)
        for (double x = box.left() + .017; x < box.right(); x += .07) {
            const QPointF p(x, yy); int count = 0;
            for (const auto& mask : masks) count += mask.contains(p);
            check(count == int(ink.contains(p)), "Partition lost, duplicated or fabricated ink");
            samples += ink.contains(p);
        }
    check(samples > 1000, "Ink check too sparse");
    const auto bodies = sub.baseGlyphs; const auto kashidas = sub.kashidaPaths;
    check(analysis.segmentSubword(599, 9, 5, 0, nullptr, true) && sub.kashidaPaths == kashidas,
          "Repeated segmentation changed connectors");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(sub.baseGlyphs[i].path == bodies[i].path, "Repeated segmentation changed body masks");
    qInfo("Alef stem passed: quadratic/linear/tangent/endpoint roots, coefficient scaling, retained cubic roots, two stem edges, own 45-degree normal, scale/translation, two bodies, one kashida, ink conservation and determinism.");
}
