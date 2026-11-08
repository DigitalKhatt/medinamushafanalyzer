// Keep the folded ligature's crossing test stable at exact Bezier corners.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static SearchContext geometry(QPainterPath path, WordResultInfo::SubWordInfo& dummy,
                              const QJsonObject& form, const QJsonObject& defaults) {
    path.setFillRule(Qt::WindingFill);
    SearchContext context{.form = form, .joinDefaults = defaults, .transformedPath = path, .subword = dummy};
    context.baseText = QStringLiteral("لإ");
    context.bbox = path.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(path, context.subPaths, extrema);
    for (const auto& point : extrema) if (point.pathIndex == 0) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    check(context.timeSorted.size() >= 3, "Missing ligature outline");
    context.setOrientation();
    context.nextIter = std::next(context.xSorted.begin());
    return context;
}

static QImage mask(const QPainterPath& path, const QRectF& box) {
    constexpr double scale = 24;
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

static void checkSeparateAlefTip() {
    QFile pageFile("pages/page360.dat"), fontFile("input/font.dat");
    check(pageFile.open(QIODevice::ReadOnly) && fontFile.open(QIODevice::ReadOnly), "Missing detached alef fixture");
    AFont font;
    QDataStream fontStream(&fontFile);
    fontStream >> font;
    PageAnalysisResult a;
    QDataStream pageStream(&pageFile);
    pageStream >> a.page;
    PageAnalysisResult::initQuranText();
    a.resolveWordBoundaries(360, &font);
    check(a.segmentSubword(359, 5, 2, 2, &font, true), "Folded alef beside the lam bowl in صِيلا remains partial");
    const auto& word = a.page.lines[5].words[2];
    const auto& sub = word.wordResultInfo.subWords[2];
    const auto shape = word.bodyShape(sub);
    const auto outlines = toSubpaths(shape.path);
    check(outlines.size() == 3 && sub.baseGlyphs.size() == 4 && !sub.provisional(), "Expected four matched bodies");
    check(sub.letterDiagnostics.last().toObject().value("template") == "alef-detached-fina" &&
          sub.baseGlyphs.back().path == outlines.back(), "Final alef must own its entire original diagonal");
    const auto lam = shape.transform.map(sub.baseGlyphs[2].path).translated(shape.pos);
    check(lam.contains({239, 218.5}) && lam.contains({236, 226.8}), "Lam lost its upright or its lower return");
    check(sub.joins.size() == 2 && sub.kashidaPaths.size() == 2 &&
          !sub.kashidaPaths[0].isEmpty() && !sub.kashidaPaths[1].isEmpty() && coversOriginalInk(word, sub),
          "Expected two earlier connectors and separate lam and alef outlines");

    // Bringing the same tip into the bowl must still reject a genuine overlap.
    for (double scale : {0.5, 1.0, 2.0}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        const auto body = mapping.map(outlines.front());
        check(isSeparateFoldedArm(body, mapping.map(outlines.back()), false), "Disjoint tip became a bridge");
        check(!isSeparateFoldedArm(body, mapping.map(outlines.back().translated(.2, 0)), true),
              "Overlapping alef tip accepted as a separate contour");
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject form, attachedForm;
    for (const auto& value : config.value("final").toArray()) {
        if (value.toObject().value("id") == "alef-folded-fina") form = value.toObject();
        if (value.toObject().value("id") == "alef-open-fina") attachedForm = value.toObject();
    }
    check(form.value("minCrossings").toInt() == 4, "Lam-alef crossing requirement was weakened");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 fixture is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page 600 fixture");
    auto& word = analysis.page.lines[0].words[1];
    auto& sub = word.wordResultInfo.subWords[1];
    const auto& shape = word.paths[sub.paths.front()];
    auto source = shape.transform.map(shape.path);
    source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    const auto defaults = config.value("joinDefaults").toObject();
    for (const auto& text : {QStringLiteral("لا"), QStringLiteral("لأ"), QStringLiteral("لإ"),
                             QStringLiteral("لآ"), QStringLiteral("لٱ")}) {
        auto context = geometry(source, dummy, form, defaults);
        context.baseText = text;
        check(searchLamAlefLiga(context) && context.res.topBaseLine.time == 5 && context.res.bottomBaseLine.time == 14,
              "Lam-alef did not retain the exact 5/14 crossing corners");
    }
    const auto base = geometry(source, dummy, form, defaults);
    for (double scale : {0.7, 1.0, 1.6}) {
        auto scaledForm = form;
        auto bounds = scaledForm.value("join").toObject();
        for (const auto* key : {"minX", "maxX", "minY", "maxY"})
            bounds.insert(key, bounds.value(key).toDouble() * scale);
        scaledForm.insert("join", bounds);
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto context = geometry(mapping.map(source), dummy, scaledForm, defaults);
        check(searchLamAlefLiga(context) && context.res.topBaseLine.time == 5 && context.res.bottomBaseLine.time == 14,
              "Lam-alef endpoint check changed under translation or page-unit scaling");
    }
    auto open = geometry(base.subPaths.front(), dummy, form, defaults);
    check(!searchLamAlefLiga(open), "Folded ligature accepted without its lower counter crossings");
    auto unrelated = geometry(source, dummy, form, defaults);
    unrelated.baseText = QStringLiteral("بإ");
    check(!searchLamAlefLiga(unrelated), "Lam-alef rule accepted an unrelated letter pair");

    check(analysis.segmentSubword(599, 0, 1, 1, nullptr, true), "Lam-alef with hamza below remains partial");
    check(sub.matchedTemplates.contains("alef-folded-fina") && sub.baseGlyphs.size() == 2 && sub.joins.size() == 1,
          "Ligature did not produce two letter bodies and one shared boundary");
    const auto& cut = *sub.joins.begin();
    check(cut.topBaseLine.time == 5 && cut.bottomBaseLine.time == 14 && !cut.fixedLeftBoundary &&
          std::all_of(sub.kashidaPaths.begin(), sub.kashidaPaths.end(), [](const auto& path) { return path.isEmpty(); }),
          "Ligature boundary moved or introduced a kashida");
    check(toSubpaths(sub.baseGlyphs[0].path).size() == 2 && toSubpaths(sub.baseGlyphs[1].path).size() == 1,
          "Lam lost its folded lower return or alef acquired the counter");
    auto local = shape.path;
    local.setFillRule(Qt::WindingFill);
    const auto box = local.boundingRect();
    const auto original = mask(local, box), lam = mask(sub.baseGlyphs[0].path, box), alef = mask(sub.baseGlyphs[1].path, box);
    int ink = 0, missing = 0, extra = 0, overlap = 0;
    for (int y = 0; y < original.height(); ++y) for (int x = 0; x < original.width(); ++x) {
        const bool present = qRed(original.pixel(x, y)) != 0;
        const int count = (qRed(lam.pixel(x, y)) != 0) + (qRed(alef.pixel(x, y)) != 0);
        ink += present; missing += present && count == 0; extra += !present && count != 0; overlap += count > 1;
    }
    const int tolerance = std::max(8, ink / 500);
    check(missing <= tolerance && extra <= tolerance && overlap <= tolerance,
          "Shared ligature cut lost or duplicated source ink");
    const auto bodies = sub.baseGlyphs;
    check(analysis.segmentSubword(599, 0, 1, 1, nullptr, true), "Repeated ligature segmentation became partial");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(bodies[i].path == sub.baseGlyphs[i].path, "Repeated ligature segmentation changed a mask");

    // Attached lam-alef: the diagonal is alef, while the upright and the
    // long lower return both belong to lam. Its interface is not a kashida.
    auto& attachedWord = analysis.page.lines[1].words[3];
    auto& attached = attachedWord.wordResultInfo.subWords[1];
    const auto& attachedShape = attachedWord.paths[attached.paths.front()];
    auto attachedSource = attachedShape.transform.map(attachedShape.path);
    attachedSource.translate(attachedShape.pos);
    for (const auto& text : {QStringLiteral("فلا"), QStringLiteral("فلأ"), QStringLiteral("فلإ"),
                             QStringLiteral("فلآ"), QStringLiteral("فلٱ")}) {
        auto context = geometry(attachedSource, dummy, attachedForm, defaults);
        context.baseText = text;
        check(searchLamAlefLiga(context) && context.res.topBaseLine.time == 4 && context.res.bottomBaseLine.time == 12,
              "Attached lam-alef did not retain the exact 4/12 shared corners");
    }
    auto wrongShape = geometry(source, dummy, attachedForm, defaults);
    check(!searchLamAlefLiga(wrongShape), "Attached rule accepted an initial lam");
    auto wrongPair = geometry(attachedSource, dummy, attachedForm, defaults);
    wrongPair.baseText = QStringLiteral("فبا");
    check(!searchLamAlefLiga(wrongPair), "Attached rule accepted an unrelated suffix");
    check(analysis.segmentSubword(599, 1, 3, 1, nullptr, true), "Attached lam-alef remains partial");
    check(attached.baseGlyphs.size() == 3 && attached.joins.size() == 2 &&
          attached.matchedTemplates.contains("lam-medi") && attached.matchedTemplates.contains("alef-open-fina"),
          "Attached ligature did not match both lam and alef bodies");
    const auto& shared = *attached.joins.rbegin();
    check(shared.topBaseLine.time == 4 && shared.bottomBaseLine.time == 12 && !shared.fixedLeftBoundary &&
          attached.kashidaPaths.size() == 2 && !attached.kashidaPaths[0].isEmpty() && attached.kashidaPaths[1].isEmpty(),
          "Attached ligature moved its shared cut or assigned it a kashida");
    const auto attachedGeometry = geometry(attachedSource, dummy, attachedForm, defaults);
    const auto ownsLandmark = [&](int body, double time) {
        auto path = attachedShape.transform.map(attached.baseGlyphs[body].path);
        path.translate(attachedShape.pos);
        for (const auto& point : attachedGeometry.timeSorted)
            if (std::abs(point.time - time) < .001) return path.contains(point.point + QPointF(0, .03));
        return false;
    };
    check(ownsLandmark(1, 30.09985) && ownsLandmark(2, 6.40378),
          "Medial lam selected the diagonal alef crest instead of its own upright");
    auto attachedLocal = attachedShape.path;
    attachedLocal.setFillRule(Qt::WindingFill);
    const auto attachedBox = attachedLocal.boundingRect();
    const auto attachedInk = mask(attachedLocal, attachedBox);
    QVector<QImage> parts;
    for (const auto& body : attached.baseGlyphs) parts.append(mask(body.path, attachedBox));
    for (const auto& path : attached.kashidaPaths) if (!path.isEmpty()) parts.append(mask(path, attachedBox));
    ink = missing = extra = overlap = 0;
    for (int y = 0; y < attachedInk.height(); ++y) for (int x = 0; x < attachedInk.width(); ++x) {
        const bool present = qRed(attachedInk.pixel(x, y)) != 0;
        int count = 0;
        for (const auto& part : parts) count += qRed(part.pixel(x, y)) != 0;
        ink += present; missing += present && count == 0; extra += !present && count != 0; overlap += count > 1;
    }
    check(std::max({missing, extra, overlap}) <= std::max(8, ink / 500),
          "Attached ligature lost or duplicated ink");
    const auto attachedBodies = attached.baseGlyphs;
    check(analysis.segmentSubword(599, 1, 3, 1, nullptr, true), "Repeated attached segmentation became partial");
    for (size_t i = 0; i < attachedBodies.size(); ++i)
        check(attachedBodies[i].path == attached.baseGlyphs[i].path, "Repeated attached segmentation changed a mask");
    checkSeparateAlefTip();
    qInfo("Lam-alef passed: standalone and attached ligatures, separate diagonal and real-overlap rejection, ink ownership and deterministic masks.");
}
