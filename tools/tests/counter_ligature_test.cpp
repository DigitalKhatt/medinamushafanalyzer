// A shared counter must be partitioned by its actual inner/outer arcs.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }
static SearchContext geometry(QPainterPath path, WordResultInfo::SubWordInfo& dummy, const QJsonObject& form) {
    path.setFillRule(Qt::WindingFill);
    SearchContext c{.form = form, .transformedPath = path, .subword = dummy};
    c.bbox = path.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(path, c.subPaths, extrema);
    for (const auto& point : extrema) if (point.pathIndex == 0) {
        c.timeSorted.insert(point); c.xSorted.insert(point); c.ySorted.insert(point);
    }
    c.setOrientation(); c.nextIter = std::next(c.xSorted.begin());
    return c;
}
static QImage mask(const QPainterPath& path, const QRectF& box) {
    constexpr double scale = 24;
    QImage image(int(std::ceil(box.width() * scale)) + 4, int(std::ceil(box.height() * scale)) + 4, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter p(&image); p.translate(2, 2); p.scale(scale, scale); p.translate(-box.topLeft());
    p.setPen(Qt::NoPen); p.setBrush(Qt::white); p.drawPath(path);
    return image;
}
static void partition(const QPainterPath& source, const std::vector<GlyphInfo>& bodies) {
    const auto box = source.boundingRect();
    const auto original = mask(source, box);
    QVector<QImage> parts;
    for (const auto& body : bodies) parts.append(mask(body.path, box));
    int ink = 0, missing = 0, extra = 0, overlap = 0;
    for (int y = 0; y < original.height(); ++y) for (int x = 0; x < original.width(); ++x) {
        const bool present = qRed(original.pixel(x, y)) != 0;
        int count = 0; for (const auto& part : parts) count += qRed(part.pixel(x, y)) != 0;
        ink += present; missing += present && count == 0; extra += !present && count != 0; overlap += count > 1;
    }
    check(std::max({missing, extra, overlap}) <= std::max(8, ink / 500), "Counter cut loses, adds or duplicates ink");
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    QJsonObject form;
    for (const auto& value : config.value("initial").toArray())
        if (value.toObject().value("id") == "kaf-ascender-init") form = value.toObject();
    check(!form.isEmpty(), "Missing counter ligature template");
    QFile file("pages/page600.dat"); check(file.open(QIODevice::ReadOnly), "Missing page 600 fixture");
    PageAnalysisResult analysis; QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[6].words[2]; auto& sub = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[sub.paths.front()];
    auto source = shape.transform.map(shape.path); source.translate(shape.pos); source.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(source, dummy, form);
    check(declaredSharedBoundary(context, form), "Joined kaf-ascender did not match");
    const auto cut = context.res;
    check(cut.topBaseLine.pathIndex == 0 && cut.topBaseLine.time == 3 &&
          cut.counterEntry.pathIndex == 1 && cut.counterEntry.time == 3 &&
          cut.counterExit.pathIndex == 1 && std::abs(cut.counterExit.time - .8060679908) < 1e-6 &&
          std::abs(cut.bottomBaseLine.time - 10.9349718297) < 1e-6,
          "Upper bridge missed outer 3/inner 3, or lower bridge missed the bottom extrema");
    check(std::abs(cut.counterExit.point.y() - context.subPaths[1].boundingRect().bottom()) < 1e-5 &&
          std::abs(cut.referenceWidth - cut.cutSegments()[0].length()) < 1e-5,
          "Baseline or local stroke scale was measured from the whole loop");
    check(cut.cutSegments().size() == 2, "Ligature did not expose two separate ink segments");
    for (const auto& line : cut.cutSegments()) check(interiorInkSegment(source, line.p1(), line.p2()), "Ligature cut crosses background");
    const auto pieces = cutRegions(source, {}, {cut}, context.orientation, true);
    check(pieces.size() == 2 && toSubpaths(pieces[0].path).size() == 1 && toSubpaths(pieces[1].path).size() == 1,
          "Shared counter was filled in or left as a whole hole in one body");
    const auto counterCenter = context.subPaths[1].boundingRect().center();
    check(!pieces[0].path.contains(counterCenter) && !pieces[1].path.contains(counterCenter), "Counter became ink");
    check(std::abs(pieces[1].path.boundingRect().left() - context.bbox.left()) < 1e-5 &&
          std::abs(pieces[1].path.boundingRect().bottom() - context.bbox.bottom()) < 1e-5 &&
          std::abs(pieces[0].path.boundingRect().right() - context.bbox.right()) < 1e-5,
          "Ascender lost its stem/return or kaf lost its upper arm");
    partition(source, pieces);
    auto noCounter = geometry(context.subPaths[0], dummy, form);
    check(!declaredSharedBoundary(noCounter, form), "Counter ligature accepted without its counter");
    Stretching unrelated{}; unrelated.topBaseLine.point = {-200, 300}; unrelated.bottomBaseLine.point = {-100, 500};
    dummy.joins.insert(unrelated); context.baseText = QStringLiteral("اختبار"); context.pairedStrokeWidth = 1000;
    check(declaredSharedBoundary(context, form) && context.res.topBaseLine.time == 3 && context.res.counterEntry.time == 3,
          "Ligature geometry depends on neighboring text or landmarks");
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy, form);
        check(declaredSharedBoundary(scaled, form) && scaled.res.topBaseLine.time == 3 && scaled.res.counterEntry.time == 3,
              "Ligature landmarks changed under scale or translation");
        check(std::abs(scaled.res.bottomBaseLine.time - cut.bottomBaseLine.time) < 1e-6 &&
              std::abs(scaled.res.counterExit.time - cut.counterExit.time) < 1e-6 &&
              std::abs(scaled.res.referenceWidth - scale * cut.referenceWidth) < 1e-5,
              "Baseline or stroke scale changed under scale or translation");
        const auto transformed = cutRegions(scaled.transformedPath, {}, {scaled.res}, scaled.orientation, true);
        check(transformed.size() == 2, "Transformed ligature did not partition");
        partition(scaled.transformedPath, transformed);
    }
    // Compose the counter boundary with another ordinary outer-contour cut.
    const double x = context.bbox.right() - context.subPaths[1].boundingRect().width() / 2;
    const auto hits = FindInterections(source, QLineF(x, context.bbox.top(), x, context.bbox.bottom()));
    check(hits.size() == 2, "Upper-arm cross-section fixture changed");
    Stretching arm{}; arm.topBaseLine = hits[0]; arm.bottomBaseLine = hits[1];
    const auto compound = cutRegions(source, {}, {arm, cut}, context.orientation, true);
    check(compound.size() == 3, "Counter cut failed beside an ordinary cut"); partition(source, compound);
    const auto text = sub.text;
    for (const auto alef : QStringLiteral("اآأإٱ")) {
        sub.text = QString(QChar(0x0643)) + alef;
        check(analysis.segmentSubword(599, 6, 2, 0, nullptr, true) && sub.baseGlyphs.size() == 2 &&
              sub.matchedTemplates.contains("kaf-ascender-init"), "Ligature failed for an alef variant");
        check(std::all_of(sub.kashidaPaths.begin(), sub.kashidaPaths.end(), [](const auto& p) {return p.isEmpty();}),
              "Shared ligature interface became a kashida");
        auto local = shape.path; local.setFillRule(Qt::WindingFill); partition(local, sub.baseGlyphs);
    }
    sub.text = text;
    check(analysis.segmentSubword(599, 6, 2, 0, nullptr, true), "Repeated ligature became partial");
    const auto bodies = sub.baseGlyphs;
    check(analysis.segmentSubword(599, 6, 2, 0, nullptr, true), "Repeated ligature became partial");
    for (size_t i = 0; i < bodies.size(); ++i) check(bodies[i].path == sub.baseGlyphs[i].path, "Ligature masks are nondeterministic");
    sub.text = QStringLiteral("كف"); analysis.segmentSubword(599, 6, 2, 0, nullptr, true);
    check(!sub.matchedTemplates.contains("kaf-ascender-init"), "Ligature accepted an unconfigured paired letter");
    sub.text = text;

    // The same loop can be shared with medial lam, including a longer
    // lam-alef return. The elbow fallback must not swallow this closed loop.
    auto noLigature = config;
    QJsonArray remaining;
    for (const auto& value : config.value("initial").toArray())
        if (value.toObject().value("id") != "kaf-ascender-init") remaining.append(value);
    noLigature.insert("initial", remaining);
    for (const auto [line, wordIndex] : {std::pair{13, 4}, std::pair{12, 7}, std::pair{13, 0}}) {
        auto& w = analysis.page.lines[line].words[wordIndex];
        auto& s = w.wordResultInfo.subWords[0];
        check(analysis.segmentSubword(599, line, wordIndex, 0, nullptr, true) && s.baseGlyphs.size() == 3 &&
              s.matchedTemplates.contains("kaf-ascender-init") && s.matchedTemplates.contains("lam-medi") &&
              s.matchedTemplates.contains("alef-open-fina") && !s.matchedTemplates.contains("kaf-elbow-init"),
              "Kaf-lam-alef did not match its three body templates");
        const auto& bridge = *s.joins.begin();
        check(bridge.topBaseLine.time == 2 && bridge.counterEntry.time == 4 &&
              bridge.bottomBaseLine.isY && bridge.counterExit.isY && bridge.referenceWidth > 0,
              "Kaf-lam bridge selected an intermediate arm corner or missed the baseline");
        check(std::all_of(s.kashidaPaths.begin(), s.kashidaPaths.end(), [](const auto& p) {return p.isEmpty();}),
              "Kaf-lam-alef introduced a kashida inside a shared ligature");
        auto ink = w.paths[s.paths.front()].path; ink.setFillRule(Qt::WindingFill);
        partition(ink, s.baseGlyphs);
        analysis.segmentSubword(599, line, wordIndex, 0, nullptr, true, &noLigature);
        check(!s.matchedTemplates.contains("kaf-elbow-init"), "Elbow fallback accepted a closed-loop body");
    }
    qInfo("Counter ligature passed: upper corners, baseline extrema, two ink bridges, exact inner arcs, empty counter, retained ascender/kaf, no kashida, missing-counter rejection, neighboring-landmark independence, scale/translation, ordinary-cut composition, all alef variants, three kaf-lam-alef forms, elbow rejection, local masks and determinism.");
}
