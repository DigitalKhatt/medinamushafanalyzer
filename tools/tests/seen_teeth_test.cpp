// Exercise the seen detector independently of the neighboring letter rules.
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
    check(context.timeSorted.size() >= 3, "Missing source outline");
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

static void checkPartition(QPainterPath local, const WordResultInfo::SubWordInfo& sub) {
    local.setFillRule(Qt::WindingFill);
    const auto box = local.boundingRect();
    const auto original = mask(local, box);
    QVector<QImage> parts;
    for (const auto& body : sub.baseGlyphs) parts.append(mask(body.path, box));
    for (const auto& connection : sub.kashidaPaths) if (!connection.isEmpty()) parts.append(mask(connection, box));
    int ink = 0, missing = 0, extra = 0, overlap = 0;
    for (int y = 0; y < original.height(); ++y) for (int x = 0; x < original.width(); ++x) {
        const bool present = qRed(original.pixel(x,y)) != 0;
        int count = 0;
        for (const auto& part : parts) count += qRed(part.pixel(x,y)) != 0;
        ink += present; missing += present && count == 0; extra += !present && count != 0; overlap += count > 1;
    }
    const int tolerance = std::max(8, ink / 500);
    check(missing <= tolerance && extra <= tolerance && overlap <= tolerance,
          "Seen bodies and kashidas do not partition the source ink");
}

static void checkFoldedSheen(const QJsonObject& config) {
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing native font");
    QDataStream fontStream(&fontFile);
    AFont font;
    fontStream >> font;
    PageAnalysisResult::initQuranText();
    for (const auto& [page, line, wordNumber] : {std::tuple{82, 12, 8}, std::tuple{519, 14, 2}, std::tuple{419, 2, 2}}) {
        QFile pageFile(QString("pages/page%1.dat").arg(page));
        check(pageFile.open(QIODevice::ReadOnly), "Missing folded sheen fixture");
        PageAnalysisResult analysis;
        QDataStream stream(&pageFile);
        stream >> analysis.page;
        analysis.resolveWordBoundaries(page, &font);
        auto& word = analysis.page.lines[line - 1].words[wordNumber - 1];
        if (word.wordResultInfo.subWords.empty()) analysis.detectSubWords(page - 1, line - 1, wordNumber - 1, &font);
        const int subwordIndex = page == 419 ? 1 : 0;
        const auto& sub = word.wordResultInfo.subWords[subwordIndex];
        const bool complete = analysis.segmentSubword(page - 1, line - 1, wordNumber - 1, subwordIndex, &font, true, &config);
        if (page == 419) {
            // A complete عيسى would need the tall tooth at 4.49 in its medial ي.
            // The earlier tooth candidate currently claims the seen's first tip.
            if (complete) {
                const auto shape = word.bodyShape(sub);
                check(sub.baseGlyphs.size() == 4, "Missing letter mask in عيسى");
                const auto ink = shape.transform.map(shape.path);
                const auto yeh = shape.transform.map(sub.baseGlyphs[1].path);
                check(yeh.contains(baseContourCurves(ink)[4].pointAt(.49) + QPointF(0, .08)),
                      "A shared entry certified a medial yeh without its own tooth");
            } else check(sub.templateTrace.contains("seen-folded-medi · shared entry would take a previously matched tooth"),
                         "Missing diagnostic for conflicting tooth ownership");
            continue;
        }
        check(complete &&
              !sub.provisional() && sub.baseGlyphs.size() == 3 && sub.matchedTemplates.contains("seen-folded-medi"),
              "Folded sheen before yeh did not produce three matched letter bodies");
        check(sub.joins.size() == 2 && sub.kashidaPaths.size() == 2 &&
              !sub.kashidaPaths.front().isEmpty() && sub.kashidaPaths.back().isEmpty(),
              "Only the incoming khah–sheen join has connecting ink");
        check(!sub.templateTrace.contains("seen-folded-medi · shared incoming ligature cut"),
              "The trace calls a genuine incoming connector a shared cut");
        const auto shape = word.bodyShape(sub);
        checkPartition(shape.path, sub);
        const auto mapped = [&](const QPainterPath& local) {
            auto ink = shape.transform.map(local);
            ink.translate(shape.pos);
            ink.setFillRule(Qt::WindingFill);
            return ink;
        };
        const auto ink = mapped(shape.path);
        WordResultInfo::SubWordInfo dummy;
        auto context = geometry(ink, dummy);
        for (const auto& item : config.value("final").toArray())
            if (item.toObject().value("id") == "yeh-fina") context.form = item.toObject();
        check(searchContourWalk(context), "Final yeh must recognize its own fold");
        const auto& fold = *sub.joins.rbegin();
        check(fold.topBaseLine.time == context.res.topBaseLine.time &&
              fold.bottomBaseLine.time == context.res.bottomBaseLine.time, "The two letters do not share the yeh cut");
        const auto pieces = cutRegions(ink, {}, {context.res}, context.orientation, true);
        check(pieces.size() == 2 && mask(pieces.back().path, context.bbox) ==
              mask(mapped(sub.baseGlyphs.back().path), context.bbox), "Yeh lost ink from its independently cut bowl");
        if (page == 82) {
            check(std::abs(fold.topBaseLine.time - 14.99424) < 0.0001 &&
                  std::abs(fold.bottomBaseLine.time - 27.65054) < 0.0001, "Page 82 shared fold moved");
            const auto sheen = mapped(sub.baseGlyphs[1].path);
            const auto curves = baseContourCurves(ink);
            for (double time : {5.14456, 10.61850, 14.02248})
                check(sheen.contains(curves[int(time)].pointAt(time - int(time)) + QPointF(0, .08)),
                      "A sheen tip escaped its body");
            check(sheen.contains((curves[14].pointAt(.80) + curves[27].pointAt(.84)) / 2),
                  "The descending stroke escaped the sheen body");
            const auto arch = std::find_if(context.timeSorted.begin(), context.timeSorted.end(),
                [](const auto& p) { return std::abs(p.time - 14.02248) < 0.0001; });
            check(arch != context.timeSorted.end() && seenTipMeetsFold(context, *arch, true, fold) &&
                  !seenTipMeetsFold(context, *arch, false, fold) &&
                  !seenTipMeetsFold(context, *arch, true, *sub.joins.begin()), "Unrelated fold accepted");
        }
    }
}

static void checkSeenTailOwnership(const QJsonObject& config) {
    struct Example {
        int page, line, word, subword;
        QString letters;
        std::vector<std::pair<int, double>> landmarks; // body index and source contour time
    };
    const std::array<Example, 5> examples{{
        {105, 8, 6, 1, "فسيحشر", {{1,7.54}, {1,12.58}, {1,15.63}, {2,19.48}, {3,30},
                                  {4,31.66}, {4,36.56}, {4,40.52}}},
        {107, 15, 9, 2, "لخسر", {{0,37.46}, {1,7}, {2,8.69}, {2,12.56}, {2,15.85}}},
        {446, 14, 2, 2, "حشر", {{0,5}, {1,7.58}, {1,12.54}, {1,16.71}}},
        {497, 12, 1, 1, "بمنشر", {{1,48.49}, {2,53.13}, {3,4.54}, {3,9.51}, {3,12.66}}},
        {595, 15, 2, 1, "لليسر", {{0,60.16}, {1,7.58}, {2,14.16}, {3,20.54}, {3,25.54}, {3,29.71}}}
    }};
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing native font");
    QDataStream fontStream(&fontFile);
    AFont font;
    fontStream >> font;
    for (const auto& e : examples) {
        QFile file(QString("pages/page%1.dat").arg(e.page));
        check(file.open(QIODevice::ReadOnly), "Missing seen–reh fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(e.page, &font);
        auto& word = page.page.lines[e.line-1].words[e.word-1];
        if (word.wordResultInfo.subWords.empty()) page.detectSubWords(e.page-1, e.line-1, e.word-1, &font);
        check(page.segmentSubword(e.page-1, e.line-1, e.word-1, e.subword-1, &font, true, &config),
              "An earlier cut blocked a three-tooth seen and its final reh");
        const auto& sub = word.wordResultInfo.subWords[e.subword-1];
        check(PageAnalysisResult::bodyLetters(sub.text) == e.letters && sub.baseGlyphs.size() == e.letters.size(),
              "Incorrect letter count in seen–reh fixture");
        const auto shape = word.bodyShape(sub);
        const auto ink = shape.transform.map(shape.path);
        const auto curves = baseContourCurves(ink);
        for (const auto& [letter,time] : e.landmarks) {
            const auto point = curves[int(time)].pointAt(time - int(time));
            const auto body = shape.transform.map(sub.baseGlyphs[letter].path);
            bool sampled = false;
            for (int i = 0; i < 32; ++i) {
                const double angle = i * 2 * std::acos(-1.) / 32;
                const auto p = point + .08 * QPointF(std::cos(angle), std::sin(angle));
                if (!ink.contains(p)) continue;
                sampled = true;
                if (!body.contains(p)) qFatal("Page %d: landmark %.2f escaped body %d", e.page, time, letter+1);
            }
            check(sampled, "No source ink at expected landmark");
        }
        check(!sub.kashidaPaths.back().isEmpty(), "Connector before final reh was absorbed into a letter");
        check(std::abs(shape.transform.map(sub.baseGlyphs.back().path).boundingRect().left() -
                       ink.boundingRect().left()) < 1e-5, "Reh lost its tail");
        if (e.page == 107) {
            check(sub.kashidaPaths.front().isEmpty() && sub.matchedTemplates.contains("hah-open-mouth-medi"),
                  "Stacked lam and khah must meet at their common attachment");
            const auto& join = *sub.joins.begin();
            check(std::abs(join.topBaseLine.time-3) < .01 && std::abs(join.bottomBaseLine.time-32.2273) < .01,
                  "The stacked attachment moved away from the upper lip");
        }
        checkPartition(shape.path, sub);
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    checkFoldedSheen(config);
    checkSeenTailOwnership(config);
    {
        QFile file("pages/page230.dat");
        check(file.open(QIODevice::ReadOnly), "Missing upright beh and wide sheen fixture");
        QDataStream stream(&file);
        PageAnalysisResult sample;
        stream >> sample.page;
        auto& word = sample.page.lines[3].words[4];
        const auto& sub = word.wordResultInfo.subWords[1];
        auto narrow = config;
        // Isolate the spacing rule from the corpus-wide contour retries.
        narrow.insert("retryUnresolvedForms", false);
        auto medial = narrow.value("medial").toArray();
        for (int i = 0; i < medial.size(); ++i) {
            auto form = medial[i].toObject();
            auto teeth = form.value("seenTeeth").toObject();
            if (!teeth.contains("retryMaxLastPitch")) continue;
            teeth.remove("retryMaxLastPitch");
            form.insert("seenTeeth", teeth);
            medial[i] = form;
        }
        narrow.insert("medial", medial);
        check(!sample.segmentSubword(229,3,4,1,nullptr,true,&narrow) &&
              sub.letterDiagnostics[2].toObject().value("status") == "no-match", "Missing narrow-spacing control");
        check(sample.segmentSubword(229,3,4,1,nullptr,true) && sub.baseGlyphs.size() == 4 &&
              sub.templateTrace.contains("seen-compact-medi · wider spacing before the third tooth"),
              "The wider final spacing in لْبُشْرَ should be recoverable");
        const auto shape = word.bodyShape(sub);
        checkPartition(shape.path, sub);
        const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
        const auto curves = baseContourCurves(mapped(shape.path));
        const auto beh = mapped(sub.baseGlyphs[1].path), sheen = mapped(sub.baseGlyphs[2].path);
        for (double t : {7.10625, 11.54508, 16.54475, 19.95003}) {
            const auto p = curves[int(t)].pointAt(t-int(t)) + QPointF(0,.08);
            check((t < 8 ? beh : sheen).contains(p) && !(t < 8 ? sheen : beh).contains(p),
                  "The upright beh and the three sheen tips need separate owners");
        }
    }
    {
        QFile file("pages/page315.dat");
        check(file.open(QIODevice::ReadOnly), "Missing steep sheen–reh fixture");
        QDataStream stream(&file);
        PageAnalysisResult sample;
        stream >> sample.page;
        auto& word = sample.page.lines[8].words[7];
        const auto& sub = word.wordResultInfo.subWords[0];
        check(sample.segmentSubword(314,8,7,0,nullptr,true) && sub.baseGlyphs.size() == 4 &&
              sub.templateTrace.contains("seen-compact-medi · exit follows the contour from the third-tooth bend"),
              "The steep connector must allow a complete يُحْشَرَ");
        const auto shape = word.bodyShape(sub);
        checkPartition(shape.path, sub);
        auto mapped = [&](const QPainterPath& path) {
            auto ink = shape.transform.map(path);
            ink.translate(shape.pos);
            return ink;
        };
        const auto ink = mapped(shape.path);
        const auto curves = baseContourCurves(ink);
        const auto sheen = mapped(sub.baseGlyphs[2].path);
        for (double t : {7.56932, 12.52378, 16.72187})
            check(sheen.contains(curves[int(t)].pointAt(t - int(t)) + QPointF(0,.08)),
                  "One of the three sheen tips escaped its letter");
        check(sub.kashidaCuts.size() == 3 && !sub.kashidaPaths.back().isEmpty(),
              "The sheen and reh need two independent boundaries");
        const auto [exit, entry] = sub.kashidaCuts.back();
        const auto& join = *sub.joins.rbegin();
        const double t = join.bottomBaseLine.time - 27;
        check(t > 0 && t < 1 && QLineF(curves[27].pointAt(t), exit.p2()).length() < 1e-5,
              "The shifted exit left the branch between corners 28 and 27");
        const auto originalBend = curves[28].pointAt(0);
        const double stroke = QLineF(originalBend, curves[17].pointAt(.44083418)).length();
        const auto tangent = curves[27].pointAt(t+.0001) - curves[27].pointAt(t-.0001);
        check(std::abs(curves[27].getSubRange(t,1).length(.001) / stroke - .45) < .001 &&
              std::abs(QPointF::dotProduct(exit.unitVector().p2()-exit.p1(), tangent) / std::hypot(tangent.x(),tangent.y())) < .001,
              "The exit should be normal to the branch, 0.45 local widths after its bend");
        check(QLineF(entry.p2(), curves[27].pointAt(0)).length() < 1e-5 &&
              std::abs(entry.dx()) < 1e-5 &&
              std::abs(mapped(sub.baseGlyphs.back().path).boundingRect().left() - ink.boundingRect().left()) < 1e-5,
              "The reh should own its whole tail from corner 27");
    }
    for (int page : {109, 116}) {
        QFile file(QString("pages/page%1.dat").arg(page));
        check(file.open(QIODevice::ReadOnly), "Missing seen-reh fixture");
        QDataStream stream(&file);
        PageAnalysisResult sample;
        stream >> sample.page;
        const int line = page == 109 ? 4 : 7, part = page == 109 ? 1 : 0;
        auto& word = sample.page.lines[line].words[7];
        auto& sub = word.wordResultInfo.subWords[part];
        auto noBend = config;
        noBend.insert("retryUnresolvedForms", false);
        auto initial = noBend.value("initial").toArray();
        for (int i = 0; i < initial.size(); ++i) {
            auto rule = initial[i].toObject();
            rule.remove("retryBendOffset");
            initial[i] = rule;
        }
        noBend.insert("initial", initial);
        check(!sample.segmentSubword(page-1, line, 7, part, nullptr, true, &noBend), "Lost the failing seen-reh control");
        check(sample.segmentSubword(page-1, line, 7, part, nullptr, true) &&
              sub.templateTrace.contains("seen-init · exit at the bend after the third tooth") &&
              sub.matchedTemplates.contains("reh-fina") && sub.baseGlyphs.size() == 2 &&
              sub.kashidaPaths.size() == 1 && !sub.kashidaPaths.front().isEmpty(), "Sloping seen-reh connector was lost");
        const auto shape = word.bodyShape(sub);
        checkPartition(shape.path, sub);
        const auto ends = sub.kashidaCuts.front();
        if (page == 116) {
            check(QLineF(ends.first.p1(), QPointF(150.14234, 276.00495)).length() < .001 &&
                  QLineF(ends.first.p2(), QPointF(150.61462, 277.24379)).length() < .001 &&
                  QLineF(ends.second.p2(), QPointF(148.30930, 278.07450)).length() < .001,
                  "Wrong third-tooth exit or reh corner");
        }
    }
    {
        QFile file("pages/page12.dat");
        check(file.open(QIODevice::ReadOnly), "Missing medial seen with terminal reh fixture");
        PageAnalysisResult sample;
        QDataStream stream(&file);
        stream >> sample.page;
        auto& word = sample.page.lines[0].words[6];
        auto& sub = word.wordResultInfo.subWords[0];
        const auto shape = word.bodyShape(sub);
        const auto tailGap = [&] {
            const auto last = shape.transform.map(sub.baseGlyphs.back().path).boundingRect();
            return last.left() - shape.transform.map(shape.path).boundingRect().left();
        };
        auto withoutGuard = config;
        auto final = config.value("final").toArray();
        for (int i = 0; i < final.size(); ++i) {
            auto rule = final[i].toObject();
            rule.remove("ownsLeftTip");
            final[i] = rule;
        }
        withoutGuard.insert("final", final);
        check(sample.segmentSubword(11,0,6,0,nullptr,true,&withoutGuard) && tailGap() > 5,
              "The unguarded reference should reproduce the false complete sliver");
        check(sample.segmentSubword(11,0,6,0,nullptr,true) && std::abs(tailGap()) < 1e-5 &&
              sub.baseGlyphs.size() == 3 &&
              sub.letterDiagnostics[1].toObject().value("template").toString().startsWith("seen-") &&
              sub.letterDiagnostics[2].toObject().value("template") == "reh-fina",
              "Medial seen took the tail away from the final reh");
        checkPartition(shape.path, sub);
    }
    QJsonObject settings;
    for (const auto& value : config.value("medial").toArray())
        if (value.toObject().value("id") == "seen-medi") settings = value.toObject().value("seenTeeth").toObject();
    check(!settings.isEmpty(), "Missing seen-medi settings");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 fixture is required");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    check(stream.status() == QDataStream::Ok, "Invalid page 600 fixture");
    auto& word = analysis.page.lines[1].words[1];
    auto& sub = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[sub.paths.front()];
    auto source = shape.transform.map(shape.path);
    source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto context = geometry(source, dummy);
    Stretching right{}, left{};
    check(seenTeethBoundaries(context, settings, context.bbox.left(), context.bbox.right(), &right, &left),
          "Three-tooth sheen was not detected independently");
    check(right.topBaseLine.time > 4 && right.topBaseLine.time < 6 &&
          left.topBaseLine.time > 16.55 && left.topBaseLine.time < 20,
          "The sheen boundaries do not enclose its real three teeth");
    const auto firstTip = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [](const auto& point) {
        return std::abs(point.time - 7.550269530030249) < 1e-5;
    });
    check(firstTip != context.timeSorted.end(), "Missing first sheen tip");
    const auto entry = seenEntryCornerPair(context, *firstTip, false, 3);
    check(entry && entry->topBaseLine.time == 6 && entry->bottomBaseLine.time == 38,
          "Seen entry did not use its own opposing corners 6 and 38");
    const QLineF edge(entry->topBaseLine.point, entry->bottomBaseLine.point);
    const QLineF moved(right.topBaseLine.point, right.bottomBaseLine.point);
    check(std::abs(right.referenceWidth - edge.length()) < 1e-7 &&
          std::abs(moved.center().x() - edge.center().x() - .5 * edge.length()) < .01 * edge.length(),
          "Right entry did not move half the corner-pair width toward the incoming join");
    const auto curves = baseContourCurves(source);
    const auto tangentAt = [&](const PathExtrema& point) {
        const int index = int(std::floor(point.time));
        const double t = point.time - index, u = 1-t;
        const auto& curve = curves[index];
        auto tangent = u*u*(curve.pt2()-curve.pt1()) + 2*u*t*(curve.pt3()-curve.pt2()) +
            t*t*(curve.pt4()-curve.pt3());
        tangent /= std::hypot(tangent.x(),tangent.y());
        return tangent;
    };
    auto a = tangentAt(right.topBaseLine), b = tangentAt(right.bottomBaseLine);
    if (QPointF::dotProduct(a,b) < 0) b = -b;
    const auto normal = right.bottomBaseLine.point - right.topBaseLine.point;
    check(std::abs(QPointF::dotProduct(normal,a+b)) < 1e-5*moved.length(),
          "Shifted seen entry is not normal to the local stroke");
    const auto same = [](const Stretching& expected, const Stretching& actual, const QTransform& mapping = {}) {
        return QLineF(mapping.map(expected.topBaseLine.point), actual.topBaseLine.point).length() < 1e-4 &&
            QLineF(mapping.map(expected.bottomBaseLine.point), actual.bottomBaseLine.point).length() < 1e-4;
    };
    Stretching unrelated{};
    unrelated.topBaseLine.point = {-100, 200};
    unrelated.bottomBaseLine.point = {-80, 400};
    dummy.joins.insert(unrelated);
    context.baseText = QStringLiteral("اختبار");
    context.pairedStrokeWidth = 1000;
    Stretching independentRight{}, independentLeft{};
    check(seenTeethBoundaries(context, settings, context.bbox.left(), context.bbox.right(), &independentRight, &independentLeft) &&
          same(right, independentRight) && same(left, independentLeft), "Seen geometry depends on neighboring cuts/text");
    // Existing length settings are in page units; scale them with the artwork.
    // The new tip-spread ratio remains unchanged under this unit conversion.
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy);
        auto scaledSettings = settings;
        for (const auto* key : {"minToothRise", "minPitch", "maxPitch"})
            scaledSettings.insert(key, settings.value(key).toDouble() * scale);
        scaledSettings.insert("exitDrop", settings.value("exitDrop").toDouble(.05) * scale);
        Stretching r{}, l{};
        check(seenTeethBoundaries(scaled, scaledSettings, scaled.bbox.left(), scaled.bbox.right(), &r, &l) &&
              same(right, r, mapping) && same(left, l, mapping), "Seen ratio changed under translation or page-unit scaling");
    }
    check(analysis.segmentSubword(599, 1, 1, 0, nullptr, true), "Lam-sheen-dal remains partial");
    check(sub.baseGlyphs.size() == 3 && sub.matchedTemplates.contains("seen-medi") &&
          sub.matchedTemplates.contains("dal-fina"), "Sheen did not unblock the final dal");
    auto sheen = shape.transform.map(sub.baseGlyphs[1].path);
    sheen.translate(shape.pos);
    for (double time : {7.550269530030249, 12.512993061909023, 16.548516799590644}) {
        const auto tip = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [&](const auto& point) {
            return std::abs(point.time - time) < 1e-5;
        });
        check(tip != context.timeSorted.end() && sheen.contains(tip->point + QPointF(0, .05)),
              "One of the three sheen tips was assigned to another region");
    }
    check(sheen.boundingRect().top() > source.boundingRect().top() + 5,
          "The sheen body includes the tall lam ascender");
    checkPartition(shape.path, sub);
    const auto bodies = sub.baseGlyphs;
    const auto kashidas = sub.kashidaPaths;
    check(analysis.segmentSubword(599, 1, 1, 0, nullptr, true) && sub.kashidaPaths == kashidas,
          "Repeat segmentation changed the result");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(sub.baseGlyphs[i].path == bodies[i].path, "Repeat segmentation changed a body mask");

    // Page 600, line 1: the old exit projection crossed a heh counter and
    // accepted two sheen teeth plus that heh's upper loop instead.
    auto& loopWord = analysis.page.lines[0].words[8];
    auto& loopSub = loopWord.wordResultInfo.subWords[0];
    const auto& loopShape = loopWord.paths[loopSub.paths.front()];
    auto loopSource = loopShape.transform.map(loopShape.path);
    loopSource.translate(loopShape.pos);
    WordResultInfo::SubWordInfo loopDummy;
    auto loopContext = geometry(loopSource, loopDummy);
    Stretching loopRight{}, loopLeft{};
    check(seenTeethBoundaries(loopContext, settings, loopContext.bbox.left(), loopContext.bbox.right(), &loopRight, &loopLeft),
          "Sheen before a heh loop was not detected independently");
    check(loopRight.topBaseLine.time < 8.53983 &&
          loopLeft.topBaseLine.time > 17.5452 && loopLeft.topBaseLine.time < 19.9935 &&
          loopLeft.bottomBaseLine.time == 58,
          "Sheen exit was not anchored at its own lower bend before the heh loop");
    const auto loopCurves = baseContourCurves(loopSource);
    const auto exitTangent = loopCurves[57].pt3() - loopCurves[57].pt4();
    const auto exitNormal = loopLeft.topBaseLine.point - loopLeft.bottomBaseLine.point;
    check(exitTangent.x() < 0 && std::abs(QPointF::dotProduct(exitTangent, exitNormal)) < 1e-6,
          "Sheen bend cut is not normal to its outgoing branch");
    auto verticalOnly = settings;
    verticalOnly.remove("maxExitBendDistance");
    Stretching noRight{}, noLeft{};
    check(!seenTeethBoundaries(loopContext, verticalOnly, loopContext.bbox.left(), loopContext.bbox.right(), &noRight, &noLeft),
          "Closed heh loop was accepted as a third seen tooth when the correct exit was unavailable");
    loopDummy.joins.insert(unrelated);
    loopContext.baseText = QStringLiteral("اختبار");
    loopContext.pairedStrokeWidth = 1000;
    Stretching isolatedRight{}, isolatedLeft{};
    check(seenTeethBoundaries(loopContext, settings, loopContext.bbox.left(), loopContext.bbox.right(), &isolatedRight, &isolatedLeft) &&
          same(loopRight, isolatedRight) && same(loopLeft, isolatedLeft),
          "Seen exit bend depends on the heh's text, landmarks, or joins");
    for (double scale : {0.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(loopSource), loopDummy);
        auto scaledSettings = settings;
        for (const auto* key : {"minToothRise", "minPitch", "maxPitch"})
            scaledSettings.insert(key, settings.value(key).toDouble() * scale);
        scaledSettings.insert("exitDrop", settings.value("exitDrop").toDouble(.05) * scale);
        Stretching r{}, l{};
        check(seenTeethBoundaries(scaled, scaledSettings, scaled.bbox.left(), scaled.bbox.right(), &r, &l) &&
              same(loopRight, r, mapping) && same(loopLeft, l, mapping),
              "Seen exit bend changed under translation or page-unit scaling");
    }
    check(analysis.segmentSubword(599, 0, 8, 0, nullptr, true), "Lam-sheen-heh-yeh-dal remains partial");
    check(loopSub.baseGlyphs.size() == 5 && loopSub.matchedTemplates.contains("seen-medi") &&
          loopSub.matchedTemplates.contains("heh-body-medi") && loopSub.matchedTemplates.contains("beh-tooth-medi") &&
          loopSub.matchedTemplates.contains("dal-fina"), "Correct sheen did not unblock the heh, yeh and dal");
    auto loopSheen = loopShape.transform.map(loopSub.baseGlyphs[1].path);
    loopSheen.translate(loopShape.pos);
    for (double time : {8.53983054, 13.52758811, 17.54524565}) {
        const auto tip = std::find_if(loopContext.timeSorted.begin(), loopContext.timeSorted.end(), [&](const auto& point) {
            return std::abs(point.time - time) < 1e-5;
        });
        check(tip != loopContext.timeSorted.end() && loopSheen.contains(tip->point + QPointF(0, .05)),
              "A real sheen tooth was assigned to a kashida or another letter");
    }
    check(toSubpaths(loopSub.baseGlyphs[1].path).size() == 1 &&
          toSubpaths(loopSub.baseGlyphs[2].path).size() == 3,
          "Sheen swallowed a heh counter or the heh lost one of its two counters");
    checkPartition(loopShape.path, loopSub);
    const auto loopBodies = loopSub.baseGlyphs;
    const auto loopKashidas = loopSub.kashidaPaths;
    check(analysis.segmentSubword(599, 0, 8, 0, nullptr, true) && loopSub.kashidaPaths == loopKashidas,
          "Repeat seen/heh segmentation changed the kashidas");
    for (size_t i = 0; i < loopBodies.size(); ++i)
        check(loopSub.baseGlyphs[i].path == loopBodies[i].path, "Repeat seen/heh segmentation changed a body mask");

    QFile roundedFile("pages/page3.dat");
    check(roundedFile.open(QIODevice::ReadOnly), "Page 3 rounded seen fixture is required");
    PageAnalysisResult roundedAnalysis;
    QDataStream roundedStream(&roundedFile);
    roundedStream >> roundedAnalysis.page;
    auto& roundedWord = roundedAnalysis.page.lines[12].words[3];
    auto& roundedSub = roundedWord.wordResultInfo.subWords[0];
    const auto& roundedShape = roundedWord.paths[roundedSub.paths.front()];
    auto roundedInk = roundedShape.transform.map(roundedShape.path);
    roundedInk.translate(roundedShape.pos);
    auto roundedContext = geometry(roundedInk, dummy);
    const auto roundedTip = std::find_if(roundedContext.timeSorted.begin(), roundedContext.timeSorted.end(), [](const auto& point) {
        return std::abs(point.time - 29.6004) < .001;
    });
    check(roundedTip != roundedContext.timeSorted.end(), "Missing rounded seen tip");
    const auto roundedEntry = seenEntryCornerPair(roundedContext, *roundedTip, false, 3);
    check(roundedEntry && std::abs(roundedEntry->topBaseLine.time - 28.8386) < .001 &&
          std::abs(roundedEntry->bottomBaseLine.time - 20.8454) < .001,
          "Rounded shoulder and lower bend were not accepted as equivalent landmarks");
    qInfo("Seen passed: corner pair 6/38, half-stroke right offset, stroke normal, rounded entry landmarks, ascender and loop rejection, own exit bend, all three teeth retained, independent geometry, translation/page-unit scaling, heh/yeh/dal recovery, ink conservation and deterministic masks.");
}
