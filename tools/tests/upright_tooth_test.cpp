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
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject upright, ordinary;
    int uprightIndex = -1;
    const auto rules = config.value("medial").toArray();
    for (int i = 0; i < rules.size(); ++i) {
        const auto rule = rules[i].toObject();
        if (rule.value("id") == "beh-upright-medi") {upright = rule.value("tooth").toObject(); uprightIndex = i;}
        if (rule.value("id") == "beh-tooth-medi") ordinary = rule;
    }
    check(uprightIndex >= 0 && !ordinary.isEmpty(), "Missing tooth templates");
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto invalid = config; auto forms = rules; auto rule = forms[uprightIndex].toObject();
        auto tooth = upright;
        if (mutation == 0) tooth.insert("minPairDistance", -1);
        if (mutation == 1) tooth.insert("maxPairDistance", tooth.value("minPairDistance"));
        if (mutation == 2) {auto entry = tooth.value("baselineEntry").toObject(); entry.insert("required", "yes"); tooth.insert("baselineEntry", entry);}
        if (mutation == 3) {auto entry = tooth.value("baselineEntry").toObject(); entry.insert("turnAngle", 90); tooth.insert("baselineEntry", entry);}
        rule.insert("tooth", tooth); forms[uprightIndex] = rule; invalid.insert("medial", forms);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Malformed upright constraints accepted");
    }
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[7].words[9];
    auto& sub = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[sub.paths.front()];
    auto ink = shape.transform.map(shape.path); ink.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(ink, dummy);
    const auto proposals = toothBodyBoundaries(context, upright, true);
    check(proposals.size() == 1, "Upright variant accepted an ordinary sheen tooth");
    const auto& tooth = proposals.front();
    check(std::abs(tooth.tip.time - 36.5519) < 1e-4 && tooth.bend.time == 20,
          "Upright did not use its own tip and lower inner bend");
    auto footSettings = upright; auto footEntry = footSettings.value("baselineEntry").toObject();
    footEntry.remove("turnAngle"); footSettings.insert("baselineEntry", footEntry);
    const auto atFoot = toothBodyBoundaries(context, footSettings, true);
    check(atFoot.size() == 1 && same(tooth.left, atFoot[0].left) &&
          tooth.right->topBaseLine.point.y() < atFoot[0].right->topBaseLine.point.y() &&
          tooth.right->topBaseLine.point.x() < atFoot[0].right->topBaseLine.point.x(),
          "Right cut did not move up to the turn while preserving the left cut");
    const auto curves = baseContourCurves(ink);
    const auto& anchor = tooth.right->topBaseLine;
    const int index = int(std::floor(anchor.time));
    const double t = anchor.time - index, u = 1 - t;
    const auto& curve = curves[index];
    const auto tangent = u*u*(curve.pt2()-curve.pt1()) + 2*u*t*(curve.pt3()-curve.pt2()) + t*t*(curve.pt4()-curve.pt3());
    const auto normal = tooth.right->bottomBaseLine.point - anchor.point;
    check(tangent.x()*tangent.y() > 0 && std::abs(std::abs(tangent.x())-std::abs(tangent.y())) < 1e-6 &&
          std::abs(QPointF::dotProduct(tangent, normal)) < 1e-6,
          "Upright entry is not normal to its own 45-degree descending turn");
    const auto old = declaredToothBoundaries(context, ordinary);
    check(std::none_of(old.begin(), old.end(), [&](const auto& proposal) {return proposal.tip.time == tooth.tip.time;}),
          "Fixture does not exercise the ordinary pair-distance cutoff");
    auto noFoot = upright; auto entry = noFoot.value("baselineEntry").toObject();
    entry.insert("minRise", 1000); noFoot.insert("baselineEntry", entry);
    check(toothBodyBoundaries(context, noFoot, true).empty(), "Required baseline entry was ignored");
    context.baseText = QStringLiteral("بدون سياق"); context.pairedStrokeWidth = 1000;
    context.nextIter = context.xSorted.end();
    Stretching unrelated{}; unrelated.topBaseLine.point = {-100, 100}; unrelated.bottomBaseLine.point = {200, -50};
    dummy.joins.insert(unrelated);
    const auto independent = toothBodyBoundaries(context, upright, true);
    check(independent.size() == 1 && same(*tooth.right, *independent[0].right) && same(tooth.left, independent[0].left),
          "Neighboring letter state changed the upright tooth");
    for (double scale : {.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(ink), dummy);
        const auto result = toothBodyBoundaries(scaled, upright, true);
        check(result.size() == 1 && same(*tooth.right, *result[0].right, mapping) && same(tooth.left, result[0].left, mapping),
              "Upright tooth changed under scale/translation");
    }
    check(analysis.segmentSubword(599, 7, 9, 0, nullptr, true), "Upright did not unblock sheen and final teh marbuta");
    check(sub.baseGlyphs.size() == 4 && sub.matchedTemplates.contains("beh-upright-medi") &&
          sub.matchedTemplates.contains("seen-medi") && sub.matchedTemplates.contains("heh-counter-fina"),
          "The four bodies were not matched to their own forms");
    std::vector<QPainterPath> masks;
    for (const auto& body : sub.baseGlyphs) {
        auto p = shape.transform.map(body.path); p.translate(shape.pos); masks.push_back(p);
    }
    check(masks[1].contains(tooth.tip.point + .05 * (tooth.bend.point - tooth.tip.point)) &&
          masks[1].contains(tooth.tip.point + .95 * (tooth.bend.point - tooth.tip.point)),
          "Yeh's upright tip or inner bend was left in kashida");
    for (double time : {43.5415, 48.5296, 52.547}) {
        const auto tip = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [&](const auto& p) {
            return std::abs(p.time - time) < 1e-4;
        });
        check(tip != context.timeSorted.end() && masks[2].contains(tip->point + QPointF(0, .03)) &&
              !masks[1].contains(tip->point + QPointF(0, .03)), "Sheen lost a tooth to yeh");
    }
    for (const auto& kashida : sub.kashidaPaths) if (!kashida.isEmpty()) {
        auto p = shape.transform.map(kashida); p.translate(shape.pos); masks.push_back(p);
    }
    const auto bounds = ink.boundingRect().adjusted(-.1, -.1, .1, .1);
    QPainterPathStroker outline;
    // Splitting the same Bezier can change Qt's containment rounding at its
    // edge. Allow only sub-pixel differences on the original outline, never
    // missing interior ink or overlaps across a cut.
    outline.setWidth(.001);
    outline.setCurveThreshold(1e-5);
    const auto boundaryBand = outline.createStroke(ink);
    int samples = 0, errors = 0;
    for (double y = bounds.top() + .013; y < bounds.bottom(); y += .07)
        for (double x = bounds.left() + .017; x < bounds.right(); x += .07) {
            const QPointF p(x, y); int count = 0;
            for (const auto& mask : masks) count += mask.contains(p);
            if (count != int(ink.contains(p))) {
                check(count <= 1 && boundaryBand.contains(p), "Partition lost, duplicated or fabricated interior ink");
                ++errors;
            }
            samples += ink.contains(p);
        }
    check(errors <= std::max(3, samples / 1000), "Contour-edge discrepancies exceed numerical tolerance");
    check(samples > 1000, "Ink check too sparse");
    const auto bodies = sub.baseGlyphs; const auto kashidas = sub.kashidaPaths;
    check(analysis.segmentSubword(599, 7, 9, 0, nullptr, true) && sub.kashidaPaths == kashidas,
          "Repeated segmentation changed connectors");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(sub.baseGlyphs[i].path == bodies[i].path, "Repeated segmentation changed body masks");
    // The same shape before seen occurs with tatweel/hamza notation. Skipping
    // the spacer body slot lets the existing lam/noon forms finish the word.
    analysis.segmentSubword(599, 14, 6, 0, nullptr, true);
    const auto& second = analysis.page.lines[14].words[6].wordResultInfo.subWords[0];
    check(second.matchedTemplates.contains("beh-upright-medi") && second.matchedTemplates.contains("seen-medi") &&
          second.matchedTemplates.contains("lam-medi") && second.matchedTemplates.contains("noon-corner-fina") &&
          second.correct && second.baseGlyphs.size() == 5,
          "Upright-before-seen control did not retain its tooth and complete after the spacer");
    qInfo("Upright tooth passed: schema, own tip/bend and 45-degree turn normal, unchanged left cut, ordinary-tooth rejection, neighbor independence, scale/translation, all three sheen teeth retained, two seen/sheen contexts, ink conservation and determinism.");
}
