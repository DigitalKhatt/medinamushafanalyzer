// Compact consecutive hah-family heads must retain separate body ownership.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

static SearchContext geometry(QPainterPath ink, WordResultInfo::SubWordInfo& dummy) {
    ink.setFillRule(Qt::WindingFill);
    SearchContext context{.transformedPath = ink, .subword = dummy};
    context.bbox = ink.boundingRect();
    QVector<PathExtrema> points;
    ShapeItem::getExtrema(ink, context.subPaths, points);
    for (const auto& point : points) if (point.pathIndex == 0) {
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
    check(!config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool(),
          "Regression must run with the kashida tracker disabled");
    QJsonObject rule;
    QJsonArray otherMedialForms;
    for (const auto& value : config.value("medial").toArray()) {
        if (value.toObject().value("id") == "hah-compact-medi") rule = value.toObject();
        else otherMedialForms.append(value);
    }
    check(!rule.isEmpty(), "Compact hah template is missing");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 is missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[13].words[11];
    auto& sub = word.wordResultInfo.subWords[1];
    const auto& shape = word.paths[sub.paths.front()];
    auto ink = shape.transform.map(shape.path); ink.translate(shape.pos);
    ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(ink, dummy);
    const auto settings = rule.value("contourBody").toObject();
    const auto pairs = contourBodyBoundaries(context, settings);
    check(pairs.size() == 1, "Expected one compact head, distinct from the preceding jeem");
    const auto [right, left] = pairs.front();
    check(std::abs(right.topBaseLine.time - 9.19842) < 1e-4 &&
          std::abs(right.bottomBaseLine.time - 36.1896) < 1e-4 &&
          std::abs(left.topBaseLine.time - 17.8627) < 1e-4 &&
          std::abs(left.bottomBaseLine.time - 33.2976) < 1e-4,
          "Compact head boundaries left their incoming/outgoing strokes");
    const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
    check(pieces.size() == 3 && pieces[1].path.contains(QPointF(115.6, 434.7)) &&
          pieces[1].path.contains(QPointF(117, 437.5)) &&
          !pieces[1].path.contains(QPointF(125.9, 431)) &&
          !pieces[1].path.contains(QPointF(113.3, 435.8)),
          "Compact hah lost its head/bottom stroke or swallowed a neighboring body");
    // The proposal must depend on the head geometry, not a previous join or text.
    Stretching unrelated{};
    unrelated.topBaseLine.point = {-500, 300};
    unrelated.bottomBaseLine.point = {200, -100};
    dummy.joins.insert(unrelated);
    context.baseText = QStringLiteral("بدون سياق");
    context.pairedStrokeWidth = 1000;
    context.nextIter = context.xSorted.end();
    const auto independent = contourBodyBoundaries(context, settings);
    check(independent.size() == 1 && same(right, independent[0].first) && same(left, independent[0].second),
          "Neighboring state changed compact hah boundaries");
    for (double scale : {.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(ink), dummy);
        const auto result = contourBodyBoundaries(scaled, settings);
        check(result.size() == 1 && same(right, result[0].first, mapping) && same(left, result[0].second, mapping),
              "Compact head boundaries changed under scale/translation");
    }
    QPainterPath oval; oval.addEllipse(QRectF(0, 0, 15, 6));
    auto smooth = geometry(oval, dummy);
    check(contourBodyBoundaries(smooth, settings).empty(), "Smooth bowl mistaken for compact hah");

    // Recreate the earlier template set, before recovery-only allographs.
    auto previous = config; previous.remove("reconsiderForms"); previous.insert("medial", otherMedialForms);
    previous.insert("retryUnresolvedForms", false);
    check(!analysis.segmentSubword(599, 13, 11, 1, nullptr, true, &previous),
          "Missing compact head should leave an explicit partial result");
    const std::vector<Stretching> oldJoins(sub.joins.begin(), sub.joins.end());
    check(oldJoins.size() == 2, "Expected existing lam and jeem boundaries");
    check(analysis.segmentSubword(599, 13, 11, 1, nullptr, true), "Compact head did not unblock yeh and final meem");
    check(sub.baseGlyphs.size() == 5 && sub.kashidaPaths.size() == 4 && sub.kashidaPaths[0].isEmpty() &&
          !sub.kashidaPaths[1].isEmpty() && !sub.kashidaPaths[2].isEmpty() && !sub.kashidaPaths[3].isEmpty(),
          "Expected five bodies, three connectors and a shared lam/jeem boundary");
    check(sub.matchedTemplates.contains("hah-rising-entry-medi") &&
          sub.matchedTemplates.contains("hah-compact-medi") &&
          sub.matchedTemplates.contains("beh-tooth-medi") &&
          sub.matchedTemplates.contains("meem-counter-fina"), "Wrong forms claimed consecutive heads");
    const std::vector<Stretching> joins(sub.joins.begin(), sub.joins.end());
    check(same(oldJoins[0], joins[0]) && same(oldJoins[1], joins[1]),
          "Compact head changed the already matched lam/jeem cuts");
    Stretching assembledEntry{};
    assembledEntry.topBaseLine = joins[1].topLeft;
    assembledEntry.bottomBaseLine = joins[1].bottomLeft;
    check(same(right, assembledEntry) && same(left, joins[2]), "Assembly moved the intrinsic compact head cuts");
    for (const auto& diagnostic : sub.letterDiagnostics)
        check(diagnostic.toObject().value("status") == "matched", "A missing body was reported complete");
    std::vector<QPainterPath> masks;
    for (const auto& body : sub.baseGlyphs) {
        auto path = shape.transform.map(body.path); path.translate(shape.pos); masks.push_back(path);
    }
    for (const auto& connector : sub.kashidaPaths) if (!connector.isEmpty()) {
        auto path = shape.transform.map(connector); path.translate(shape.pos); masks.push_back(path);
    }
    const auto bounds = ink.boundingRect().adjusted(-.1, -.1, .1, .1);
    int samples = 0;
    for (double y = bounds.top() + .013; y < bounds.bottom(); y += .07)
        for (double x = bounds.left() + .017; x < bounds.right(); x += .07) {
            const QPointF point(x, y); int count = 0;
            for (const auto& mask : masks) count += mask.contains(point);
            check(count == int(ink.contains(point)), "Ink was lost, duplicated or fabricated");
            samples += ink.contains(point);
        }
    check(samples > 1000, "Insufficient ink conservation samples");
    const auto bodies = sub.baseGlyphs;
    const auto connectors = sub.kashidaPaths;
    check(analysis.segmentSubword(599, 13, 11, 1, nullptr, true) && sub.kashidaPaths == connectors,
          "Repeated segmentation changed connectors");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(bodies[i].path == sub.baseGlyphs[i].path, "Repeated segmentation changed body masks");
    qInfo("Compact hah passed: intrinsic boundaries, retained head, independent neighbors, scale/translation, bowl rejection, five bodies, three connectors, stable earlier cuts, conserved ink and determinism.");
}
