#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

LigatureMatch segmentDeclaredLigature(const Word& word, WordResultInfo::SubWordInfo& sub,
                                      const QJsonObject& config) {
    const auto rules = config.value("ligatures").toArray();
    if (rules.isEmpty() || sub.text.contains(QChar(0x0640))) return LigatureMatch::NotApplicable;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() != 2) return LigatureMatch::NotApplicable;
    const auto selected = std::find_if(rules.begin(), rules.end(), [&](const auto& value) {
        const auto selectors = value.toObject().value("letters").toArray();
        return selectors.size() == 2 && selectors.at(0).toString().contains(letters.front()) &&
               selectors.at(1).toString().contains(letters.back());
    });
    if (selected == rules.end()) return LigatureMatch::NotApplicable;

    const auto rule = selected->toObject();
    const auto id = rule.value("id").toString();
    const auto initial = bodyConstraintForm(config, ligatureForm(config, rule.value("initial").toObject()));
    const auto final = bodyConstraintForm(config, ligatureForm(config, rule.value("final").toObject()));
    const auto initialId = initial.value("id").toString();
    const auto finalId = final.value("id").toString();
    auto trial = sub;
    const auto clearCuts = [&] {
        trial.baseGlyphs.clear();
        trial.kashidaPaths.clear();
        trial.kashidaCuts.clear();
        trial.joins.clear();
    };
    clearCuts();
    trial.extrema.clear();
    trial.templateTrace.clear();
    const auto slot = [&](int index, const QString& form, bool matched) {
        return QJsonObject{{"letterIndex", index + 1}, {"character", letters.mid(index, 1)},
            {"position", index == 0 ? "initial" : "final"}, {"status", matched ? "matched" : "no-match"},
            {"template", matched ? form : QString{}}, {"candidates", QJsonArray{form}}};
    };
    trial.letterDiagnostics = {slot(0, initialId, false), slot(1, finalId, false)};
    trial.correct = false;
    const auto fail = [&](const QString& reason) {
        clearCuts();
        trial.templateTrace.append(id + " · " + reason);
        sub = std::move(trial);
        return LigatureMatch::Failed;
    };
    if (sub.paths.empty() || sub.paths.front() < 0 || sub.paths.front() >= int(word.paths.size()))
        return fail("no source outline");
    const auto shape = word.bodyShape(sub);
    OutlineSearchCache cache;
    SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                          .subword = trial, .baseText = letters, .outlineCache = &cache};
    context.transformedPath.setFillRule(Qt::WindingFill);
    context.bbox = context.transformedPath.boundingRect();
    if (!preparePassOutline(context, cache)) return fail("no contour landmarks");
    trial.basePathOrientation = context.orientation;
    context.nextIter = std::next(context.xSorted.begin());

    // The receiving body supplies the one seam. The initial form must retain
    // its defining landmarks at that seam without supplying a second cut.
    if (!declaredSharedBoundary(context, final))
        return fail(finalId + " · no shared boundary");
    if (final.value("ownsLeftTip").toBool() && final.value("bottomProbe") == "normal" &&
        getJoinTime(context.res, context.orientation).isStrictRight(context.xSorted.rbegin()->time))
        std::swap(context.res.topBaseLine, context.res.bottomBaseLine);
    if (!matchesBoundaryBody(context, final, context.res, false))
        return fail(finalId + " · shared cut loses body features");
    auto seam = context.res;
    const auto body = initial.value("body").toObject();
    if (body.value("shape") == "suspended-loop") {
        InspectFormScope inspection(initial, context.transformedPath);
        const auto evidence = recognizedSharedShape(context, body);
        inspectSharedShape(*evidence);
        const auto parts = cutRegions(context.transformedPath, {}, {seam}, context.orientation, true);
        if (!evidence->head || parts.size() != 2 ||
            !parts.front().path.boundingRect().contains(evidence->head->counter.box) ||
            parts.back().path.boundingRect().intersects(evidence->head->counter.box))
            return fail(initialId + " · shared cut loses the loop");
    } else {
        if (!declaredSharedBoundary(context, initial, &seam))
            return fail(initialId + " · body not recognized");
        if (cutPrecedes(seam, context.res, context.orientation))
            return fail(initialId + " · shared cut precedes the initial body's exit");
        if (rule.contains("minConnectorWidths")) {
            const auto& exit = context.res;
            if (!cutPrecedes(exit, seam, context.orientation) || !matchesBoundaryBody(context, initial, exit, true))
                return fail(initialId + " · initial exit does not fit before the shared cut");
            const auto midpointX = [](const Stretching& cut) {
                return (cut.topBaseLine.point.x() + cut.bottomBaseLine.point.x()) / 2;
            };
            const double minSpan = rule.value("minConnectorWidths").toDouble() * config.value("strokeWidth").toDouble(1);
            if (midpointX(exit) - midpointX(seam) >= minSpan)
                return LigatureMatch::SeparateConnector;
        }
    }
    if (!matchesBoundaryBody(context, initial, seam, true))
        return fail(initialId + " · shared cut loses body features");
    seam.topLeft = seam.topRight = seam.topBaseLine;
    seam.bottomLeft = seam.bottomRight = seam.bottomBaseLine;
    seam.fixedLeftBoundary = false;
    seam.length = 0;
    seam.kashidaOverrides.insert("enabled", false);
    trial.joins.insert(seam);
    trial.baseGlyphs = cutRegions(shape.path, trial.text, {seam}, context.orientation);
    trial.kashidaPaths.resize(1);
    trial.kashidaCuts.resize(1);
    if (trial.baseGlyphs.size() != 2 ||
        std::any_of(trial.baseGlyphs.begin(), trial.baseGlyphs.end(), [](const auto& glyph) { return glyph.path.isEmpty(); }) ||
        (initial.contains("bodyCounters") &&
         int(toSubpaths(trial.baseGlyphs.front().path).size()) != initial.value("bodyCounters").toInt() + 1) ||
        (final.contains("bodyCounters") &&
         int(toSubpaths(trial.baseGlyphs.back().path).size()) != final.value("bodyCounters").toInt() + 1) ||
        !coversOriginalInk(word, trial))
        return fail("shared cut failed ink ownership");
    if ((hasToothCluster(initial) && !retainsToothCluster(context, initial.value("body").toObject(),
            shape.transform.map(trial.baseGlyphs.front().path).translated(shape.pos), false, &seam, false,
            config.value("strokeWidth").toDouble(1))) ||
        (hasToothCluster(final) && !retainsToothCluster(context, final.value("body").toObject(),
            shape.transform.map(trial.baseGlyphs.back().path).translated(shape.pos), true, &seam, true,
            config.value("strokeWidth").toDouble(1))))
        return fail("shared cut loses a tooth or the terminal bowl");
    sub = std::move(trial);
    sub.letterDiagnostics = {slot(0, initialId, true), slot(1, finalId, true)};
    recordBoundaryTemplate(sub, 0, false, finalId);
    sub.templateTrace.append(id + " · " + initialId + " + " + finalId + " · shared cut");
    sub.correct = true;
    return LigatureMatch::SharedCut;
}

bool connectorContainsLeadingStroke(const QVector<PathExtrema>& extrema,
        const Stretching& right, const Stretching& left, Orientation orientation) {
    const PathExtrema* origin = nullptr;
    for (const auto& p : extrema) if (p.pathIndex == 0 &&
        (!origin || p.point.x() > origin->point.x())) origin = &p;
    return origin && getJoinTime(right, orientation).isStrictLeft(origin->time) &&
        getJoinTime(left, orientation).isStrictRight(origin->time);
}

} // namespace MedinaCutter

QString PageAnalysisResult::bodyLetters(const QString& text) {
    QString letters;
    for (const auto ch : text) {
        // U+0640 supplies joining/spacing (often under a combining hamza),
        // not an extra letter body. The marks remain in the source components.
        if (!ch.isMark() && ch != QChar(0x0640)) letters += ch;
    }
    return letters;
}

namespace MedinaCutter {

bool coversOriginalInk(const Word& word, const WordResultInfo::SubWordInfo& sub) {
    if (sub.paths.empty())
        return false;
    const auto shape = word.bodyShape(sub);
    auto world = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path);
        p.translate(shape.pos);
        return p;
    };
    auto ink = world(shape.path);
    ink.setFillRule(Qt::WindingFill);
    const auto box = ink.boundingRect();
    constexpr double scale = 12;
    const QSize size(int(std::ceil(box.width() * scale)) + 4, int(std::ceil(box.height() * scale)) + 4);
    if (size.isEmpty())
        return false;
    const QTransform transform(scale, 0, 0, scale, 2 - box.left() * scale, 2 - box.top() * scale);
    const auto inverse = transform.inverted();
    auto render = [&](const QPainterPath& path) {
        QImage image(size, QImage::Format_Grayscale8);
        image.fill(0);
        QPainter painter(&image);
        painter.setTransform(transform);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPath(path);
        painter.end();
        return image;
    };
    const auto source = render(ink);
    std::vector<QPainterPath> shapes;
    for (const auto& body : sub.baseGlyphs)
        shapes.push_back(world(body.path));
    for (const auto& join : sub.kashidaPaths)
        if (!join.isEmpty())
            shapes.push_back(world(join));
    std::vector<QImage> images;
    for (const auto& path : shapes)
        images.push_back(render(path));
    std::optional<QPainterPath> boundary;
    std::optional<QPainterPath> preciseInk;
    std::vector<QPainterPath> preciseShapes;
    std::vector<const uchar*> scan(images.size());
    for (int y = 0; y < size.height(); ++y) {
        for (size_t i = 0; i < images.size(); ++i)
            scan[i] = images[i].constScanLine(y);
        const auto line = source.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            int owners = 0;
            for (auto row : scan)
                owners += row[x] != 0;
            if (owners == int(line[x] != 0))
                continue;
            const auto probe = inverse.map(QPointF(x + .5, y + .5));
            int actual = 0;
            for (const auto& path : shapes)
                actual += path.contains(probe);
            if (actual == int(ink.contains(probe))) continue;
            if (cutGeometryMemo && cutGeometryMemo->preciseGeometry) {
                // A subdivided cubic and its original can flatten differently.
                // Retest at finer precision before treating it as extra or missing ink.
                const auto detail = QTransform::fromScale(64, 64);
                if (!preciseInk) {
                    preciseInk = detail.map(ink);
                    for (const auto& path : shapes) preciseShapes.push_back(detail.map(path));
                }
                const auto point = detail.map(probe);
                const auto count = std::ranges::count_if(preciseShapes,
                    [&](const auto& path) { return path.contains(point); });
                if (count == int(preciseInk->contains(point))) continue;
            }
            if (actual > 1) return false;
            if (!boundary) {
                QPainterPathStroker stroke;
                stroke.setWidth(.002);
                stroke.setCurveThreshold(1e-5);
                boundary = stroke.createStroke(ink);
            }
            if (!boundary->contains(probe)) return false;
        }
    }
    return true;
}

// Compare the two sides by arc length. A short bent connector can lie well
// outside its straight endpoint chord while both sides still follow the same
// thin stroke. A tooth or head produces a long unmatched excursion instead.
bool followsOneStroke(const QPainterPath& ribbon, const QLineF& right, const QLineF& left,
                      ConnectorEndpointMatch match, double nominalWidth) {
    auto curves = baseContourCurves(ribbon);
    if (curves.size() < 3)
        return false;
    const auto first = curves.front().pt1(), last = curves.back().pt4();
    if (QLineF(last, first).length() > 1e-6) {
        const QLineF closing(last, first);
        curves.push_back(QBezier::fromPoints(last, closing.pointAt(1. / 3), closing.pointAt(2. / 3), first));
    }
    const int n = int(curves.size());
    std::array<int, 4> role{-1, -1, -1, -1};
    const std::array<QPointF, 4> end{right.p1(), right.p2(), left.p1(), left.p2()};
    // Fixed sections can be closer together than the tolerance. Use the
    // nearest vertices for these sections and preserve the existing default.
    for (int k = 0; k < 4; ++k) {
        double nearest = .002;
        for (int i = 0; i < n; ++i) {
            const double distance = QLineF(end[k], curves[i].pt1()).length();
            if (distance < nearest) {
                role[k] = i;
                if (match == ConnectorEndpointMatch::FirstWithinTolerance) break;
                nearest = distance;
            }
        }
    }
    if (std::any_of(role.begin(), role.end(), [](int r) { return r < 0; }))
        return false;
    std::array<std::vector<QPointF>, 2> points;
    std::array<std::vector<double>, 2> distances;
    for (int k = 0; k < 2; ++k) {
        int at = role[k];
        const int forward = (at + 1) % n == role[1 - k] ? -1 : 1;
        auto& p = points[k];
        auto& d = distances[k];
        p.push_back(curves[at].pt1());
        d.push_back(0);
        for (int i = 0; i < n; ++i) {
            const auto& c = curves[(at + (forward < 0 ? n - 1 : 0)) % n];
            for (int j = 1; j <= 12; ++j) {
                const auto next = c.pointAt(forward > 0 ? j / 12. : 1 - j / 12.);
                d.push_back(d.back() + QLineF(p.back(), next).length());
                p.push_back(next);
            }
            at = (at + forward + n) % n;
            if (at == role[2] || at == role[3])
                break;
            if (at == role[1 - k])
                return false;
        }
        if (d.back() <= 1e-5)
            return false;
    }
    auto sample = [&](int side, double t) {
        const auto& ds = distances[side];
        const auto& ps = points[side];
        const auto target = t * ds.back();
        const auto i =
            std::clamp(int(std::upper_bound(ds.begin(), ds.end(), target) - ds.begin()), 1, int(ds.size()) - 1);
        return QLineF(ps[i - 1], ps[i]).pointAt((target - ds[i - 1]) / std::max(ds[i] - ds[i - 1], 1e-9));
    };
    double width = std::max(right.length(), left.length());
    if (nominalWidth > 0 && width < nominalWidth) {
        const auto middle = [&](double t) {
            return QLineF(sample(0, t), sample(1, t)).length();
        };
        width = std::max(width, std::min({nominalWidth, middle(.25), middle(.5), middle(.75)}));
    }
    const double shortSide = std::min(distances[0].back(), distances[1].back());
    const double longSide = std::max(distances[0].back(), distances[1].back());
    if (longSide - shortSide > 3.5 * width + .18 * shortSide ||
        (shortSide + longSide) / 2 > 2.2 * QLineF(right.center(), left.center()).length() + 2.8 * width)
        return false;
    for (double f : {.15, .3, .5, .7, .85})
        if (QLineF(sample(0, f), sample(1, f)).length() > 2.7 * width)
            return false;
    return true;
}

// An open or enclosed medial hah has two lips. Across part of the head,
// a page-vertical section must meet both with a visible gap between them.
bool containsHeadLips(const QPainterPath& body, const QJsonObject& lips, QLineF* evidence) {
    const auto box = body.boundingRect();
    const double run = lips.value("minRun").toDouble(), gap = lips.value("minGap").toDouble();
    if (run <= 0 || gap <= 0 || box.width() < run || box.height() <= gap) return false;
    const int samples = std::max(1, int(std::ceil(box.width() / (.25 * run))));
    const double step = box.width() / samples;
    int consecutive = 0, longest = 0;
    std::vector<QLineF> sections;
    if (evidence) sections.resize(samples);
    for (int i = 0; i < samples; ++i) {
        const double x = box.left() + (i + .5) * step;
        auto hits = FindInterections(body, QLineF(x, box.top() - 1, x, box.bottom() + 1), true);
        std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.point.y() < b.point.y(); });
        bool divided = false, seenInk = false;
        double lastBottom = 0, lastMiddle = 0;
        for (size_t j = 1; j < hits.size(); ++j) {
            double top = hits[j - 1].point.y(), bottom = hits[j].point.y();
            if (bottom - top < .01 || !body.contains(QPointF(x, (top + bottom) / 2)))
                continue;
            if (seenInk && top - lastBottom >= gap) {
                divided = true;
                if (evidence) sections[i] = QLineF(x, lastMiddle, x, (top + bottom) / 2);
                break;
            }
            lastBottom = bottom;
            lastMiddle = (top + bottom) / 2;
            seenInk = true;
        }
        consecutive = divided ? consecutive + 1 : 0;
        if (consecutive * step >= run) {
            if (!evidence) return true;
            if (consecutive > longest) {
                longest = consecutive;
                *evidence = sections[i - consecutive / 2];
            }
        }
    }
    return longest > 0;
}

} // namespace MedinaCutter
