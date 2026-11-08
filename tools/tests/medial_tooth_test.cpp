// Test the private detector without neighboring templates or join landmarks.
// The runner omits the normal segmentsubword object when linking this file.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

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
    check(context.timeSorted.size() >= 3, "Missing source outline");
    context.setOrientation();
    return context;
}

static bool same(const Stretching& expected, const Stretching& actual, const QTransform& mapping = {}) {
    return QLineF(mapping.map(expected.topBaseLine.point), actual.topBaseLine.point).length() < 1e-4 &&
        QLineF(mapping.map(expected.bottomBaseLine.point), actual.bottomBaseLine.point).length() < 1e-4;
}

static std::vector<std::pair<Stretching, Stretching>> medialToothBoundaries(
        SearchContext& context, const QJsonObject& settings) {
    std::vector<std::pair<Stretching, Stretching>> result;
    for (const auto& tooth : toothBodyBoundaries(context, settings, true)) result.emplace_back(*tooth.right, tooth.left);
    return result;
}

struct ExpectedTooth {
    int body;
    const char* form;
    double tip;
    double bend;
};

static void checkAssignedTeeth(int page, int line, int wordIndex, int subIndex,
                               std::initializer_list<ExpectedTooth> teeth) {
    QFile file(QString("pages/page%1.dat").arg(page));
    check(file.open(QIODevice::ReadOnly), "Cannot open medial-tooth fixture");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    auto& word = analysis.page.lines[line - 1].words[wordIndex - 1];
    auto& sub = word.wordResultInfo.subWords[subIndex - 1];
    check(analysis.segmentSubword(page - 1, line - 1, wordIndex - 1, subIndex - 1, nullptr, true),
          "Medial-tooth fixture did not complete");
    const auto shape = word.bodyShape(sub);
    const auto curves = baseContourCurves(shape.path);
    const auto point = [&](double time) {
        const int index = static_cast<int>(time);
        return curves.at(index).pointAt(time - index);
    };
    for (const auto& tooth : teeth) {
        const size_t i = tooth.body - 1;
        check(sub.letterDiagnostics.at(i).toObject().value("template") == tooth.form,
              "The wrong form claimed a tooth");
        const QLineF chord(point(tooth.tip), point(tooth.bend));
        for (double t : {.05, .5, .95}) {
            const auto sample = chord.pointAt(t);
            for (size_t owner = 0; owner < sub.baseGlyphs.size(); ++owner)
                check(sub.baseGlyphs[owner].path.contains(sample) == (owner == i),
                      "A tip or inner bend is outside its own letter");
            for (const auto& kashida : sub.kashidaPaths)
                check(!kashida.contains(sample), "A connector claimed part of a tooth");
        }
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject settings;
    for (const auto& value : config.value("medial").toArray())
        if (value.toObject().value("id") == "beh-tooth-medi") settings = value.toObject().value("tooth").toObject();
    check(!settings.isEmpty() && !settings.contains("maxExitDistance"), "Tooth still uses the neighboring exit distance");
    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 cache is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page cache");
    auto& word = analysis.page.lines[1].words[3];
    auto& subword = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[subword.paths.front()];
    auto source = shape.transform.map(shape.path);
    source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(source, dummy);
    const auto pairs = medialToothBoundaries(context, settings);
    // The whole outline can contain similar bends in other letters. Select
    // the reviewed tooth for this fixture; the production dispatcher uses
    // reading order to associate independent proposals with letter instances.
    const auto tip = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [](const auto& p) {
        return std::abs(p.time - 5.53152858) < 1e-6;
    });
    const auto bend = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [](const auto& p) {
        return p.time == 17;
    });
    check(tip != context.timeSorted.end() && bend != context.timeSorted.end(), "Missing reviewed tooth landmarks");
    const auto sample = QLineF(tip->point, bend->point).center();
    const auto selected = std::find_if(pairs.begin(), pairs.end(), [&](const auto& pair) {
        const auto pieces = cutRegions(source, {}, {pair.first, pair.second}, context.orientation, true);
        return pieces.size() == 3 && pieces[1].path.contains(sample);
    });
    check(selected != pairs.end(), "No proposal encloses the reviewed tooth");
    const auto index = std::distance(pairs.begin(), selected);
    const auto [right, left] = *selected;
    const auto curves = baseContourCurves(source);
    const auto tangent = [&](const PathExtrema& point) {
        const auto& c = curves.at(static_cast<size_t>(point.time));
        const double t = point.time - std::floor(point.time), u = 1 - t;
        const auto v = u * u * (c.pt2() - c.pt1()) + 2 * u * t * (c.pt3() - c.pt2()) +
            t * t * (c.pt4() - c.pt3());
        return v / std::hypot(v.x(), v.y());
    };
    for (const auto& cut : {right, left}) {
        check(interiorChord(source, cut.topBaseLine, cut.bottomBaseLine), "Tooth cut leaves solid ink");
        auto a = tangent(cut.topBaseLine), b = tangent(cut.bottomBaseLine);
        if (QPointF::dotProduct(a, b) < 0) b = -b;
        const auto direction = cut.bottomBaseLine.point - cut.topBaseLine.point;
        check(std::abs(QPointF::dotProduct(direction, a + b)) <
            1e-4 * std::hypot(direction.x(), direction.y()), "Tooth cut is not normal to the local stroke");
    }
    // Join locations, widths, search cursors and text must not influence the
    // independent proposal. In particular the distant meem is not its scale.
    Stretching unrelated{};
    unrelated.topBaseLine.point = {-100, 200};
    unrelated.bottomBaseLine.point = {-80, 400};
    unrelated.referenceWidth = 1000;
    dummy.joins.insert(unrelated);
    context.baseText = QStringLiteral("اختبار");
    context.pairedStrokeWidth = 1000;
    context.res = unrelated;
    context.nextIter = context.xSorted.end();
    const auto independent = medialToothBoundaries(context, settings);
    check(independent.size() == pairs.size() && same(right, independent[index].first) && same(left, independent[index].second),
          "Neighboring landmarks or transcript changed the tooth proposal");
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy);
        const auto proposed = medialToothBoundaries(scaled, settings);
        check(std::any_of(proposed.begin(), proposed.end(), [&](const auto& pair) {
                  return same(right, pair.first, mapping) && same(left, pair.second, mapping);
              }),
              "Tooth proposal is not scale/translation invariant");
    }
    QPainterPath roundBody;
    roundBody.addEllipse(QRectF(10, 20, 15, 8));
    auto smooth = geometry(roundBody, dummy);
    check(medialToothBoundaries(smooth, settings).empty(), "A smooth body without an inner bend was treated as a tooth");

    const auto verifyCuts = [&] {
        check(subword.matchedTemplates.contains("beh-tooth-medi"), "Integrated tooth was not matched");
        const std::vector<Stretching> joins(subword.joins.begin(), subword.joins.end());
        check(joins.size() == 2, "Expected two joins in kha-ta-meem");
        Stretching incoming{};
        incoming.topBaseLine = joins[0].topLeft;
        incoming.bottomBaseLine = joins[0].bottomLeft;
        check(same(right, incoming) && same(left, joins[1]), "Assembly changed the independent tooth cuts");
    };
    check(analysis.segmentSubword(2, 1, 3, 0, nullptr, true), "Kha-ta-meem is still partial");
    verifyCuts();
    check(subword.baseGlyphs.size() == 3 && subword.kashidaPaths.size() == 2 &&
          !subword.kashidaPaths[0].isEmpty() && !subword.kashidaPaths[1].isEmpty(), "Expected three bodies and two kashidas");
    auto withoutFinal = config;
    withoutFinal.insert("final", QJsonArray{});
    check(!analysis.segmentSubword(2, 1, 3, 0, nullptr, true, &withoutFinal), "Missing final template reported complete");
    verifyCuts();
    for (const auto& location : {std::array{1, 5, 1}, std::array{1, 9, 1}, std::array{2, 2, 2},
             std::array{3, 8, 2}, std::array{4, 5, 3}, std::array{5, 4, 3}, std::array{10, 1, 3},
             std::array{10, 4, 2}, std::array{12, 2, 3}}) {
        analysis.segmentSubword(2, location[0] - 1, location[1] - 1, location[2] - 1, nullptr, true);
        const auto& control = analysis.page.lines[location[0] - 1].words[location[1] - 1]
            .wordResultInfo.subWords[location[2] - 1];
        check(control.matchedTemplates.contains("beh-tooth-medi"), "A previously matched page 3 tooth was lost");
    }
    // The rounded hamza seat in ma-idh has a wider shallow cross-section,
    // but the same outer-tip/inner-bend geometry as the ordinary tooth.
    QFile laterFile("pages/page600.dat");
    check(laterFile.open(QIODevice::ReadOnly), "Page 600 cache is required");
    PageAnalysisResult later;
    QDataStream laterStream(&laterFile); laterStream >> later.page;
    auto& laterWord = later.page.lines[2].words[8];
    auto& laterSub = laterWord.wordResultInfo.subWords[1];
    const auto& laterShape = laterWord.paths[laterSub.paths.front()];
    auto laterInk = laterShape.transform.map(laterShape.path); laterInk.translate(laterShape.pos);
    auto laterContext = geometry(laterInk, dummy);
    const auto broad = toothBodyBoundaries(laterContext, settings, true);
    check(broad.size() == 1 && std::abs(broad.front().tip.time - 30.4397) < 1e-4 &&
          std::abs(broad.front().bend.time - 16.5705) < 1e-4,
          "Rounded hamza seat was missed or the meem/dhal was mistaken for its tooth");
    for (double scale : {.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(laterInk), dummy);
        const auto result = toothBodyBoundaries(scaled, settings, true);
        check(result.size() == 1 && same(*broad.front().right, *result.front().right, mapping) &&
              same(broad.front().left, result.front().left, mapping),
              "Rounded tooth changed under scale/translation");
    }
    check(later.segmentSubword(599, 2, 8, 1, nullptr, true) && laterSub.baseGlyphs.size() == 3 &&
          laterSub.matchedTemplates.contains("meem-init") && laterSub.matchedTemplates.contains("beh-tooth-medi") &&
          laterSub.matchedTemplates.contains("dal-fina"), "Rounded tooth did not unblock the existing final dhal");
    check(laterSub.kashidaPaths.size() == 2 && !laterSub.kashidaPaths[0].isEmpty() &&
          !laterSub.kashidaPaths[1].isEmpty(), "Ma-idh lost a genuine connector");
    auto toothMask = laterShape.transform.map(laterSub.baseGlyphs[1].path); toothMask.translate(laterShape.pos);
    check(toothMask.contains(broad.front().tip.point + .05 * (broad.front().bend.point - broad.front().tip.point)) &&
          toothMask.contains(broad.front().tip.point + .95 * (broad.front().bend.point - broad.front().tip.point)),
          "Rounded tooth tip or inner bend was assigned to connecting ink");
    auto dhalMask = laterShape.transform.map(laterSub.baseGlyphs[2].path); dhalMask.translate(laterShape.pos);
    check(std::abs(dhalMask.boundingRect().left() - laterInk.boundingRect().left()) < 1e-6 &&
          std::abs(dhalMask.boundingRect().top() - laterInk.boundingRect().top()) < 1e-6,
          "Final dhal lost its ascender or left tail");
    // Sad's own last tooth stays with sad, while genuinely shared entries
    // beside feh and hah may trim the preceding letter's provisional margin.
    checkAssignedTeeth(24, 4, 3, 2, {{1, "sad-init", 30.563026, 20},
                                     {2, "beh-upright-medi", 33.450564, 18},
                                     {3, "beh-tooth-medi", 38.640274, 17}});
    checkAssignedTeeth(259, 11, 8, 1, {{2, "beh-tooth-medi", 6.38515, 24.52536}});
    checkAssignedTeeth(394, 13, 3, 3, {{2, "beh-tooth-medi", 20.99892, 12.81770}});
    checkAssignedTeeth(30, 10, 4, 2, {{2, "beh-tooth-medi", 37.387855, 23.527899},
                                      {3, "beh-upright-medi", 40.104188, 21}});
    checkAssignedTeeth(419, 2, 2, 2, {{2, "beh-upright-medi", 4.4929052, 47}});
    checkAssignedTeeth(57, 14, 2, 2, {{2, "beh-low-medi", 17.5212992, 6.4125923}});
    checkAssignedTeeth(29, 6, 9, 3, {{2, "beh-tooth-medi", 6.4328760, 18}});
    checkAssignedTeeth(6, 5, 5, 1, {{2, "beh-tooth-medi", 7.7838350, 24},
                                     {3, "beh-tooth-medi", 11.5914868, 22}});
    checkAssignedTeeth(116, 11, 1, 1, {{2, "beh-tooth-medi", 33.4365557, 22.4780002},
                                       {3, "beh-upright-medi", 37.1063435, 20},
                                       {4, "beh-tooth-medi", 41.6103758, 19},
                                       {5, "beh-tooth-medi", 45.3686336, 17.5371011}});
    // Exact native landmarks distinguish the tooth from the neighboring terminal,
    // including when an earlier, geometrically valid initial cut takes too much ink.
    checkAssignedTeeth(33, 14, 4, 1, {{2, "beh-bent-medi", 14.5638, 7}});
    // On this rendering the crown and the inclined shaft both belong to yeh.
    // The second section crosses the shaft below the old crown-only cut.
    checkAssignedTeeth(54, 1, 8, 1, {{2, "beh-tooth-medi", 25.18, 11},
                                      {2, "beh-tooth-medi", 23, 11.5}});
    checkAssignedTeeth(157, 10, 4, 2, {{3, "beh-shoulder-medi", 27, 9.02764}});
    checkAssignedTeeth(174, 6, 6, 2, {{2, "beh-hook-medi", 23.3074, 10},
                                      {3, "beh-tooth-medi", 30.3424, 6.51222}});
    checkAssignedTeeth(220, 12, 1, 1, {{3, "beh-tooth-medi", 32.4267, 15.5179}});
    checkAssignedTeeth(235, 12, 7, 1, {{3, "beh-tooth-medi", 9, 43}});
    qInfo("Medial tooth passed: own contour/scale, stroke normals, independent text/joins, affine invariance, smooth-body abstention, two kashidas, no final template, nine prior matches, and rounded hamza-seat/dhal ownership.");
}
