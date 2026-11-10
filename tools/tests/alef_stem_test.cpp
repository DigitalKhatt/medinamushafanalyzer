#include "../../segmentsubword.cpp"
#include <QApplication>

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
    const auto rootsAre = [](double a, double b, double c, double d, std::vector<double> expected) {
        auto roots = cubicRoots(a, b, c, d);
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
    rootsAre(0, 0, 0, 0, {});
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
