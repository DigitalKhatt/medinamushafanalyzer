// Cached candidates must equal a fresh search after every relevant input edit.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static bool samePartition(const WordResultInfo::SubWordInfo& a, const WordResultInfo::SubWordInfo& b) {
    const auto count = a.baseGlyphs.size();
    if (!a.correct || !b.correct || a.paths != b.paths || a.secondaryBodyPath != b.secondaryBodyPath ||
        count != b.baseGlyphs.size() || a.letterDiagnostics.size() != int(count) ||
        b.letterDiagnostics.size() != int(count) || a.kashidaPaths.size() != a.joins.size() || a.kashidaPaths != b.kashidaPaths ||
            a.kashidaCuts != b.kashidaCuts || a.joins.size() != b.joins.size()) return false;
    for (size_t slot = 0; slot < count; ++slot) {
        const auto& first = a.baseGlyphs[slot].path;
        const auto& second = b.baseGlyphs[slot].path;
        if (first != second || first.fillRule() != second.fillRule()) return false;
        const auto one = a.letterDiagnostics.at(int(slot)).toObject(), two = b.letterDiagnostics.at(int(slot)).toObject();
        if (one.value("template") != two.value("template") || one.value("variant") != two.value("variant")) return false;
    }
    auto second = b.joins.begin();
    size_t join = 0;
    for (const auto& first : a.joins) {
        if (a.kashidaPaths[join++].isEmpty() && first.cutSegments() != second->cutSegments()) return false;
        ++second;
    }
    return true;
}

static void appendCandidateBoundary(QByteArray& key, const Stretching& cut) {
    for (const auto* p : {&cut.topBaseLine, &cut.bottomBaseLine, &cut.topLeft, &cut.topRight,
                          &cut.bottomLeft, &cut.bottomRight, &cut.counterEntry, &cut.counterExit}) {
        appendGeometryAnchor(key, *p);
        appendGeometryKey(key, p->isX); appendGeometryKey(key, p->isY); appendGeometryKey(key, p->isSharp);
    }
    appendGeometryKey(key, cut.length);
    appendGeometryKey(key, cut.clockWise);
    appendGeometryKey(key, cut.fixedRightBoundary);
    appendGeometryKey(key, cut.fixedLeftBoundary);
    appendGeometryKey(key, cut.topEndpointTolerance);
    appendGeometryKey(key, cut.referenceWidth);
    appendGeometryKey(key, cut.readingOrder);
    appendGeometryPoint(key, cut.counterBendWeights);
    const auto settings = QJsonDocument(cut.kashidaOverrides).toJson(QJsonDocument::Compact);
    appendGeometryKey(key, settings.size());
    key.append(settings);
}

static QByteArray snapshot(const std::vector<ToothBody>& candidates) {
    QByteArray bytes;
    for (const auto& c : candidates) {
        appendGeometryAnchor(bytes, c.tip); appendGeometryAnchor(bytes, c.bend);
        appendCandidateBoundary(bytes, c.left);
        appendGeometryKey(bytes, bool(c.right));
        if (c.right) appendCandidateBoundary(bytes, *c.right);
        appendGeometryKey(bytes, c.curvedAxis.size());
        for (const auto& p : c.curvedAxis) appendGeometryPoint(bytes, p);
    }
    return bytes;
}

static void checkTemplatePriority(const QJsonObject& config, AFont& font) {
    struct Fixture {
        int page, line, word, subword;
        QStringList forms;
        int join = -1;
        bool hasConnector = false;
    };
    const Fixture fixtures[] = {
        {29, 14, 6, 1, {"seen-init", "beh-upright-medi", "beh-tooth-medi", "lam-bowl-fina"}},
        {416, 7, 8, 2, {"seen-init", "beh-tooth-medi", "hah-flat-medi", "waw-fina"}},
        {411, 7, 9, 3, {"lam-init", "beh-bent-medi", "kaf-bowl-fina"}},
        {6, 6, 10, 2, {"lam-hah-stacked-init", "hah-returning-lips-medi", "kaf-zigzag-medi", "beh-tooth-medi",
                       "meem-counter-fina"}, 0},
        {293, 7, 2, 2, {"lam-hah-stacked-init", "hah-returning-lips-medi", "seen-medi", "beh-tooth-medi",
                        "yeh-fina"}, 0},
        {527, 14, 3, 2, {"lam-hah-stacked-init", "hah-returning-lips-medi", "reh-fina"}, 0},
        {92, 6, 7, 3, {"heh-init", "meem-upper-neck-fina"}, 0},
        {103, 6, 3, 1, {"lam-init", "feh-descending-rim-medi", "yeh-first-turn-fina"}, 1},
        {161, 4, 7, 2, {"lam-init", "meem-medi", "hah-returning-lips-medi", "reh-fina"}, 2, true},
        {71, 2, 10, 1, {"hah-returning-lip-init", "waw-fina"}, 0, true},
        {406, 14, 1, 1, {"hah-open-init", "waw-fina"}, 0},
    };
    for (const auto& f : fixtures) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing template priority fixture");
        PageAnalysisResult analysis;
        QDataStream stream(&file);
        stream >> analysis.page;
        analysis.resolveWordBoundaries(f.page, &font);
        auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        if (word.wordResultInfo.subWords.empty())
            analysis.detectSubWords(f.page - 1, f.line - 1, f.word - 1, &font);
        check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Template priority lost a complete partition");
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        QStringList selected;
        for (const auto& letter : sub.letterDiagnostics) selected.append(letter.toObject().value("template").toString());
        check(selected == f.forms, "Template priority changed the selected body forms");
        if (f.join >= 0)
            check(sub.kashidaPaths.size() > size_t(f.join) &&
                  (!sub.kashidaPaths[f.join].isEmpty()) == f.hasConnector,
                  "Template priority changed the connector at the checked join");
        if (f.page == 406) {
            const auto& seam = *sub.joins.begin();
            check(seam.topBaseLine.time == 5 && seam.bottomBaseLine.time == 14 &&
                  sub.letterDiagnostics.last().toObject().value("entryPolicy") == "counter-bridge",
                  "The shared wāw cut moved away from its fixed counter bridge");
        }
    }
}

static void checkFixedAnchors(const QJsonObject& config) {
    AnchorForms anchors;
    check(compileAnchorForms(config, anchors).isEmpty(), "Valid fixed anchor templates were rejected");
    const auto& finalMeem = anchors[2].value(QChar(0x0645));
    check(!finalMeem.empty() && finalMeem.front().value("id") == "meem-counter-fina",
          "Fixed anchor forms lost final meem or its template order");
    auto invalid = config;
    auto forms = invalid.value("initial").toArray();
    auto form = forms.first().toObject();
    form.insert("fixedAnchors", "true");
    forms[0] = form;
    invalid.insert("initial", forms);
    check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Fixed anchors accepted a nonboolean selector");

    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing fixed anchor fixture font");
    AFont font;
    QDataStream fontStream(&fontFile); fontStream >> font;
    PageAnalysisResult::initQuranText();

    struct Fixture { int page, line, word, subword; };
    for (const auto& f : {Fixture{2, 7, 1, 3}, Fixture{2, 6, 1, 2}, Fixture{151, 3, 5, 2},
                          Fixture{167, 15, 5, 2}, Fixture{411, 7, 9, 3}, Fixture{116, 4, 9, 1},
                          Fixture{128, 10, 1, 2}, Fixture{287, 5, 2, 1}, Fixture{158, 12, 2, 3},
                          Fixture{278, 5, 9, 3}, Fixture{286, 11, 10, 4}, Fixture{409, 8, 10, 4},
                          Fixture{3, 7, 1, 2}, Fixture{7, 1, 7, 1}, Fixture{4, 5, 1, 3},
                          Fixture{1, 4, 2, 3}, Fixture{53, 12, 12, 1}, Fixture{5, 13, 5, 2},
                          Fixture{4, 2, 1, 2}, Fixture{2, 3, 4, 2}, Fixture{3, 2, 10, 2},
                          Fixture{1, 5, 1, 2}, Fixture{3, 1, 2, 3}, Fixture{107, 13, 8, 4},
                          Fixture{22, 11, 6, 2}}) {
        QFile pageFile(QString("pages/page%1.dat").arg(f.page));
        check(pageFile.open(QIODevice::ReadOnly), "Missing fixed anchor fixture");
        PageAnalysisResult analysis;
        QDataStream pageStream(&pageFile); pageStream >> analysis.page;
        analysis.resolveWordBoundaries(f.page, &font);
        auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        const auto segment = [&](const QJsonObject& declarations) {
            return analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &declarations);
        };
        check(segment(config), "Missing initial anchor partition for fixture");
        const auto expected = word.wordResultInfo.subWords[f.subword - 1];
        check(segment(config), "Fixed anchors lost a complete partition");
        const auto& actual = word.wordResultInfo.subWords[f.subword - 1];
        check(samePartition(actual, expected), "Repeating anchor segmentation changed the partition");
        check(!actual.templateTrace.isEmpty() && actual.templateTrace.front().startsWith("Fixed anchors"),
              "The fixture did not use the anchor search");
        if (f.page == 2 && f.line == 7) {
            const AnchorForms ineligible;
            const std::array priorities{AnchorPriority{config, ineligible}, AnchorPriority{config, anchors}};
            const auto later = [&] {
                CutGeometryMemoScope memo;
                return segmentWithAnchors(word, actual, priorities, {});
            }();
            check(later && samePartition(*later, actual),
                  "An ineligible configuration blocked the next fixed-anchor configuration");
            int rejected = 0;
            const AnchorPartitionCheck reject = [&](const auto&) { ++rejected; return false; };
            auto bounded = config;
            bounded.remove("fixedAnchorExpansion");
            bounded.insert("fixedAnchorExpansionChecks", 64);
            check(!analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1,
                                           &font, true, &bounded, reject),
                  "Rejecting every anchor partition returned a complete result");
            const auto& restored = word.wordResultInfo.subWords[f.subword - 1];
            check(rejected > 0 && restored.baseGlyphs.empty() && !restored.correct,
                  "A rejected fixed-anchor partition survived validation");
        }
    }
    for (const auto& f : {Fixture{3, 1, 2, 2}, Fixture{3, 1, 9, 1},
                          Fixture{48, 6, 6, 2}, Fixture{61, 10, 2, 2}, Fixture{221, 13, 10, 1}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing final dal fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        check(page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Final dal did not assemble with its preceding bodies");
        const auto& word = page.page.lines[f.line - 1].words[f.word - 1];
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        check(sub.templateTrace.value(0).startsWith("Fixed anchors") && !sub.kashidaPaths.back().isEmpty(),
              "Final dal fell back or lost the connector");
        const auto source = word.bodyShape(sub);
        auto ink = source.transform.map(source.path).translated(source.pos);
        ink.setFillRule(Qt::WindingFill);
        WordResultInfo::SubWordInfo empty;
        OutlineSearchCache cache;
        SearchContext geometry{.transformedPath = ink, .bbox = ink.boundingRect(), .subword = empty,
                               .outlineCache = &cache};
        check(preparePassOutline(geometry, cache), "Cannot prepare the final dal outline");
        const auto form = bodyConstraintForm(config, namedLetterForm(config, "dal-fina"));
        const auto candidates = anchoredBodies(geometry, form, 2, 1);
        const auto& seam = sub.kashidaCuts.back().second;
        const auto chosen = std::find_if(candidates.begin(), candidates.end(), [&](const auto& body) {
            return QLineF(body.entry->topBaseLine.point, body.entry->bottomBaseLine.point) == seam;
        });
        check(chosen != candidates.end() && chosen->entryLimit &&
              getJoinTime(*chosen->entry, geometry.orientation).isLeft(chosen->entryLimit->time),
              "Assembly moved final dal's cut past the recognized fork");
        const std::map<int, double> expectedFork{{3, f.word == 2 ? 14. : 8.}, {48, 29.}, {61, 28.}, {221, 9.}};
        check(chosen->entryLimit->time == expectedFork.at(f.page), "Final dal borrowed a neighboring fork");
        if (f.page == 221) {
            const auto preferred = std::find_if(candidates.begin(), chosen, [&](const auto& body) {
                return body.entryLimit->time == chosen->entryLimit->time;
            });
            check(preferred != chosen && seam.p2().y() < preferred->entry->bottomBaseLine.point.y(),
                  "Tight qaf–dal join did not retreat up the same flank");
        }

        geometry.pairedStrokeWidth = 100;
        const auto independent = anchoredBodies(geometry, form, 2, 1);
        check(independent.size() == candidates.size() &&
              std::equal(candidates.begin(), candidates.end(), independent.begin(), [](const auto& a, const auto& b) {
                  return a.entry->topBaseLine.point == b.entry->topBaseLine.point &&
                         a.entry->bottomBaseLine.point == b.entry->bottomBaseLine.point;
              }), "Final dal's entries depend on the preceding stroke width");
    }
    for (const auto& f : {Fixture{3, 5, 1, 1}, Fixture{4, 13, 5, 1}, Fixture{6, 9, 8, 2},
                          Fixture{8, 5, 4, 2}, Fixture{71, 5, 5, 1},
                          Fixture{141, 14, 5, 2}, Fixture{218, 15, 1, 3}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing fixed-anchor migration fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        check(page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Migrated initial form or attachment prevented assembly");
        const auto& sub = page.page.lines[f.line - 1].words[f.word - 1].wordResultInfo.subWords[f.subword - 1];
        check(sub.templateTrace.value(0).startsWith("Fixed anchors") &&
              sub.letterDiagnostics.last().toObject().value("template") == "dal-fina",
              "Migrated final dal subword used the old search");
        if (f.page == 3)
            check(sub.letterDiagnostics.at(1).toObject().value("template") == "hah-stacked-medi" &&
                  sub.kashidaPaths.front().isEmpty(), "Stacked hah lost its shared seam");
        else if (f.page == 8 || f.page == 71) {
            const auto expectedHah = f.page == 8 ? "hah-stacked-medi" : "hah-shoulder-medi";
            check(sub.letterDiagnostics.first().toObject().value("template") == "beh-stacked-init" &&
                  sub.letterDiagnostics.at(1).toObject().value("template") == expectedHah &&
                  sub.kashidaPaths.front().isEmpty(), "Stacked tooth did not share the hah attachment");
            const auto seam = sub.joins.begin()->cutSegments().front();
            check(!seam.isNull() && std::abs(seam.dy()) < 1e-5, "Stacked tooth left the attachment level");
        } else if (f.page == 141)
            check(sub.text.contains(QChar(0x0640)) && !sub.kashidaPaths.front().isEmpty(),
                  "Fixed anchors lost the explicitly transcribed tatweel's stroke");
        else
            check(sub.letterDiagnostics.first().toObject().value("template") == "seen-init",
                  "Fixed anchors lost the recognized initial seen");
    }
    for (const auto& f : {Fixture{3, 9, 2, 2}, Fixture{36, 11, 11, 2}, Fixture{486, 11, 11, 2},
                          Fixture{533, 3, 2, 1}, Fixture{603, 9, 8, 1}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing fixed-anchor dal migration fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        check(page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Fixed anchors lost a dal-ending subword");
        const auto& sub = page.page.lines[f.line - 1].words[f.word - 1].wordResultInfo.subWords[f.subword - 1];
        check(sub.templateTrace.value(0).startsWith("Fixed anchors") &&
              sub.letterDiagnostics.last().toObject().value("template") == "dal-fina",
              "A dal-ending fixture fell back to the original cutter");
        if (f.page == 486)
            check(sub.letterDiagnostics.at(1).toObject().value("template") == "hah-raised-open-lip-medi" &&
                  sub.kashidaPaths.at(0).isEmpty() && sub.kashidaPaths.at(1).isEmpty(),
                  "The open lip lost one of its attachment seams");
    }
    for (const auto& f : {Fixture{135, 13, 8, 1}, Fixture{267, 15, 5, 2}, Fixture{576, 14, 10, 1}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing fixed hah–waw junction fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        const int line = f.line - 1, word = f.word - 1, subword = f.subword - 1;
        check(page.segmentSubword(f.page - 1, line, word, subword, &font, true, &config),
              "The fixed lower lip and waw junction did not assemble");
        const auto& item = page.page.lines[line].words[word];
        const auto& result = item.wordResultInfo.subWords[subword];
        check(result.templateTrace.value(0).startsWith("Fixed anchors") && result.kashidaPaths.size() == 2 &&
              result.kashidaPaths.back().isEmpty() && coversOriginalInk(item, result),
              "The hah–waw seam lost ink or manufactured a connector");
    }
    {
        QFile file("pages/page23.dat");
        check(file.open(QIODevice::ReadOnly), "Missing fixed hah–kaf junction fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(23, &font);
        check(page.segmentSubword(22, 11, 5, 2, &font, true, &config),
              "The nearly coincident hah and kaf cuts did not assemble");
        const auto& word = page.page.lines[11].words[5];
        const auto& sub = word.wordResultInfo.subWords[2];
        check(sub.templateTrace.value(0).startsWith("Fixed anchors") &&
              sub.letterDiagnostics.at(2).toObject().value("template") == "kaf-fork-medi" &&
              sub.kashidaPaths.size() == 4 && sub.kashidaPaths[1].isEmpty() &&
              !sub.kashidaPaths[2].isEmpty() && coversOriginalInk(word, sub),
              "Resolving the hah–kaf junction changed the kaf–meem connector");
    }
    struct StackedJoin { int page, line, word, subword, connectors; double corner; };
    for (const auto& f : {StackedJoin{36, 13, 7, 1, 1, 11}, StackedJoin{226, 3, 7, 2, 1, 15},
                          StackedJoin{317, 13, 4, 1, 1, 11}, StackedJoin{453, 15, 8, 1, 1, 31},
                          StackedJoin{550, 2, 12, 1, 2, 14}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing upper attachment fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        check(page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Stacked subword did not assemble");
        const auto& word = page.page.lines[f.line - 1].words[f.word - 1];
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        const auto initial = f.page == 453 ? "ain-init" : "beh-stacked-init";
        check(sub.templateTrace.value(0).startsWith("Fixed anchors") &&
              sub.letterDiagnostics.first().toObject().value("template") == initial &&
              !sub.joins.empty() && !sub.kashidaPaths.empty() && sub.kashidaPaths.front().isEmpty() &&
              std::count_if(sub.kashidaPaths.begin(), sub.kashidaPaths.end(),
                            [](const auto& path) { return !path.isEmpty(); }) == f.connectors &&
              coversOriginalInk(word, sub), "Stacked join changed stroke ownership");
        const auto& seam = *sub.joins.begin();
        check(seam.bottomBaseLine.time == f.corner && std::abs(sectionLine(seam).dy()) < 1e-5,
              "Stacked join missed the upper attachment corner");
    }
    for (const auto& f : {Fixture{21, 9, 4, 2}, Fixture{544, 14, 6, 1},
                          Fixture{407, 7, 8, 1}, Fixture{579, 1, 11, 2}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing stacked attachment and alif fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        check(page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Stacked attachment and alif did not assemble");
        const auto& word = page.page.lines[f.line - 1].words[f.word - 1];
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        check(sub.templateTrace.value(0).startsWith("Fixed anchors") &&
              sub.letterDiagnostics.last().toObject().value("template") == "alef-fina" &&
              sub.kashidaPaths.size() == 2 && sub.kashidaPaths.front().isEmpty() &&
              sub.kashidaPaths.back().isEmpty() == (f.page == 407 || f.page == 579) &&
              coversOriginalInk(word, sub),
              "The stacked attachment or the alif connector changed ownership");
        const auto& join = *sub.joins.rbegin();
        const auto foot = sub.kashidaPaths.back().isEmpty() ? sectionLine(join) : sub.kashidaCuts.back().second;
        check(foot.dy() > .3, "Alif used the horizontal shaft section instead of the foot");
    }
    {
        QFile file("pages/page259.dat");
        check(file.open(QIODevice::ReadOnly), "Missing alif shaft ownership fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(259, &font);
        check(page.segmentSubword(258, 4, 0, 2, &font, true, &config), "Alif shaft ownership did not assemble");
        const auto& word = page.page.lines[4].words[0];
        const auto& sub = word.wordResultInfo.subWords[2];
        check(sub.letterDiagnostics.last().toObject().value("template") == "alef-return-fina" &&
              sub.baseGlyphs.size() == 3 && coversOriginalInk(word, sub),
              "Upright alif candidate sliced the shaft off the final body");
    }
    struct RehJunction { int page, line, word, subword, connectors; double lowerCorner; };
    for (const auto& f : {RehJunction{16, 3, 1, 2, 3, 29},
                          RehJunction{19, 12, 3, 1, 2, 15},
                          RehJunction{34, 12, 9, 3, 3, 25}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing fixed rāʾ junction fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        const int line = f.line - 1, word = f.word - 1, subword = f.subword - 1;
        check(page.segmentSubword(f.page - 1, line, word, subword, &font, true, &config),
              "The fixed rāʾ entry did not assemble");
        const auto& item = page.page.lines[line].words[word];
        const auto& result = item.wordResultInfo.subWords[subword];
        check(result.templateTrace.value(0).startsWith("Fixed anchors") &&
              result.letterDiagnostics.last().toObject().value("template") == "reh-fina" &&
              std::count_if(result.kashidaPaths.begin(), result.kashidaPaths.end(),
                            [](const auto& path) { return !path.isEmpty(); }) == f.connectors &&
              coversOriginalInk(item, result), "Final rāʾ changed connector ownership");
        const auto& join = *result.joins.rbegin();
        const auto& lower = result.kashidaPaths.back().isEmpty() ? join.bottomBaseLine : join.bottomLeft;
        check(std::abs(lower.time - f.lowerCorner) < 1e-6, "Final rāʾ lost its entry corner");
    }
    {
        QFile file("pages/page33.dat");
        check(file.open(QIODevice::ReadOnly), "Missing final lām fork endpoint fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(33, &font);
        check(page.segmentSubword(32, 12, 10, 0, &font, true, &config), "Final lām fork did not assemble");
        const auto& word = page.page.lines[12].words[10];
        const auto& sub = word.wordResultInfo.subWords[0];
        check(sub.templateTrace.value(0).startsWith("Fixed anchors") &&
              sub.letterDiagnostics.last().toObject().value("template") == "lam-bowl-fina" &&
              sub.kashidaPaths.size() == 1 && !sub.kashidaPaths.front().isEmpty() &&
              std::abs(sub.joins.begin()->bottomLeft.time - 9.19066) < 1e-5 &&
              coversOriginalInk(word, sub),
              "Final lām skipped the fork at the lower endpoint of the preliminary cut");
    }
    {
        const Fixture f{53, 12, 2, 1};
        QFile file("pages/page53.dat");
        check(file.open(QIODevice::ReadOnly), "Missing page 53 attachment fixture");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        const auto segment = [&](const QJsonObject& rules) {
            return page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &rules);
        };
        const auto& word = page.page.lines[f.line - 1].words[f.word - 1];
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        check(segment(config), "Missing established page 53 cut");
        const auto expected = sub;
        check(segment(config) && sub.templateTrace.value(0).startsWith("Fixed anchors") &&
              sub.kashidaCuts == expected.kashidaCuts && sub.baseGlyphs.size() == expected.baseGlyphs.size(),
              "Fixed anchor assembly changed a verified page 53 cut");
        for (size_t i = 0; i < sub.baseGlyphs.size(); ++i)
            check(sub.baseGlyphs[i].path == expected.baseGlyphs[i].path, "Page 53 body outline changed");
        check(page.segmentSubword(52, 8, 6, 1, &font, true, &config), "Stacked tāʾ–khāʾ did not assemble");
        const auto& stackedWord = page.page.lines[8].words[6];
        const auto& stacked = stackedWord.wordResultInfo.subWords[1];
        check(stacked.templateTrace.value(0).startsWith("Fixed anchors") &&
              stacked.letterDiagnostics.at(1).toObject().value("template") == "hah-returning-lips-medi" &&
              stacked.kashidaPaths.front().isEmpty() && !stacked.kashidaPaths.back().isEmpty() &&
              coversOriginalInk(stackedWord, stacked), "Stacked tāʾ–khāʾ lost its shared entry or outgoing connector");
        const auto& seam = *stacked.joins.begin();
        check(std::abs(seam.topBaseLine.time - 2.17912) < 1e-5 &&
              seam.bottomBaseLine.time == 19 && std::abs(sectionLine(seam).dy()) < 1e-5,
              "Stacked tāʾ–khāʾ did not use the horizontal attachment");
    }
    QFile foldedPage("pages/page21.dat");
    check(foldedPage.open(QIODevice::ReadOnly), "Missing folded seen–yeh fixture");
    PageAnalysisResult folded;
    QDataStream foldedStream(&foldedPage); foldedStream >> folded.page;
    folded.resolveWordBoundaries(21, &font);
    auto foldedAlternates = config;
    foldedAlternates.insert("fixedAnchorAlternatesEnding", QStringLiteral("ي"));
    check(folded.segmentSubword(20, 3, 4, 1, &font, true, &foldedAlternates),
          "The folded seen–yeh fixture lost its partition");
    const auto& foldedWord = folded.page.lines[3].words[4].wordResultInfo.subWords[1];
    check(foldedWord.kashidaPaths.back().isEmpty() &&
          foldedWord.letterDiagnostics.at(2).toObject().value("template") == "seen-folded-medi",
          "Fixed-anchor alternatives separated a folded seen–yeh join");
    for (const auto& f : {Fixture{5, 14, 8, 2}, Fixture{21, 3, 5, 2}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing lam–yeh fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        const auto segment = [&](const QJsonObject& forms) {
            return page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &forms);
        };
        const auto& sub = page.page.lines[f.line - 1].words[f.word - 1].wordResultInfo.subWords[f.subword - 1];
        check(segment(config), "Missing established lam–yeh partition");
        const auto expected = sub;
        check(expected.kashidaPaths.front().isEmpty() == (f.page == 5),
              "The lam–yeh fixture no longer exercises the expected join");
        auto alternates = config;
        alternates.insert("fixedAnchorAlternatesEnding", QStringLiteral("يئى"));
        check(segment(alternates) && samePartition(sub, expected),
              "Fixed-anchor alternatives changed lam–yeh stroke ownership");
    }
    checkTemplatePriority(config, font);

    // The lām–kāf return separates the two stems and keeps the bowl.
    QFile pageFile("pages/page42.dat");
    check(pageFile.open(QIODevice::ReadOnly), "Missing terminal-bowl ownership fixture");
    PageAnalysisResult analysis;
    QDataStream pageStream(&pageFile); pageStream >> analysis.page;
    analysis.resolveWordBoundaries(42, &font);
    const auto& word = analysis.page.lines[0].words[1];
    const auto& sub = word.wordResultInfo.subWords[0];
    check(analysis.segmentSubword(41, 0, 1, 0, &font, true, &config),
          "Lām–kāf return lost its partition");
    check(sub.templateTrace.value(0).startsWith("Fixed anchors") &&
          sub.letterDiagnostics.last().toObject().value("template") == "kaf-return-fina",
          "Lām–kāf did not select the terminal return");
    check(sub.kashidaPaths.size() == 2 && !sub.kashidaPaths.back().isEmpty() &&
          coversOriginalInk(word, sub), "Lām–kāf lost its connecting stroke or source ink");

    for (const auto& f : {Fixture{36, 14, 10, 1}, Fixture{313, 8, 3, 2}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing final alif foot fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        check(page.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &config),
              "Fixed anchors lost the final alif");
        const auto& alif = page.page.lines[f.line - 1].words[f.word - 1].wordResultInfo.subWords[f.subword - 1];
        check(alif.templateTrace.value(0).startsWith("Fixed anchors") && !alif.kashidaCuts.empty(),
              "Final alif foot fell back to the original cutter");
        const auto cap = alif.kashidaCuts.back().second;
        check(!cap.isNull() && std::abs(cap.dy()) > .6 * cap.length(), "Final alif cut through the shaft above the foot");
    }

    for (const auto& f : {Fixture{287, 2, 1, 4}, Fixture{323, 13, 6, 1}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing mīm rim fixture");
        PageAnalysisResult page;
        QDataStream stream(&file); stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        const int line = f.line - 1, word = f.word - 1, index = f.subword - 1;
        check(page.segmentSubword(f.page - 1, line, word, index, &font, true, &config),
              "Mīm rim did not segment");
        const auto& result = page.page.lines[line].words[word].wordResultInfo.subWords[index];
        check(result.templateTrace.value(0).startsWith("Fixed anchors"), "Mīm rim required the previous cutter");
        const bool shared = result.kashidaPaths.front().isEmpty();
        const QLineF entry = shared ? result.joins.begin()->cutSegments().front() : result.kashidaCuts.front().second;
        check(shared == (f.page == 287) && entry.length() > .5 && entry.length() < 1.5,
              "Mīm entry cut through the full height of the rim");
    }
}

static void checkMigratedAnchors(const QJsonObject& config) {
    struct Case {
        int page, line, word, subword;
        const char* final;
    };
    PageAnalysisResult::initQuranText();
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing migration font");
    QDataStream fontStream(&fontFile);
    AFont font;
    fontStream >> font;
    for (const auto& f : {Case{10, 8, 5, 1, "noon-fina"},
                          Case{27, 1, 2, 1, "seen-fina"},
                          Case{44, 3, 7, 1, "lam-stem-fork-fina"},
                          Case{56, 5, 4, 2, "reh-fina"},
                          Case{56, 6, 7, 1, "heh-counter-fina"},
                          Case{76, 7, 8, 2, "seen-fina"},
                          Case{107, 11, 9, 1, "lam-stem-fork-fina"},
                          Case{119, 11, 1, 1, "alef-detached-fina"},
                          Case{145, 12, 3, 1, "reh-fina"},
                          Case{532, 6, 1, 1, "heh-counter-fina"},
                          Case{583, 11, 8, 2, "feh-fina"},
                          Case{584, 3, 8, 1, "reh-fina"},
                          Case{590, 15, 3, 1, "sad-tah-fina"}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing migration page");
        PageAnalysisResult page;
        QDataStream stream(&file);
        stream >> page.page;
        page.resolveWordBoundaries(f.page, &font);
        const int line = f.line - 1, word = f.word - 1, slot = f.subword - 1;
        check(page.segmentSubword(f.page - 1, line, word, slot, &font, true, &config),
              "Anchor search did not complete the subword");
        const auto& item = page.page.lines[line].words[word];
        const auto& result = item.wordResultInfo.subWords[slot];
        check(result.letterDiagnostics.last().toObject().value("template") == f.final &&
              coversOriginalInk(item, result), "Migrated fixed anchors changed body ownership");
        if (f.page == 10)
            check(result.kashidaPaths.size() == 3 && !result.kashidaPaths[1].isEmpty(),
                  "Explicit tatweel lost its stroke");
        if (f.page == 56 && f.line == 5)
            check(result.kashidaPaths.size() == 1 && result.kashidaPaths.front().isEmpty(),
                  "Three-tooth body and final ra must share the junction");
        if (f.page == 56 && f.line == 6)
            check(result.letterDiagnostics.at(0).toObject().value("template") == "kaf-descending-hook-init" &&
                  result.letterDiagnostics.at(1).toObject().value("template") == "heh-body-medi" &&
                  !result.kashidaPaths.empty() && !result.kashidaPaths.front().isEmpty(),
                  "Kaf joined the wrong medial heh body");
        if (f.page == 76) {
            const auto& entry = *result.joins.rbegin();
            check(result.kashidaPaths.size() == 2 && !result.kashidaPaths.back().isEmpty() &&
                  std::abs(entry.topLeft.time - 5) < 1e-6 && std::abs(entry.bottomLeft.time - 30) < 1e-6,
                  "Final seen missed the first tooth junction");
        }
        if (f.page == 532)
            check(!result.kashidaPaths.empty() && !result.kashidaPaths.front().isEmpty(),
                  "The initial tooth took the connector before sin");
        if (f.page == 590)
            check(result.letterDiagnostics.at(1).toObject().value("template") == "hah-stacked-medi" &&
                  result.kashidaPaths.size() == 3 && result.kashidaPaths.at(0).isEmpty() &&
                  result.kashidaPaths.at(1).isEmpty() && !result.kashidaPaths.at(2).isEmpty(),
                  "Meem and stacked hah lost their shared attachments");
        if (f.page == 584)
            check(result.letterDiagnostics.at(1).toObject().value("template") == "hah-joining-lip-medi" &&
                  result.kashidaPaths.size() == 3 && result.kashidaPaths[1].isEmpty(),
                  "The lower lip of medial hah became a connector");
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    checkMigratedAnchors(config);
    checkFixedAnchors(config);
    CutGeometryMemoScope memo;
    cutGeometryMemo->profiling = true;
    int toothPairs = 0;
    struct Fixture { int page, line, word, sub; };
    for (const auto f : {Fixture{3,1,5,1}, Fixture{600,2,4,2}, Fixture{286,2,2,1}}) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Candidate fixture missing");
        PageAnalysisResult analysis;
        QDataStream in(&file); in >> analysis.page;
        check(analysis.segmentSubword(f.page-1,f.line-1,f.word-1,f.sub-1,nullptr,true),
              "Candidate fixture failed segmentation");
        const auto& word = analysis.page.lines[f.line-1].words[f.word-1];
        const auto& sub = word.wordResultInfo.subWords[f.sub-1];
        const auto shape = word.bodyShape(sub);
        WordResultInfo::SubWordInfo empty;
        SearchContext context{.transformedPath=shape.transform.map(shape.path).translated(shape.pos), .subword=empty};
        OutlineSearchCache cache;
        check(preparePassOutline(context, cache), "Candidate outline preparation failed");
        context.outlineCache = &cache;
        context.bbox = context.transformedPath.boundingRect();
        context.nextIter = context.xSorted.begin();
        context.pairedStrokeWidth = .5;
        for (const auto* id : {"beh-tooth-medi","beh-upright-medi","beh-bent-medi"}) {
            auto form = resolvedBodyForm(config,namedLetterForm(config,id));
            const auto verify = [&](const QJsonObject& declaration) {
                const auto expected = computeToothBoundaries(context,declaration);
                const auto recovered = computeToothBoundaries(context,declaration,true);
                for (int repeat=0; repeat<2; ++repeat) {
                    const auto actual = declaredToothBoundaries(context,declaration);
                    const auto a = snapshot(actual), b = snapshot(expected);
                    if (a != b) {
                        int first=0;
                        while(first<std::min(a.size(),b.size()) && a[first]==b[first]) ++first;
                        qWarning() << f.page << id << repeat << actual.size() << expected.size() << "byte" << first
                                   << a.mid(first,32).toHex() << b.mid(first,32).toHex();
                    }
                    check(a==b,"Cached tooth candidates differ from a fresh search");
                    check(snapshot(declaredToothBoundaries(context,declaration,true)) == snapshot(recovered),
                          "Cached raised entries differ from a fresh projection");
                }
                toothPairs += expected.size();
            };
            verify(form);
            auto cuts = form.value("cuts").toObject(); cuts.insert("leftOffset",.65); form.insert("cuts",cuts);
            verify(form);
            auto body = form.value("body").toObject(); body.insert("maxPairDistance",1.1); form.insert("body",body);
            verify(form);
        }
        // Reusing the same cache with moved source geometry cannot retain old coordinates.
        context.transformedPath.translate(3,-2);
        check(preparePassOutline(context,cache), "Moved outline preparation failed");
        context.bbox = context.transformedPath.boundingRect();
        context.nextIter = context.xSorted.begin();
        const auto form = resolvedBodyForm(config,namedLetterForm(config,"beh-tooth-medi"));
        check(snapshot(declaredToothBoundaries(context,form))==snapshot(computeToothBoundaries(context,form)),
              "A source edit reused stale tooth coordinates");
    }
    check(toothPairs>0, "Candidate tests only exercised empty results");
    bool reused = false;
    for (const auto& s : cutGeometryMemo->forms) reused |= s.requests>s.builds;
    check(reused, "No candidate inventory was reused");
    qInfo("Candidate search passed: %d tooth pairs, anchor ownership, and source edits.",toothPairs);
}
