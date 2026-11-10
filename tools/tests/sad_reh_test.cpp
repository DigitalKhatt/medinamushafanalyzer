// Exercise the shared loop/walk/tooth composition without neighboring templates.
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
    context.setOrientation();
    context.nextIter = context.xSorted.begin();
    return context;
}

static bool same(const Stretching& expected, const Stretching& actual, const QTransform& mapping = {}) {
    return QLineF(mapping.map(expected.topBaseLine.point), actual.topBaseLine.point).length() < 1e-4 &&
        QLineF(mapping.map(expected.bottomBaseLine.point), actual.bottomBaseLine.point).length() < 1e-4;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject loop, reh;
    for (const auto& value : config.value("medial").toArray())
        if (value.toObject().value("id") == "sad-medi") {
            check(value.toObject().value("current") == QStringLiteral("صض"), "Sad form must also select dad");
            loop = value.toObject().value("loopBoundaries").toObject();
        }
    for (const auto& value : config.value("final").toArray())
        if (value.toObject().value("id") == "reh-fina") reh = value.toObject();
    check(!loop.isEmpty() && reh.value("body").toObject().value("shape") == "contour-route", "Missing reusable body definitions");
    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page cache");
    auto& word = analysis.page.lines[2].words[0];
    auto& sub = word.wordResultInfo.subWords[1];
    const auto& shape = word.paths[sub.paths.front()];
    auto source = shape.transform.map(shape.path); source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(source, dummy);
    const auto body = medialLoopBoundaries(context, loop, context.bbox.left(), context.bbox.right());
    check(bool(body), "Counter and attached tooth did not form a medial body");
    const auto [right, left] = *body;
    check(right.topBaseLine.time == 2 && right.topBaseLine.isSharp &&
          std::abs(right.topBaseLine.point.x() - right.bottomBaseLine.point.x()) < 1e-8,
          "Sad entry moved away from vertical corner 2");
    const auto tip = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [](const auto& p) {
        return std::abs(p.time - 8.5474800831) < 1e-6;
    });
    const auto bend = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [](const auto& p) { return p.time == 19; });
    check(tip != context.timeSorted.end() && bend != context.timeSorted.end(), "Missing reviewed tooth landmarks");
    check(left.topBaseLine.point.x() < tip->point.x() && left.bottomBaseLine.point.x() < bend->point.x(),
          "Sad exit does not retain the complete tooth");
    const auto pieces = cutRegions(source, {}, {right, left}, context.orientation, true);
    check(pieces.size() == 3 && pieces[1].path.contains(QLineF(tip->point, bend->point).center()),
          "Tooth was excluded from the sad body");
    context.form = reh;
    check(declaredSharedBoundary(context, reh), "Reh return route failed");
    const auto rehCut = context.res;
    check(rehCut.bottomBaseLine.time == 17 &&
          std::abs(rehCut.topBaseLine.point.x() - rehCut.bottomBaseLine.point.x()) < 1e-8,
          "Reh entry must project upward from corner 17");

    // Geometry must remain independent of transcript, earlier cuts and scale hints.
    context.baseText = QStringLiteral("اختبار");
    context.pairedStrokeWidth = 1000;
    context.nextIter = context.xSorted.end();
    context.res.topBaseLine.point = {-100, 200};
    context.res.bottomBaseLine.point = {-80, 400};
    dummy.joins.insert(context.res);
    const auto independent = medialLoopBoundaries(context, loop, context.bbox.left(), context.bbox.right());
    check(independent && same(right, independent->first) && same(left, independent->second),
          "Neighboring text or joins changed the loop/tooth body");
    context.form = reh;
    check(declaredSharedBoundary(context, reh) && same(rehCut, context.res), "Reh depends on a neighboring cut");
    for (double scale : {0.7, 1.6}) {
        QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy);
        const auto proposed = medialLoopBoundaries(scaled, loop, scaled.bbox.left(), scaled.bbox.right());
        check(proposed && same(right, proposed->first, mapping) && same(left, proposed->second, mapping),
              "Loop/tooth composition is not scale/translation invariant");
        scaled.form = reh;
        check(declaredSharedBoundary(scaled, reh) && same(rehCut, scaled.res, mapping),
              "Reh return route is not scale/translation invariant");
    }
    auto withoutCounter = geometry(context.subPaths.front(), dummy);
    check(!medialLoopBoundaries(withoutCounter, loop, withoutCounter.bbox.left(), withoutCounter.bbox.right()),
          "A missing counter was accepted as a loop body");

    check(analysis.segmentSubword(2, 2, 0, 1, nullptr, true), "Ba-sad-reh remains partial");
    check(sub.baseGlyphs.size() == 3 && sub.kashidaPaths.size() == 2 &&
          !sub.kashidaPaths[0].isEmpty() && !sub.kashidaPaths[1].isEmpty(), "Expected three bodies and two kashidas");
    const auto& entry = *sub.joins.begin();
    const auto& exit = *std::next(sub.joins.begin());
    check(entry.topLeft.time == 2 && exit.bottomLeft.time == 17 &&
          same(left, exit), "Assembly changed the independently proposed boundaries");
    auto finalBody = shape.transform.map(sub.baseGlyphs.back().path); finalBody.translate(shape.pos);
    const auto box = source.boundingRect();
    int samples = 0;
    for (double x = box.left() + 0.05; x < rehCut.bottomBaseLine.point.x() - 0.05; x += 0.1)
        for (double y = box.top() + 0.05; y < box.bottom(); y += 0.1) {
            const QPointF p(x, y);
            if (!source.contains(p)) continue;
            ++samples;
            check(finalBody.contains(p), "Ink left of corner 17 was not assigned to reh");
        }
    check(samples > 100, "Reh ownership check did not exercise its return and bowl");
    for (const auto& location : {std::array{1, 3, 1}, std::array{4, 6, 3}, std::array{6, 2, 1},
             std::array{6, 6, 1}, std::array{6, 7, 1}, std::array{6, 9, 1}, std::array{9, 5, 1}, std::array{13, 7, 1}})
        check(analysis.segmentSubword(2, location[0] - 1, location[1] - 1, location[2] - 1, nullptr, true),
              "A previously complete reh control regressed");

    // A preceding open hah must not claim the sad's counter. A subdivided
    // sad flank must reach its turning corner, rather than an earlier sharp
    // point on the same flank (page 600: corner 6, not corner 7).
    QJsonObject openHah;
    for (const auto& value : config.value("initial").toArray())
        if (value.toObject().value("id") == "hah-open-init") openHah = value.toObject();
    check(openHah.value("body").toObject().value("counters").toInt(-1) == 0, "Open hah must exclude a neighboring counter");
    QFile page600("pages/page600.dat");
    check(page600.open(QIODevice::ReadOnly), "Page 600 is required");
    PageAnalysisResult other;
    QDataStream otherStream(&page600); otherStream >> other.page;
    for (int wi : {0, 3}) {
        auto& w = other.page.lines[2].words[wi];
        auto& sw = w.wordResultInfo.subWords[1];
        const auto& sh = w.paths[sw.paths.front()];
        auto ink = sh.transform.map(sh.path); ink.translate(sh.pos);
        WordResultInfo::SubWordInfo isolated;
        auto own = geometry(ink, isolated);
        const auto proposal = medialLoopBoundaries(own, loop, own.bbox.left(), own.bbox.right());
        check(bool(proposal), "Page 600 sad's own counter/tooth was not recognized");
        check(proposal->first.topBaseLine.time == (wi == 0 ? 31 : 6), "Sad entry did not reach its own turning corner");
        const auto parts = cutRegions(ink, {}, {proposal->first, proposal->second}, own.orientation, true);
        check(parts.size() == 3 && toSubpaths(parts[1].path).size() == 2,
              "Sad lost its complete counter");
        const double tipTime = wi == 0 ? 39.480237 : 12.510931;
        const double bendTime = wi == 0 ? 11.555429 : 33.511150;
        const auto tipPoint = std::find_if(own.timeSorted.begin(), own.timeSorted.end(), [&](const auto& p) {
            return std::abs(p.time - tipTime) < 1e-5;
        });
        const auto bendPoint = std::find_if(own.timeSorted.begin(), own.timeSorted.end(), [&](const auto& p) {
            return std::abs(p.time - bendTime) < 1e-5;
        });
        check(tipPoint != own.timeSorted.end() && bendPoint != own.timeSorted.end() &&
              parts[1].path.contains(QLineF(tipPoint->point, bendPoint->point).center()),
              "Sad lost its attached tooth");
        own.baseText = QStringLiteral("بدون سياق"); own.pairedStrokeWidth = 1000;
        own.nextIter = own.xSorted.end();
        const auto unchanged = medialLoopBoundaries(own, loop, own.bbox.left(), own.bbox.right());
        check(unchanged && same(proposal->first, unchanged->first) && same(proposal->second, unchanged->second),
              "Page 600 sad proposal depends on neighboring text or joins");
        Stretching hahCut{};
        if (wi == 0) {
            own.form = openHah; own.res = {};
            check(declaredSharedBoundary(own,openHah), "Open hah did not find its own lip");
            hahCut = own.res;
            const auto head = cutRegions(ink, {}, {hahCut}, own.orientation, true);
            check(head.size() == 2 && toSubpaths(head[0].path).size() == 1,
                  "Initial hah swallowed the sad counter");
        }
        for (double scale : {.7, 1.6}) {
            const QTransform mapping(scale, 0, 0, scale, 37, -21);
            auto changed = geometry(mapping.map(ink), isolated);
            const auto pair = medialLoopBoundaries(changed, loop, changed.bbox.left(), changed.bbox.right());
            check(pair && same(proposal->first, pair->first, mapping) && same(proposal->second, pair->second, mapping),
                  "Page 600 sad cuts changed under scale/translation");
            if (wi == 0) {
                changed.form = openHah; changed.res = {};
                check(declaredSharedBoundary(changed,openHah) && same(hahCut, changed.res, mapping),
                      "Open hah cut changed under scale/translation");
            }
        }
        check(other.segmentSubword(599, 2, wi, 1, nullptr, true) && sw.matchedTemplates.contains("sad-medi"),
              "Page 600 sad example remains partial");
        check(sw.baseGlyphs.size() == 3, "Expected three distinct letter bodies");
        const std::vector<Stretching> boundaries(sw.joins.begin(), sw.joins.end());
        Stretching entryCut{};
        entryCut.topBaseLine = boundaries[0].topLeft; entryCut.bottomBaseLine = boundaries[0].bottomLeft;
        check(same(proposal->first, entryCut) && same(proposal->second, boundaries[1]),
              "Assembly replaced the sad's independently proposed boundaries");
        if (wi == 0) check(sw.matchedTemplates.contains("hah-open-init") && same(hahCut, boundaries[0]),
                           "Assembly retained the overwide hah cut");
    }
    check(analysis.segmentSubword(2, 11, 5, 0, nullptr, true), "Looped kha-lam-waw control regressed");
    check(analysis.page.lines[11].words[5].wordResultInfo.subWords[0].matchedTemplates.contains("hah-init"),
          "Open hah was incorrectly used for a looped initial form");

    QFile spurPage("pages/page193.dat");
    check(spurPage.open(QIODevice::ReadOnly), "Page 193 is required");
    PageAnalysisResult spurAnalysis;
    QDataStream spurStream(&spurPage); spurStream >> spurAnalysis.page;
    check(spurStream.status() == QDataStream::Ok, "Invalid page 193 cache");
    auto& spurWord = spurAnalysis.page.lines[9].words[8];
    auto& spurSubword = spurWord.wordResultInfo.subWords[0];
    const auto spurShape = spurWord.bodyShape(spurSubword);
    auto spurInk = spurShape.transform.map(spurShape.path); spurInk.translate(spurShape.pos);
    auto spurGeometry = geometry(spurInk, dummy);
    const auto spurBody = medialLoopBoundaries(spurGeometry, loop, spurGeometry.bbox.left(), spurGeometry.bbox.right());
    check(spurBody && spurBody->second.bottomBaseLine.time == 16,
          "Sad exit must cross the narrow connector at the return corner");
    const auto spurTip = std::find_if(spurGeometry.timeSorted.begin(), spurGeometry.timeSorted.end(), [](const auto& p) {
        return std::abs(p.time - 18.5212683547) < 1e-6;
    });
    check(spurTip != spurGeometry.timeSorted.end() &&
          getJoinTime(spurBody->second, spurGeometry.orientation).isStrictRight(spurTip->time),
          "The descending spur was assigned to the connector");
    check(spurAnalysis.segmentSubword(192, 9, 8, 0, nullptr, true) &&
          spurSubword.baseGlyphs.size() == 4 && spurSubword.joins.size() == 3 &&
          same(spurBody->second, *std::next(spurSubword.joins.begin(), 2)),
          "Assembly replaced the corrected sad exit");
    qInfo("Sad/reh passed: fixed corners 2/17, retained tooth 8.55/19, two kashidas, complete reh ownership, independent geometry, affine invariance, missing-counter rejection and eight complete reh controls.");
    qInfo("Page 600 sad passed: open hah ownership, entry corners 31/6, intact counters/teeth, independent geometry, affine invariance, both complete examples and looped kha control.");
}
