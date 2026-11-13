// Exercise the seen detector independently of the neighboring letter rules.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;
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
        check(complete && sub.baseGlyphs.size() == 3 && sub.matchedTemplates.contains("seen-folded-medi"),
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
        check(declaredSharedBoundary(context, context.form), "Final yeh must recognize its own fold");
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

    for (const auto& [page, line, wordNumber, subword] : {std::array{4, 8, 2, 1}, std::array{40, 1, 10, 2}}) {
        QFile pageFile(QString("pages/page%1.dat").arg(page));
        check(pageFile.open(QIODevice::ReadOnly), "Missing initial folded seen fixture");
        PageAnalysisResult analysis;
        QDataStream stream(&pageFile);
        stream >> analysis.page;
        analysis.resolveWordBoundaries(page, &font);
        const auto& word = analysis.page.lines[line - 1].words[wordNumber - 1];
        const auto& sub = word.wordResultInfo.subWords[subword - 1];
        check(analysis.segmentSubword(page - 1, line - 1, wordNumber - 1, subword - 1, &font, true, &config) &&
              sub.baseGlyphs.size() == 2 && sub.joins.size() == 1 &&
              sub.matchedTemplates == QStringList{"seen-folded-init", "yeh-fina"},
              "The folded initial body did not match at the final yeh cut");
        check(sub.templateTrace == QStringList{"seen-folded-init · shared cut at the final fold"},
              "The contextual match entered the ordinary boundary search");
        const auto shape = word.bodyShape(sub);
        WordResultInfo::SubWordInfo dummy;
        auto context = geometry(shape.transform.map(shape.path).translated(shape.pos), dummy);
        const auto yeh = namedLetterForm(config, "yeh-fina");
        check(declaredSharedBoundary(context, yeh), "Final yeh must recognize its own fold");
        const auto& cut = *sub.joins.begin();
        check(sub.kashidaPaths.size() == 1 && sub.kashidaPaths.front().isEmpty() &&
              cut.topBaseLine.point == context.res.topBaseLine.point &&
              cut.bottomBaseLine.point == context.res.bottomBaseLine.point,
              "The contextual match changed the original shared cut");
        checkPartition(shape.path, sub);
    }
}

static void checkFinalSeenLandmarkOwnership(const QJsonObject& config) {
    QFile file("pages/page175.dat");
    check(file.open(QIODevice::ReadOnly), "Missing final seen ownership fixture");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing native font");
    QDataStream fontStream(&fontFile);
    AFont font;
    fontStream >> font;
    analysis.resolveWordBoundaries(175, &font);
    auto& word = analysis.page.lines[3].words[1];
    if (word.wordResultInfo.subWords.empty()) analysis.detectSubWords(174, 3, 1, &font);
    const auto& sub = word.wordResultInfo.subWords.front();
    check(analysis.segmentSubword(174, 3, 1, 0, &font, true, &config),
          "Final seen with a complete tooth cluster and bowl should match");
    check(sub.baseGlyphs.size() == 3 && sub.joins.size() == 2 &&
          !sub.kashidaPaths.back().isEmpty(), "Missing the connector before the final seen");

    // Move the receiving cap past the first tooth. This is a complete
    // partition of the original ink, but the connector now owns that tooth.
    const auto shape = word.bodyShape(sub);
    const auto ink = shape.transform.map(shape.path).translated(shape.pos);
    const auto bounds = ink.boundingRect();
    const double x = bounds.left() + 10.5115;
    auto hits = FindInterections(ink, QLineF(x, bounds.top() - 1, x, bounds.bottom() + 1));
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.point.y() < b.point.y(); });
    check(hits.size() == 2, "Missing the two contour intersections of the invalid receiving cap");
    auto wrong = sub;
    auto last = std::prev(wrong.joins.end());
    auto join = *last;
    join.topLeft = hits[0];
    join.bottomLeft = hits[1];
    join.fixedLeftBoundary = true;
    wrong.joins.erase(last);
    wrong.joins.insert(join);
    auto final = wrong.letterDiagnostics.last().toObject();
    final.insert("template", "seen-fina");
    wrong.letterDiagnostics[2] = final;
    check(assembleBodyPartition(word, wrong, config) && coversOriginalInk(word, wrong) &&
          wrong.kashidaCuts.back().second == QLineF(hits[0].point, hits[1].point),
          "The invalid receiving cap must still partition every source pixel");
    check(!verifyBodyLandmarkOwnership(word, wrong, config) && !wrong.correct &&
          wrong.letterDiagnostics.last().toObject().value("status") == "no-match",
          "A complete partition with a first tooth in the connector was certified");
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

static void checkSeenEntryAndExit(const QJsonObject& config) {
    for (const auto& [p, line, wordIndex, entryJoin, upper] :
            {std::tuple{5, 14, 0, 0, 3}, std::tuple{7, 13, 9, 1, 2}}) {
        QFile file(QString("pages/page%1.dat").arg(p));
        check(file.open(QIODevice::ReadOnly), "Missing seen junction fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        auto& word = page.page.lines[line].words[wordIndex];
        check(page.segmentSubword(p-1, line, wordIndex, 0, nullptr, true, &config), "Seen must segment completely");
        const auto& sub = word.wordResultInfo.subWords[0];
        check(PageAnalysisResult::bodyLetters(sub.text) == (p == 5 ? "فسو" : "نفس") && sub.kashidaCuts.size() == 2 &&
              !sub.kashidaPaths[0].isEmpty() && !sub.kashidaPaths[1].isEmpty(), "Missing the seen connectors");
        const auto shape = word.bodyShape(sub);
        const auto curves = baseContourCurves(shape.transform.map(shape.path).translated(shape.pos));
        const auto entry = sub.kashidaCuts[entryJoin].second;
        check(QLineF(entry.p1(), curves[upper].pt1()).length() < 1e-5 &&
              QLineF(entry.p2(), curves[20].pt1()).length() < 1e-5,
              "The seen entry must meet the first tooth at its two opposing corners");
        if (p == 5)
            check(QLineF(sub.kashidaCuts[1].first.p2(), curves[17].pt1()).length() < 1e-5,
                  "The seen exit must reach the hollow beyond the third tooth");
        checkPartition(shape.path, sub);
    }
}

static void checkSeenJunctionEntries(const QJsonObject& config) {
    struct Example {
        int page, line, word, subword, join;
        double upper, lower;
        bool shared;
    };
    const std::array<Example, 6> examples{{
        {147, 10, 9, 2, 0, 2.847, 26, true},
        {230, 2, 1, 1, 0, 5, 27, false},
        {356, 13, 4, 2, 1, 7.65867, 33, true},
        {444, 10, 3, 1, 1, 7, 39, false},
        {478, 2, 2, 2, 0, 5, 33, true},
        {526, 14, 12, 2, 0, 7, 43, false}
    }};
    for (const auto& e : examples) {
        QFile file(QString("pages/page%1.dat").arg(e.page));
        check(file.open(QIODevice::ReadOnly), "Missing seen junction fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        auto& word = page.page.lines[e.line - 1].words[e.word - 1];
        check(page.segmentSubword(e.page - 1, e.line - 1, e.word - 1, e.subword - 1, nullptr, true, &config),
              "Sheen must segment completely at the junction");
        const auto& sub = word.wordResultInfo.subWords[e.subword - 1];
        check(sub.kashidaPaths.size() == sub.baseGlyphs.size() - 1 && sub.kashidaPaths[e.join].isEmpty() == e.shared,
              "Placing the sheen entry must preserve the type of join");
        const auto shape = word.bodyShape(sub);
        const auto curves = baseContourCurves(shape.transform.map(shape.path).translated(shape.pos));
        const auto pointAt = [&](double t) { return curves[int(t)].pointAt(t - int(t)); };
        const auto join = std::next(sub.joins.begin(), e.join);
        const auto cut = e.shared ? QLineF(join->topBaseLine.point, join->bottomBaseLine.point)
                                  : sub.kashidaCuts[e.join].second;
        const double upperTolerance = e.page == 147 ? .005 : 1e-4;
        check(QLineF(cut.p1(), pointAt(e.upper)).length() < upperTolerance &&
              QLineF(cut.p2(), pointAt(e.lower)).length() < 1e-4,
              "The entry must reach the first tooth from its opposing bend");
        checkPartition(shape.path, sub);
    }
}

static void checkClusterAttachments(const QJsonObject& config) {
    struct Example {
        int page, line, word, subword, seen;
        std::array<double, 3> tips;
        bool connector;
    };
    const std::array<Example, 7> examples{{
        {3, 5, 8, 2, 2, {31.6225, 35.6388, 38.5079}, true},
        {39, 1, 5, 3, 0, {21.5734, 26.6657, 30.7143}, true},
        {104, 6, 4, 1, 2, {6.42374, 11.7121, 17.6433}, true},
        {115, 4, 4, 2, 3, {14.5569, 19.6289, 23.6858}, true},
        {238, 7, 10, 1, 1, {6.4307, 10.7275, 13.6555}, false},
        {244, 11, 9, 1, 1, {4.55777, 8.50743, 12.4836}, true},
        {480, 13, 3, 3, 0, {21.4082, 28.592, 31.6273}, true}
    }};
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing native font");
    QDataStream fontStream(&fontFile);
    AFont font;
    fontStream >> font;
    for (const auto& e : examples) {
        QFile file(QString("pages/page%1.dat").arg(e.page));
        check(file.open(QIODevice::ReadOnly), "Missing third tooth attachment fixture");
        PageAnalysisResult analysis;
        QDataStream stream(&file);
        stream >> analysis.page;
        analysis.resolveWordBoundaries(e.page, &font);
        auto& word = analysis.page.lines[e.line - 1].words[e.word - 1];
        if (word.wordResultInfo.subWords.empty())
            analysis.detectSubWords(e.page - 1, e.line - 1, e.word - 1, &font);
        check(analysis.segmentSubword(e.page - 1, e.line - 1, e.word - 1, e.subword - 1, &font, true, &config),
              "The third tooth attachment should assemble with a complete seen");
        const auto& sub = word.wordResultInfo.subWords[e.subword - 1];
        const auto shape = word.bodyShape(sub);
        const auto ink = shape.transform.map(shape.path);
        const auto body = shape.transform.map(sub.baseGlyphs[e.seen].path);
        const auto curves = baseContourCurves(ink);
        for (double t : e.tips) {
            const auto point = curves[int(t)].pointAt(t - int(t)) + QPointF(0, .065);
            check(ink.contains(point) && body.contains(point), "A tooth escaped the fitted cluster");
        }
        check(sub.kashidaPaths[e.seen].isEmpty() != e.connector, "The exit changed whether the join is shared");
        if (e.page == 3) {
            const auto& [sending, receiving] = sub.kashidaCuts[e.seen];
            const auto origin = ink.boundingRect().topLeft() + shape.pos;
            check(QLineF(sending.p2() - origin, QPointF(18.1471, 9.03938)).length() < .005 &&
                  std::abs(receiving.center().x() - origin.x() - 13.32125) < .005,
                  "The connector must start at the tooth's lower turn and end at the original heh attachment");
        } else if (e.page == 39 || e.page == 480) {
            const auto& [sending, receiving] = sub.kashidaCuts[e.seen];
            const auto hollow = curves[e.page == 39 ? 18 : 17].pt1() + shape.pos;
            check(QLineF(sending.p2(), hollow).length() < .005 && sending.center().x() - receiving.center().x() > 5,
                  "An initial seen must leave the connector at its third tooth's inward turn");
        } else if (e.page == 104) {
            const auto& [sending, receiving] = sub.kashidaCuts[e.seen];
            check(sending.length() < 1.5 && QLineF(sending.p2(), curves[29].pt1() + shape.pos).length() < 1e-5 &&
                  QLineF(receiving.p2(), curves[28].pt1() + shape.pos).length() < 1e-5,
                  "The sheen exit must cross the stroke at its lower hollow and preserve the reh entry");
        } else if (e.page == 115) {
            const auto& [sending, receiving] = sub.kashidaCuts[e.seen];
            check(sending.center().x() - receiving.center().x() > .7,
                  "A hollow close to the receiving cut must leave room for the connector");
        } else if (e.page == 244) {
            const auto& [sending, receiving] = sub.kashidaCuts[e.seen];
            const auto origin = ink.boundingRect().topLeft() + shape.pos;
            check(QLineF(sending.p2() - origin, QPointF(18.9212, 6.86608)).length() < .005 &&
                  std::abs(receiving.center().x() - origin.x() - 8.37916) < .005 &&
                  sending.center().x() - receiving.center().x() > 10,
                  "The third tooth's inward turn must preserve the long connector to reh");
        }
        checkPartition(shape.path, sub);
    }

    QFile file("pages/page189.dat");
    check(file.open(QIODevice::ReadOnly), "Missing pen lift fixture");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    analysis.resolveWordBoundaries(189, &font);
    auto& word = analysis.page.lines[14].words[0];
    if (word.wordResultInfo.subWords.empty()) analysis.detectSubWords(188, 14, 0, &font);
    check(analysis.segmentSubword(188, 14, 0, 2, &font, true, &config), "A medial seen can end at a pen lift");
    const auto& sub = word.wordResultInfo.subWords[2];
    check(sub.letterDiagnostics.at(2).toObject().value("template") == "seen-lifted-exit-medi" &&
          sub.letterDiagnostics.at(3).toObject().value("boundaryKind") == "separate-contours" &&
          coversOriginalInk(word, sub), "The pen lift must retain the two independently matched contours");
    const auto& receiving = *std::next(sub.joins.begin());
    check(std::abs(receiving.topLeft.time - 2) < 1e-6 && std::abs(receiving.bottomLeft.time - 23) < 1e-6,
          "The pen-lift seen must enter at its own junction on the first contour");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    checkFoldedSheen(config);
    checkFinalSeenLandmarkOwnership(config);
    checkClusterAttachments(config);
    checkSeenTailOwnership(config);
    checkSeenEntryAndExit(config);
    checkSeenJunctionEntries(config);
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
            auto body = form.value("body").toObject();
            auto teeth = body.value("cluster").toObject();
            if (!teeth.contains("retryMaxLastPitch")) continue;
            teeth.remove("retryMaxLastPitch");
            body.insert("cluster", teeth); form.insert("body", body);
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
            auto cuts = rule.value("cuts").toObject(), cluster = cuts.value("cluster").toObject();
            cluster.remove("retryBendOffset"); cuts.insert("cluster", cluster); rule.insert("cuts", cuts);
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
        if (value.toObject().value("id") == "seen-medi") settings = seenSettings(value.toObject());
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
    check(sub.kashidaCuts[0].second == edge, "The assembled medial sheen entry must use corners 6 and 38");
    const auto& exit = sub.kashidaCuts[1].first;
    check(QLineF(exit.p2() - source.boundingRect().topLeft(), QPointF(9.481, 12.881)).length() < .005,
          "The medial sheen exit should meet the inward turn of the third tooth");
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
    qInfo("Seen passed: exact medial and final entry junctions, third-tooth hollow, rounded landmarks, independent geometry, recovery, ink conservation and deterministic masks.");
}
