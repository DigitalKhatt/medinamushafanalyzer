// Medial feh/qaf must retain their own loop rather than clip a neighbor's stem.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static SearchContext geometry(QPainterPath path, WordResultInfo::SubWordInfo& dummy) {
    path.setFillRule(Qt::WindingFill);
    SearchContext context{.transformedPath = path, .subword = dummy};
    context.bbox = path.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(path, context.subPaths, extrema);
    for (const auto& point : extrema) if (point.pathIndex == 0) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    check(context.timeSorted.size() >= 3, "Missing head outline");
    context.setOrientation();
    return context;
}

static QImage mask(const QPainterPath& path, const QRectF& box) {
    constexpr double scale = 16;
    QImage image(int(std::ceil(box.width() * scale)) + 4,
                 int(std::ceil(box.height() * scale)) + 4, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.translate(2, 2);
    painter.scale(scale, scale);
    painter.translate(-box.topLeft());
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(path);
    return image;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    QJsonObject settings;
    for (const auto& value : PageAnalysisResult::letterForms().value("medial").toArray())
        if (value.toObject().value("id") == "feh-head-medi") settings = value.toObject().value("headEdges").toObject();
    check(!settings.isEmpty() && !settings.contains("maxExitDistance"), "Head still uses its neighboring exit cut");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 fixture is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page 600 fixture");
    auto& word = analysis.page.lines[1].words[9];
    auto& sub = word.wordResultInfo.subWords[1];
    const auto& shape = word.paths[sub.paths.front()];
    auto source = shape.transform.map(shape.path);
    source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(source, dummy);
    const auto boundaries = medialLoopHeadBoundaries(context, settings);
    check(boundaries.size() == 1, "Qaf head was missed or the lam/waw was also accepted as this head");
    const auto& [right, left] = boundaries.front();
    const auto landmark = [&](double time) -> PathExtrema {
        for (const auto& point : context.timeSorted) if (std::abs(point.time - time) < 1e-5) return point;
        qFatal("Missing qaf landmark");
    };
    const auto crest = landmark(10.09158564), r = landmark(8.56887251), l = landmark(11.77798415);
    check(std::abs(right.topBaseLine.point.x() - r.point.x()) < 1e-5 &&
          std::abs(left.topBaseLine.point.x() - l.point.x()) < 1e-5,
          "Cuts are not aligned with the qaf's own side edges");
    const auto pieces = cutRegions(source, {}, {right, left}, context.orientation, true);
    check(pieces.size() == 3 && toSubpaths(pieces[1].path).size() == 2 &&
          pieces[1].path.contains(crest.point + QPointF(0, .05)) &&
          !pieces[1].path.contains(landmark(35.89517485).point + QPointF(0, .05)),
          "Qaf body lost its crest/counter or contains the lam tip");
    const auto same = [](const Stretching& expected, const Stretching& actual, const QTransform& mapping = {}) {
        return QLineF(mapping.map(expected.topBaseLine.point), actual.topBaseLine.point).length() < 1e-4 &&
            QLineF(mapping.map(expected.bottomBaseLine.point), actual.bottomBaseLine.point).length() < 1e-4;
    };
    Stretching unrelated{};
    unrelated.topBaseLine.point = {-100, 200}; unrelated.bottomBaseLine.point = {-80, 400};
    dummy.joins.insert(unrelated);
    context.baseText = QStringLiteral("اختبار");
    context.pairedStrokeWidth = 1000;
    const auto independent = medialLoopHeadBoundaries(context, settings);
    check(independent.size() == 1 && same(right, independent[0].first) && same(left, independent[0].second),
          "Head cuts depend on neighboring joins, text, or stroke width");
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy);
        const auto transformed = medialLoopHeadBoundaries(scaled, settings);
        check(transformed.size() == 1 && same(right, transformed[0].first, mapping) && same(left, transformed[0].second, mapping),
              "Head proportions are not scale/translation invariant");
    }
    auto noCounters = geometry(context.subPaths.front(), dummy);
    check(medialLoopHeadBoundaries(noCounters, settings).empty(), "Head without a counter accepted a stem strip");
    auto noQafCounterPath = context.subPaths.front();
    noQafCounterPath.addPath(context.subPaths[2]); // Keep the final waw's counter.
    auto noQafCounter = geometry(noQafCounterPath, dummy);
    check(medialLoopHeadBoundaries(noQafCounter, settings).empty(), "Missing qaf counter was replaced by the waw's counter");

    check(analysis.segmentSubword(599, 1, 9, 1, nullptr, true), "Lam-qaf-beh-waw became partial");
    check(sub.baseGlyphs.size() == 4 && sub.matchedTemplates.contains("feh-head-medi"), "Missing qaf body");
    auto qaf = shape.transform.map(sub.baseGlyphs[1].path); qaf.translate(shape.pos);
    check(mask(qaf, source.boundingRect()) == mask(pieces[1].path, source.boundingRect()),
          "Dispatcher changed the independently detected qaf body");
    check(toSubpaths(sub.baseGlyphs[1].path).size() == 2 && toSubpaths(sub.baseGlyphs[3].path).size() == 2,
          "Qaf or waw lost its counter");
    auto local = shape.path; local.setFillRule(Qt::WindingFill);
    const auto box = local.boundingRect();
    const auto original = mask(local, box);
    QVector<QImage> parts;
    for (const auto& body : sub.baseGlyphs) parts.append(mask(body.path, box));
    for (const auto& join : sub.kashidaPaths) if (!join.isEmpty()) parts.append(mask(join, box));
    int ink = 0, missing = 0, extra = 0, overlap = 0;
    for (int y = 0; y < original.height(); ++y) for (int x = 0; x < original.width(); ++x) {
        const bool present = qRed(original.pixel(x, y)) != 0;
        int count = 0;
        for (const auto& part : parts) count += qRed(part.pixel(x, y)) != 0;
        ink += present; missing += present && count == 0; extra += !present && count != 0; overlap += count > 1;
    }
    const int tolerance = std::max(8, ink / 500);
    check(missing <= tolerance && extra <= tolerance && overlap <= tolerance,
          "Head and kashida cuts lost or duplicated ink");
    const auto bodies = sub.baseGlyphs;
    const auto kashidas = sub.kashidaPaths;
    check(analysis.segmentSubword(599, 1, 9, 1, nullptr, true) && sub.kashidaPaths == kashidas,
          "Repeat segmentation changed the kashidas");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(sub.baseGlyphs[i].path == bodies[i].path, "Repeat segmentation changed a body mask");

    // Keep the originally reviewed feh edges in ka-fa-ra.
    QFile controlFile("pages/page3.dat");
    check(controlFile.open(QIODevice::ReadOnly), "Page 3 fixture is required");
    PageAnalysisResult control;
    QDataStream controlStream(&controlFile); controlStream >> control.page;
    auto& controlWord = control.page.lines[0].words[2];
    auto& controlSub = controlWord.wordResultInfo.subWords[0];
    const auto& controlShape = controlWord.paths[controlSub.paths.front()];
    auto controlInk = controlShape.transform.map(controlShape.path); controlInk.translate(controlShape.pos);
    auto controlContext = geometry(controlInk, dummy);
    const auto controlHeads = medialLoopHeadBoundaries(controlContext, settings);
    check(controlHeads.size() == 1 && std::abs(controlHeads[0].first.topBaseLine.point.x() - 231.739293484) < 1e-5 &&
          std::abs(controlHeads[0].second.topBaseLine.point.x() - 228.672162506) < 1e-5,
          "Reviewed feh edges 12.58/15.39 moved");
    // A tall teardrop counter can extend below both side extrema. It only
    // needs to overlap the head's crest-to-edge band, not fit its center in it.
    QFile tallFile("pages/page286.dat");
    check(tallFile.open(QIODevice::ReadOnly), "Page 286 tall-head fixture is required");
    PageAnalysisResult tall;
    QDataStream tallStream(&tallFile); tallStream >> tall.page;
    for (const auto& location : {std::array{4, 10}, std::array{5, 7}, std::array{8, 7}}) {
        auto& w = tall.page.lines[location[0] - 1].words[location[1] - 1];
        auto& s = w.wordResultInfo.subWords[0];
        const auto& sh = w.paths[s.paths.front()];
        auto ink = sh.transform.map(sh.path); ink.translate(sh.pos);
        auto c = geometry(ink, dummy);
        const auto heads = medialLoopHeadBoundaries(c, settings);
        check(heads.size() == 1, "Tall feh/qaf head was rejected");
        check(tall.segmentSubword(285, location[0] - 1, location[1] - 1, 0, nullptr, true) &&
              s.matchedTemplates.contains("feh-head-medi") && toSubpaths(s.baseGlyphs[1].path).size() == 2,
              "Tall head fell back or lost its counter");
    }
    qInfo("Medial heads passed: own counter/crest/edges, intermediate corner traversal, lam/waw rejection, neighboring-cut independence, scale/translation invariance, missing-counter abstention, ink conservation, deterministic masks, reviewed feh edges and three tall heads on page 286.");
}
