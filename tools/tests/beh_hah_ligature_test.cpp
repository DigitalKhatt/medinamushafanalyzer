// Private geometry checks run without the surrounding letter templates.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool passed, const QString& message) {
    if (!passed) qFatal("%s", qPrintable(message));
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
    context.setOrientation();
    return context;
}

static bool same(const Stretching& first, const Stretching& second, const QTransform& mapping = {}) {
    return QLineF(mapping.map(first.topBaseLine.point), second.topBaseLine.point).length() < 1e-4 &&
        QLineF(mapping.map(first.bottomBaseLine.point), second.bottomBaseLine.point).length() < 1e-4;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject hahRule, noonRule;
    for (const auto& value : config.value("medial").toArray())
        if (value.toObject().value("id") == "hah-stacked-medi") hahRule = value.toObject();
    for (const auto& value : config.value("final").toArray())
        if (value.toObject().value("id") == "noon-corner-fina") noonRule = value.toObject();
    check(hahRule.value("sharedEntry").toBool(), "Stacked hah must have a shared ligature entry");
    check(noonRule.value("strokeOffsetX").toDouble() == .5, "Noon offset must be half a stroke width");
    struct StackedWord {
        int page, line, word;
        double cornerTime, oppositeTime;
        QPointF risingStroke;
    };
    for (const auto& example : {StackedWord{315,9,8,40,2.61523,{126.3,299.8}},
                                StackedWord{131,10,5,40,1.75399,{167.7,326.2}},
                                StackedWord{479,13,10,41,1.75404,{100.8,407.25}}}) {
        QFile source(QString("pages/page%1.dat").arg(example.page));
        check(source.open(QIODevice::ReadOnly), "Missing stacked yeh–hah fixture");
        QDataStream input(&source);
        PageAnalysisResult sample;
        input >> sample.page;
        auto& word = sample.page.lines[example.line-1].words[example.word-1];
        const auto& sub = word.wordResultInfo.subWords[0];
        const auto segment = [&](const QJsonObject* forms = nullptr) {
            return sample.segmentSubword(example.page-1,example.line-1,example.word-1,0,nullptr,true,forms);
        };
        auto capOnly = config;
        QJsonArray capRules;
        for (const auto& value : capOnly.value("initial").toArray()) {
            const auto id = value.toObject().value("id").toString();
            // Isolate the old cap-only hypothesis from independent sloping
            // and hook forms which can already keep the upper attachment.
            if (id != "beh-sloping-init" && id != "beh-hook-init") capRules.append(value);
        }
        capOnly.insert("initial", capRules);
        auto rules = capOnly.value("medial").toArray();
        for (int i = 0; i < rules.size(); ++i) {
            auto rule = rules[i].toObject();
            rule.remove("upperAttachment");
            rules[i] = rule;
        }
        capOnly.insert("medial", rules);
        check(segment(&capOnly) && sub.baseGlyphs.size() == 4, "Expected the original cap cut");
        const auto before = sub;
        const auto shape = word.bodyShape(sub);
        const auto pagePath = [&](const QPainterPath& p) { return shape.transform.map(p).translated(shape.pos); };
        check(!pagePath(before.baseGlyphs[0].path).contains(example.risingStroke) &&
              pagePath(before.baseGlyphs[1].path).contains(example.risingStroke),
              "The original cut must put the rising stroke in the hah-family body");
        check(segment() && sub.baseGlyphs.size() == 4 &&
              (sub.letterDiagnostics[1].toObject().value("entryTemplate") == "hah-stacked-medi" ||
               sub.letterDiagnostics[1].toObject().value("template") == "hah-stacked-medi"),
              "The upper attachment should be identified by the stacked hah template");
        const auto& entry = *sub.joins.begin();
        check(entry.bottomBaseLine.time == example.cornerTime && std::abs(entry.topBaseLine.time - example.oppositeTime) < .0001 &&
              std::abs(entry.topBaseLine.point.y() - entry.bottomBaseLine.point.y()) < 1e-7 &&
              entry.topBaseLine.point.x() < entry.bottomBaseLine.point.x() && sub.kashidaPaths[0].isEmpty(),
              "The shared cut must extend horizontally left from the attachment corner");
        const auto yeh = pagePath(sub.baseGlyphs[0].path);
        check(std::abs(yeh.boundingRect().bottom() - entry.bottomBaseLine.point.y()) < 1e-6 &&
              yeh.contains(example.risingStroke), "The rising stroke above the attachment must stay in the yeh body");
        check(sub.baseGlyphs[2].path == before.baseGlyphs[2].path && sub.baseGlyphs[3].path == before.baseGlyphs[3].path &&
              sub.kashidaPaths[1] == before.kashidaPaths[1] && sub.kashidaCuts[1] == before.kashidaCuts[1] &&
              sub.kashidaPaths[2] == before.kashidaPaths[2] && sub.kashidaCuts[2] == before.kashidaCuts[2],
              "Changing the yeh–hah cut altered other boundaries");
    }
    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 cache is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page cache");

    for (const auto& location : {std::array{13, 3, 1, 24}, std::array{5, 1, 1, 20}, std::array{5, 6, 1, 21}}) {
        const int li = location[0] - 1, wi = location[1] - 1, si = location[2] - 1;
        auto& word = analysis.page.lines[li].words[wi];
        auto& sub = word.wordResultInfo.subWords[si];
        const auto& shape = word.paths[sub.paths.front()];
        auto source = shape.transform.map(shape.path);
        source.translate(shape.pos);
        WordResultInfo::SubWordInfo dummy;
        auto context = geometry(source, dummy);
        const auto pairs = contourBodyBoundaries(context, hahRule.value("contourBody").toObject());
        check(pairs.size() == 1, "Expected one stacked-hah body");
        const auto& cut = pairs.front().first;
        check(cut.bottomBaseLine.time == location[3] && cut.bottomBaseLine.isSharp &&
              cut.topBaseLine.point.x() < cut.bottomBaseLine.point.x() &&
              std::abs(cut.topBaseLine.point.y() - cut.bottomBaseLine.point.y()) < 1e-7,
              "Ligature cut did not extend left from its inward corner");

        Stretching unrelated{};
        unrelated.topBaseLine.point = {-10, -100};
        unrelated.bottomBaseLine.point = {500, 200};
        dummy.joins.insert(unrelated);
        context.pairedStrokeWidth = 1000;
        context.baseText = QStringLiteral("اختبار");
        const auto independent = contourBodyBoundaries(context, hahRule.value("contourBody").toObject());
        check(independent.size() == 1 && same(cut, independent.front().first),
              "Neighboring landmarks changed the ligature cut");
        for (double scale : {.7, 1.6}) {
            const QTransform mapping(scale, 0, 0, scale, 37, -21);
            auto scaled = geometry(mapping.map(source), dummy);
            const auto result = contourBodyBoundaries(scaled, hahRule.value("contourBody").toObject());
            check(result.size() == 1 && same(cut, result.front().first, mapping),
                  "Ligature cut is not scale/translation invariant");
        }
        check(analysis.segmentSubword(2, li, wi, si, nullptr, true), "Ligature became partial");
        const auto& shared = *sub.joins.begin();
        check(sub.baseGlyphs.size() == 3 && sub.kashidaPaths.size() == 2 &&
              sub.kashidaPaths[0].isEmpty() && !sub.kashidaPaths[1].isEmpty() && same(cut, shared) &&
              shared.topRight.point == shared.topBaseLine.point && shared.bottomRight.point == shared.bottomBaseLine.point,
              "The ligature retained a kashida or stale displayed boundary");

        if (location[0] != 13) continue;
        check(sub.matchedTemplates.contains("noon-corner-fina"), "Noon corner form was not used");
        auto noonContext = geometry(source, dummy);
        noonContext.form = noonRule;
        noonContext.form.insert("normalCut", false);
        check(searchContourWalk(noonContext), "Noon corner projection was not found");
        const auto unshifted = noonContext.res;
        check(unshifted.bottomBaseLine.time == 18, "Noon did not find its own corner 18");
        noonContext.form = noonRule;
        noonContext.form.insert("strokeOffsetX", 0);
        noonContext.res = {};
        check(searchContourWalk(noonContext), "The noon neck has no measurable normal width");
        const double width = QLineF(noonContext.res.topBaseLine.point, noonContext.res.bottomBaseLine.point).length();
        noonContext.form = noonRule;
        noonContext.res = {};
        check(searchContourWalk(noonContext), "Shifted noon cut was not found");
        const auto shifted = noonContext.res;
        const double dx = QLineF(shifted.topBaseLine.point, shifted.bottomBaseLine.point).center().x() -
            unshifted.bottomBaseLine.point.x();
        // Rotating the shifted chord can move its midpoint slightly relative
        // to the vertical probe, because the two contour sides are curved.
        check(dx > .45 * width && dx < .55 * width &&
              shifted.topBaseLine.point.x() > unshifted.bottomBaseLine.point.x() &&
              shifted.bottomBaseLine.point.x() > unshifted.bottomBaseLine.point.x(),
              "Noon boundary is not half a local stroke to the right of its corner");
        const auto& last = *sub.joins.rbegin();
        check(QLineF(last.topLeft.point, shifted.topBaseLine.point).length() < 1e-4 &&
              QLineF(last.bottomLeft.point, shifted.bottomBaseLine.point).length() < 1e-4,
              "The dispatcher replaced the independent noon cut");
        const auto noon = shape.transform.map(sub.baseGlyphs.back().path).boundingRect();
        const auto ink = shape.transform.map(shape.path).boundingRect();
        check(std::abs(noon.left() - ink.left()) < 1e-6 && std::abs(noon.bottom() - ink.bottom()) < 1e-6,
              "Noon lost part of its bowl");
        qInfo() << "Noon corner shift:" << dx << "page units; local normal stroke:" << width;
    }
    qInfo("Beh-hah ligature passed: shared cuts at corners 24/20/21, no intervening kashida, independent geometry, scale/translation invariance, half-stroke noon offset and full bowl ownership.");
}
