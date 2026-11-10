#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static SearchContext geometry(QPainterPath ink, WordResultInfo::SubWordInfo& dummy, const QJsonObject& form) {
    ink.setFillRule(Qt::WindingFill);
    SearchContext c{.form = form, .transformedPath = ink, .subword = dummy};
    c.bbox = ink.boundingRect();
    QVector<PathExtrema> points;
    ShapeItem::getExtrema(ink, c.subPaths, points);
    for (const auto& p : points) if (!p.pathIndex) {
        c.timeSorted.insert(p); c.xSorted.insert(p); c.ySorted.insert(p);
    }
    c.setOrientation(); c.nextIter = std::next(c.xSorted.begin());
    return c;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject form;
    for (const auto& value : config.value("initial").toArray())
        if (value.toObject().value("id") == "hah-counter-init") form = value.toObject();
    check(!form.isEmpty() && form.value("body").toObject().value("shape") == "contour-route" && form.value("body").toObject().value("counters").toInt() == 1,
          "Missing closed-head contour template");
    QFile file("pages/page600.dat"); check(file.open(QIODevice::ReadOnly), "Missing page 600");
    PageAnalysisResult a; QDataStream in(&file); in >> a.page;
    auto& word = a.page.lines[9].words[6]; auto& sub = word.wordResultInfo.subWords[0];
    auto& shape = word.paths[sub.paths[0]];
    auto source = shape.transform.map(shape.path); source.translate(shape.pos); source.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy;
    auto c = geometry(source, dummy, form);
    check(declaredSharedBoundary(c,form), "Closed hah did not find its corner");
    const auto cut = c.res;
    check(cut.topBaseLine.time == 15 && std::abs(cut.bottomBaseLine.time - 5.10839) < 1e-5 &&
          std::abs(cut.topBaseLine.point.x() - cut.bottomBaseLine.point.x()) < 1e-6,
          "Closed hah cut missed its own sharp corner or lower contour");
    const auto pieces = cutRegions(source, {}, {cut}, c.orientation, true);
    check(pieces.size() == 2 && toSubpaths(pieces[0].path).size() == 2 &&
          toSubpaths(pieces[1].path).size() == 1 &&
          pieces[0].path.boundingRect().top() > source.boundingRect().top(),
          "Closed head lost its counter or swallowed the ascender");
    c.baseText = QStringLiteral("unrelated"); c.pairedStrokeWidth = 1000;
    Stretching unrelated{}; unrelated.topBaseLine.point = {-200, 300}; unrelated.bottomBaseLine.point = {-100, 500};
    dummy.joins.insert(unrelated);
    check(declaredSharedBoundary(c,form) && c.res.topBaseLine.point == cut.topBaseLine.point &&
          c.res.bottomBaseLine.point == cut.bottomBaseLine.point, "Hah geometry uses neighboring text or joins");
    for (double scale : {.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy, form);
        check(declaredSharedBoundary(scaled,form) &&
              QLineF(mapping.map(cut.topBaseLine.point), scaled.res.topBaseLine.point).length() < 1e-5 &&
              QLineF(mapping.map(cut.bottomBaseLine.point), scaled.res.bottomBaseLine.point).length() < 1e-5,
              "Closed-head cut changed under scale/translation");
    }
    check(a.segmentSubword(599, 9, 6, 0, nullptr, true) && sub.baseGlyphs.size() == 2 &&
          sub.matchedTemplates.contains("hah-counter-init") && sub.matchedTemplates.contains("alef-fina"),
          "Hah-alef did not match both bodies");
    std::vector<QPainterPath> masks;
    for (const auto& body : sub.baseGlyphs) masks.push_back(body.path);
    int connectors = 0;
    for (const auto& path : sub.kashidaPaths) if (!path.isEmpty()) {masks.push_back(path); ++connectors;}
    check(connectors == 1, "Independent body cuts lost the connecting ink");
    auto local = shape.path; local.setFillRule(Qt::WindingFill);
    QPainterPathStroker outline; outline.setWidth(.001); outline.setCurveThreshold(1e-5);
    const auto edge = outline.createStroke(local);
    const auto box = local.boundingRect().adjusted(-.1, -.1, .1, .1);
    int samples = 0, errors = 0;
    for (double y = box.top() + .013; y < box.bottom(); y += .07)
        for (double x = box.left() + .017; x < box.right(); x += .07) {
            const QPointF p(x, y); int count = 0;
            for (const auto& mask : masks) count += mask.contains(p);
            if (count != int(local.contains(p))) {
                check(count <= 1 && edge.contains(p), "Closed-head partition loses or duplicates interior ink");
                ++errors;
            }
            samples += local.contains(p);
        }
    check(samples > 1000 && errors <= std::max(3, samples / 1000), "Ink partition exceeds contour-rounding tolerance");
    const auto bodies = sub.baseGlyphs; const auto kashidas = sub.kashidaPaths;
    check(a.segmentSubword(599, 9, 6, 0, nullptr, true) && sub.kashidaPaths == kashidas &&
          sub.baseGlyphs[0].path == bodies[0].path && sub.baseGlyphs[1].path == bodies[1].path,
          "Closed-head partition is nondeterministic");
    // Missing counters cannot silently become a closed-head match.
    const auto original = shape.path; shape.path = toSubpaths(original).front();
    a.segmentSubword(599, 9, 6, 0, nullptr, true);
    check(!sub.matchedTemplates.contains("hah-counter-init"), "Closed-head template accepted a missing counter");
    shape.path = original;
    // An open hah before a sad must retain the existing open-head boundary.
    check(a.segmentSubword(599, 2, 0, 1, nullptr, true), "Open hah/sad control regressed");
    const auto& open = a.page.lines[2].words[0].wordResultInfo.subWords[1];
    check(open.matchedTemplates.contains("hah-open-init") && !open.matchedTemplates.contains("hah-counter-init"),
          "Closed-head fallback consumed the following sad's counter");
    // The same intrinsic route recognizes a closed kha head before medial lam.
    // Its already-supported active segmentation keeps its existing template.
    QFile controlFile("pages/page3.dat"); check(controlFile.open(QIODevice::ReadOnly), "Missing page 3");
    PageAnalysisResult control; QDataStream controlIn(&controlFile); controlIn >> control.page;
    auto& cw = control.page.lines[11].words[5]; auto& cs = cw.wordResultInfo.subWords[0];
    const auto& cp = cw.paths[cs.paths[0]];
    auto controlInk = cp.transform.map(cp.path); controlInk.translate(cp.pos);
    auto controlGeometry = geometry(controlInk, dummy, form);
    check(declaredSharedBoundary(controlGeometry,form) && controlGeometry.res.topBaseLine.time == 18,
          "Closed kha before lam did not find its own corner");
    const auto controlBodies = cutRegions(controlInk, {}, {controlGeometry.res}, controlGeometry.orientation, true);
    check(controlBodies.size() == 2 && toSubpaths(controlBodies[0].path).size() == 2,
          "Closed kha route did not preserve its counter");
    check(control.segmentSubword(2, 11, 5, 0, nullptr, true) &&
          cs.matchedTemplates.contains("hah-init") && !cs.matchedTemplates.contains("hah-counter-init"),
          "Existing closed-head template precedence changed");
    qInfo("Closed hah passed: own corner/counter before alef and lam, vertical lower intersection, neighboring-state independence, scale/translation, two bodies/one connector, missing-counter rejection, existing-template precedence, ink conservation and determinism.");
}
