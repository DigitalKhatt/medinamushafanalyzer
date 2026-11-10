// Meem's two necks belong to its own loop, including suspended ligatures.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static SearchContext geometry(QPainterPath ink, WordResultInfo::SubWordInfo& dummy) {
    ink.setFillRule(Qt::WindingFill);
    SearchContext context{.transformedPath = ink, .subword = dummy};
    context.bbox = ink.boundingRect();
    QVector<PathExtrema> points;
    ShapeItem::getExtrema(ink, context.subPaths, points);
    for (const auto& point : points) if (!point.pathIndex) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    context.setOrientation();
    context.nextIter = std::next(context.xSorted.begin());
    return context;
}

static bool same(const Stretching& a, const Stretching& b, const QTransform& map = {}) {
    return QLineF(map.map(a.topBaseLine.point), b.topBaseLine.point).length() < 1e-4 &&
        QLineF(map.map(a.bottomBaseLine.point), b.bottomBaseLine.point).length() < 1e-4;
}

static QImage mask(const QPainterPath& path, const QRectF& box) {
    constexpr double scale = 16;
    QImage image(int(std::ceil(box.width() * scale)) + 4,
        int(std::ceil(box.height() * scale)) + 4, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.translate(2, 2); painter.scale(scale, scale); painter.translate(-box.topLeft());
    painter.setPen(Qt::NoPen); painter.setBrush(Qt::white); painter.drawPath(path);
    return image;
}

static void partition(const QPainterPath& source, const WordResultInfo::SubWordInfo& sub) {
    auto ink = source; ink.setFillRule(Qt::WindingFill);
    const auto box = ink.boundingRect();
    const auto original = mask(ink, box);
    QVector<QImage> parts;
    for (const auto& body : sub.baseGlyphs) parts.append(mask(body.path, box));
    for (const auto& join : sub.kashidaPaths) if (!join.isEmpty()) parts.append(mask(join, box));
    int total = 0, missing = 0, extra = 0, overlap = 0;
    for (int y = 0; y < original.height(); ++y) for (int x = 0; x < original.width(); ++x) {
        const bool present = qRed(original.pixel(x, y)) != 0;
        int count = 0;
        for (const auto& part : parts) count += qRed(part.pixel(x, y)) != 0;
        total += present; missing += present && count == 0; extra += !present && count != 0; overlap += count > 1;
    }
    const int tolerance = std::max(8, total / 500);
    check(missing <= tolerance && extra <= tolerance && overlap <= tolerance,
        "Meem boundaries lost or duplicated ink");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Medial meem configuration rejected");
    check(!config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool(), "Tracker enabled");
    QJsonObject ordinary, hanging, upperNeck;
    for (const auto& value : config.value("medial").toArray()) {
        const auto form = resolvedBodyForm(config, value.toObject());
        if (form.value("id") == "meem-medi") ordinary = form;
        if (form.value("id") == "meem-hanging-medi") hanging = form;
        if (form.value("id") == "meem-upper-neck-medi") upperNeck = form;
    }
    check(!ordinary.isEmpty() && !hanging.isEmpty() && !upperNeck.isEmpty(), "Missing intrinsic meem forms");
    for (const auto& bad : QJsonArray{QJsonObject{{"maxDistance", 0}}, QJsonObject{{"required", "yes"}},
            QJsonObject{{"normalCut", 1}}, QJsonObject{{"placement", "sideways"}},
            QJsonObject{{"placement", "right-corner-pair"}}, QJsonObject{{"maxCounterY", .5}},
            QJsonObject{{"placement", "upper"}, {"maxCounterY", .6}}}) {
        auto invalid = config, rule = ordinary;
        const auto key = bad.toObject().contains("normalCut") || bad.toObject().contains("required") ? "cuts" : "body";
        auto section = rule.value(key).toObject(), loop = section.value("loop").toObject();
        loop.insert("exitCorner", bad); section.insert("loop", loop); rule.insert(key, section);
        invalid.insert("medial", QJsonArray{rule});
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid exit corner accepted");
    }

    struct Fixture {
        int page, line, word, subword, letter, counter;
        bool suspended;
        double rightCorner, leftCorner;
        bool steppedEntry = false;
    };
    const Fixture fixtures[] = {
        {1, 2, 3, 3, 1, 1, true, 24, 20},
        {3, 2, 9, 1, 1, 2, false, 21, 19},
        {5, 14, 5, 1, 1, 1, true, 8, 6},
        {6, 5, 2, 2, 1, 1, false, 4, 2},
        {6, 7, 3, 1, 2, 2, false, 4, 2},
        {6, 15, 4, 1, 2, 2, true, 24, 22},
        {7, 7, 1, 1, 1, 1, false, 11, 8},
        {8, 8, 7, 1, 2, 2, true, 32, 29},
        {182, 4, 3, 2, 2, 1, false, 44, 41, true},
        {230, 11, 3, 1, 2, 1, true, 16, 13}
    };
    for (const auto& f : fixtures) {
        qInfo("Meem fixture page%d:%d/%d/%d", f.page, f.line, f.word, f.subword);
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Meem fixture missing");
        PageAnalysisResult analysis; QDataStream stream(&file); stream >> analysis.page;
        auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        const auto& shape = word.paths[sub.paths.front()];
        const auto pagePath = [&](const QPainterPath& path) { auto out = shape.transform.map(path); out.translate(shape.pos); return out; };
        auto ink = pagePath(shape.path); ink.setFillRule(Qt::WindingFill);
        WordResultInfo::SubWordInfo dummy; auto context = geometry(ink, dummy);
        const auto hole = context.subPaths[f.counter].boundingRect();
        const auto& form = f.steppedEntry ? upperNeck : f.suspended ? hanging : ordinary;
        const auto settings = loopSettings(form);
        const auto locate = [&](SearchContext& c, const QRectF& box) {
            return medialLoopBoundaries(c, settings, box.center().x() - .01 * box.height(),
                box.center().x() + .01 * box.height());
        };
        const auto pair = locate(context, hole);
        check(bool(pair), "Intrinsic meem pair not found");
        check((f.rightCorner < 0 || pair->first.bottomBaseLine.time == f.rightCorner) &&
            pair->second.bottomBaseLine.time == f.leftCorner, "Meem cuts missed its own attachment corners");
        const auto curves = baseContourCurves(ink);
        const auto normalAtCorner = [&](const Stretching& cut, bool incoming) {
            const int index = static_cast<int>(std::lround(cut.bottomBaseLine.time)) % curves.size();
            const auto& previous = curves[(index + curves.size() - 1) % curves.size()];
            const auto& next = curves[index];
            const auto a = previous.pt3() - previous.pt4(), b = next.pt2() - next.pt1();
            const auto tangent = (incoming ? a.x() > b.x() : a.x() < b.x()) ? a : b;
            const auto chord = cut.topBaseLine.point - cut.bottomBaseLine.point;
            const double scale = std::hypot(tangent.x(), tangent.y()) * std::hypot(chord.x(), chord.y());
            check(scale > 0 && std::abs(QPointF::dotProduct(tangent, chord)) < 1e-5 * scale,
                "Meem neck cut is not normal to its attachment stroke");
        };
        if (f.rightCorner >= 0) normalAtCorner(pair->first, true);
        normalAtCorner(pair->second, false);
        const auto pieces = cutRegions(ink, {}, {pair->first, pair->second}, context.orientation, true);
        check(pieces.size() == 3, "Meem cuts cross");
        const auto contours = toSubpaths(pieces[1].path);
        check(contours.size() == 2 && contours[1] == context.subPaths[f.counter], "Meem lost or borrowed a counter");
        if (f.suspended) {
            check(pieces[1].path.boundingRect().height() <= 4 * hole.height(), "Hanging meem took a neighboring ascender");
            auto tooSmall = settings; tooSmall.insert("maxBodyHeight", 1);
            check(!medialLoopBoundaries(context, tooSmall, hole.center().x() - .01, hole.center().x() + .01),
                "Loop height constraint was ignored");
        }

        // Both boundaries stay fixed if neighboring labels, joins or scales change.
        Stretching unrelated{}; unrelated.topBaseLine.point = {-100, 200}; unrelated.bottomBaseLine.point = {-80, 400};
        dummy.joins.insert(unrelated); context.baseText = QStringLiteral("اختبار"); context.pairedStrokeWidth = 1000;
        const auto independent = locate(context, hole);
        check(independent && same(pair->first, independent->first) && same(pair->second, independent->second),
            "Meem geometry depends on a neighboring join or letter");
        for (double scale : {.65, 1.7}) {
            const QTransform map(scale, 0, 0, scale, 37, -21);
            auto scaled = geometry(map.map(ink), dummy);
            const auto shifted = locate(scaled, map.mapRect(hole));
            check(shifted && same(pair->first, shifted->first, map) && same(pair->second, shifted->second, map),
                "Meem geometry changed with scale or translation");
        }
        auto noCounter = geometry(context.subPaths.front(), dummy);
        check(!locate(noCounter, hole), "Meem accepted a shape without its counter");
        Stretching lowerCorner{};
        const auto ordinarySettings = loopSettings(ordinary);
        if (!counterEntryCorner(context, hole, ordinarySettings, lowerCorner)) {
            auto required = ordinarySettings, corner = required.value("entryCorner").toObject();
            corner.insert("required", true); required.insert("entryCorner", corner);
            check(!medialLoopBoundaries(context, required, hole.center().x() - .01, hole.center().x() + .01),
                "A required missing entry corner silently used an offset probe");
        }

        check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, nullptr, true),
            "Meem fixture remains partial");
        check(sub.matchedTemplates.contains(form.value("id").toString()), "Dispatcher used the wrong meem form");
        auto body = pagePath(sub.baseGlyphs[f.letter].path);
        check(mask(body, ink.boundingRect()) == mask(pieces[1].path, ink.boundingRect()),
            "Dispatcher moved the intrinsic meem boundary");
        // A later neighboring-body policy can open a genuine connector at a
        // hanging entry (already present in the committed page 6 fixture).
        // Judge the final geometry, not an intermediate shared-entry trace.
        if (f.suspended && !sub.kashidaPaths[f.letter - 1].isEmpty())
            check(sub.kashidaCuts[f.letter - 1].first != sub.kashidaCuts[f.letter - 1].second,
                "Nonempty connector has coincident boundaries");
        partition(shape.path, sub);
        const auto bodies = sub.baseGlyphs;
        const auto joins = sub.kashidaPaths;
        check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, nullptr, true) &&
            sub.kashidaPaths == joins, "Repeated segmentation changed connectors");
        for (size_t i = 0; i < bodies.size(); ++i) check(sub.baseGlyphs[i].path == bodies[i].path, "Repeated segmentation changed bodies");
    }

    // A small change in meem's exit must not make lam take the following waw head.
    QFile controlFile("pages/page13.dat"); check(controlFile.open(QIODevice::ReadOnly), "Control page missing");
    PageAnalysisResult control; QDataStream stream(&controlFile); stream >> control.page;
    check(control.segmentSubword(12, 8, 2, 0, nullptr, true), "Ta-ain-meem-lam-waw became partial");
    const auto& sub = control.page.lines[8].words[2].wordResultInfo.subWords[0];
    check(toSubpaths(sub.baseGlyphs[2].path).size() == 2 && toSubpaths(sub.baseGlyphs[3].path).size() == 1 &&
        toSubpaths(sub.baseGlyphs[4].path).size() == 2, "Lam borrowed the meem or waw counter");
    QFile noonFile("pages/page397.dat"); check(noonFile.open(QIODevice::ReadOnly), "Noon control page missing");
    PageAnalysisResult noon; QDataStream noonStream(&noonFile); noonStream >> noon.page;
    check(noon.segmentSubword(396, 11, 4, 1, nullptr, true), "Lam route blocked an independently matched final noon");
    const auto& noonWord = noon.page.lines[11].words[4];
    const auto& noonSub = noonWord.wordResultInfo.subWords[1];
    check(noonSub.matchedTemplates.contains("noon-fina") && noonSub.baseGlyphs.size() == 6,
        "A partial lam route suppressed a compatible final boundary");
    partition(noonWord.paths[noonSub.paths.front()].path, noonSub);
    qInfo("Medial meem corners, hanging ligatures, independence and ink conservation passed");
}
