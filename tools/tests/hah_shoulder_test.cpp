// A shoulder-entry hah head, measured independently of surrounding letters.
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
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
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
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject rule;
    QJsonArray withoutShoulder;
    for (const auto& value : config.value("medial").toArray()) {
        if (value.toObject().value("id") == "hah-shoulder-medi") rule = value.toObject();
        else withoutShoulder.append(value);
    }
    check(!rule.isEmpty() && rule.value("sharedEntry").toBool(), "Missing shared shoulder template");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[6].words[6];
    auto& sub = word.wordResultInfo.subWords[1];
    const auto& shape = word.paths[sub.paths.front()];
    auto ink = shape.transform.map(shape.path); ink.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(ink, dummy);
    const auto settings = pairedRouteSettings(rule);
    const auto pairs = contourBodyBoundaries(context, settings);
    check(pairs.size() == 1, "Expected one intrinsic hah shoulder");
    const auto& [right, left] = pairs.front();
    check(std::abs(right.topBaseLine.time - 4.15034) < 1e-4 &&
          std::abs(right.bottomBaseLine.time - 32.3086) < 1e-4 &&
          std::abs(left.topBaseLine.time - 11.4003) < 1e-4 &&
          std::abs(left.bottomBaseLine.time - 26.3508) < 1e-4,
          "Cuts left the shoulder or outgoing neck");
    const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
    check(pieces.size() == 3 && pieces[1].path.contains(QPointF(145.6, 247.3)) &&
          pieces[1].path.contains(QPointF(153.0, 248.2)) &&
          !pieces[1].path.contains(QPointF(147.3, 240)) &&
          !pieces[1].path.contains(QPointF(143.1, 249.2)),
          "Hah lost its head or swallowed the lam stem/beh tooth");
    context.baseText = QStringLiteral("بدون سياق");
    context.pairedStrokeWidth = 1000;
    context.nextIter = context.xSorted.end();
    Stretching unrelated{}; unrelated.topBaseLine.point = {-500, 300};
    unrelated.bottomBaseLine.point = {200, -100}; dummy.joins.insert(unrelated);
    const auto independent = contourBodyBoundaries(context, settings);
    check(independent.size() == 1 && same(right, independent[0].first) && same(left, independent[0].second),
          "Neighboring letter state changed the shoulder cuts");
    for (double scale : {.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(ink), dummy);
        const auto result = contourBodyBoundaries(scaled, settings);
        check(result.size() == 1 && same(right, result[0].first, mapping) && same(left, result[0].second, mapping),
              "Shoulder cuts changed under scale/translation");
    }
    QPainterPath oval; oval.addEllipse(QRectF(0, 0, 15, 6));
    auto smooth = geometry(oval, dummy);
    check(contourBodyBoundaries(smooth, settings).empty(), "Smooth bowl mistaken for a shoulder head");
    // Recreate the earlier template set, before recovery-only allographs.
    auto previous = config; previous.remove("reconsiderForms"); previous.insert("medial", withoutShoulder);
    previous.insert("retryUnresolvedForms", false);
    check(!analysis.segmentSubword(599, 6, 6, 1, nullptr, true, &previous), "Missing shoulder fixture is no longer partial");
    const auto lamCut = *sub.joins.begin();
    check(analysis.segmentSubword(599, 6, 6, 1, nullptr, true), "Shoulder did not unblock remaining letters");
    check(sub.baseGlyphs.size() == 4 && sub.kashidaPaths.size() == 3 && sub.kashidaPaths[0].isEmpty() &&
          !sub.kashidaPaths[1].isEmpty() && !sub.kashidaPaths[2].isEmpty() && same(lamCut, *sub.joins.begin()),
          "Wrong body/kashida count or changed shared lam boundary");
    for (const auto& diagnostic : sub.letterDiagnostics)
        check(diagnostic.toObject().value("status") == "matched", "Unresolved letter was reported as complete");
    std::vector<QPainterPath> masks;
    for (const auto& body : sub.baseGlyphs) {
        auto path = shape.transform.map(body.path); path.translate(shape.pos); masks.push_back(path);
    }
    for (const auto& kashida : sub.kashidaPaths) if (!kashida.isEmpty()) {
        auto path = shape.transform.map(kashida); path.translate(shape.pos); masks.push_back(path);
    }
    const auto bounds = ink.boundingRect().adjusted(-.1, -.1, .1, .1);
    int samples = 0;
    for (double y = bounds.top() + .013; y < bounds.bottom(); y += .07)
        for (double x = bounds.left() + .017; x < bounds.right(); x += .07) {
            const QPointF p(x, y); int count = 0;
            for (const auto& mask : masks) count += mask.contains(p);
            check(count == int(ink.contains(p)), "Ink was lost, duplicated or fabricated");
            samples += ink.contains(p);
        }
    check(samples > 1000, "Ink conservation check too sparse");
    const auto bodies = sub.baseGlyphs;
    const auto kashidas = sub.kashidaPaths;
    check(analysis.segmentSubword(599, 6, 6, 1, nullptr, true) && sub.kashidaPaths == kashidas,
          "Repeated segmentation changed connectors");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(bodies[i].path == sub.baseGlyphs[i].path, "Repeated segmentation changed body masks");
    qInfo("Hah shoulder passed: intrinsic entry/exit, retained head, neighbor independence, scale/translation, smooth-bowl rejection, four bodies, two kashidas, shared lam boundary, ink conservation and determinism.");
}
