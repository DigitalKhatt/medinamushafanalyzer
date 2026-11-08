#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}

static QByteArray readFixture(const QString& name) {
    QFile file(name);
    check(file.open(QIODevice::ReadOnly), "Missing native fixture");
    return file.readAll();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Invalid letter forms");
    const QStringList toothRules{"beh-init", "beh-shallow-step-init", "beh-upright-medi", "beh-tooth-medi"};
    int checkedSelectors = 0;
    for (auto phase : {"initial", "medial"}) {
        for (const auto& value : config.value(phase).toArray()) {
            const auto rule = value.toObject();
            if (!toothRules.contains(rule.value("id").toString())) continue;
            const auto letters = rule.value("current").toString();
            check(letters.count(QChar(u'ئ')) == 1 && letters.count(QChar(u'ى')) == 1,
                  "Both yeh variants must be eligible for each tooth form");
            ++checkedSelectors;
        }
    }
    check(checkedSelectors == toothRules.size(), "Missing tooth selector");
    for (const auto& value : config.value("final").toArray()) {
        auto rule = value.toObject();
        if (rule.value("id") != "hah-fina") continue;
        check(rule.value("penWidth").toDouble() == 0.5, "Use the manuscript's pen width in page units");
        for (const auto& invalid : QJsonArray{0, -0.5, "half"}) {
            rule.insert("penWidth", invalid);
            check(!invalidWalkSettings(rule, "hah-fina").isEmpty(), "Invalid pen width accepted");
        }
    }

    AFont font;
    QDataStream fontStream(readFixture("input/font.dat"));
    fontStream >> font;
    PageAnalysisResult::initQuranText();
    // Numbered corners identify landmarks in these native fixtures.
    struct Fixture { int page, line, word, subword; const char* form; int corner; };
    const Fixture fixtures[] = {
        {6, 7, 2, 3, "beh-init", -1}, {22, 1, 7, 2, "beh-tooth-medi", -1},
        {24, 7, 2, 1, "hah-fina", 18}, {25, 4, 5, 3, "hah-fina", 19},
        {56, 6, 9, 2, "hah-fina", 32}, {130, 6, 8, 1, "hah-fina", 23},
        {211, 4, 8, 2, "hah-stacked-fina", 18}, {566, 13, 11, 2, "hah-stacked-fina", 17},
        {395, 9, 7, 3, "hah-fina", 27}, {417, 13, 4, 2, "hah-fina", 34}
    };
    for (const auto& f : fixtures) {
        PageAnalysisResult analysis;
        QDataStream stream(readFixture(QString("pages/page%1.dat").arg(f.page)));
        stream >> analysis.page;
        analysis.resolveWordBoundaries(f.page, &font);
        auto& word = analysis.page.lines[f.line - 1].words[f.word - 1];
        if (word.wordResultInfo.subWords.empty()) analysis.detectSubWords(f.page - 1, f.line - 1, f.word - 1, &font);
        check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true),
              "Native subword remains partial");
        const auto& sub = word.wordResultInfo.subWords[f.subword - 1];
        check(!sub.provisional() && sub.matchedTemplates.contains(f.form), "Expected body form did not match");
        if (f.corner < 0) continue;

        const auto& outline = word.paths[sub.paths.front()];
        const auto mapped = [&](const QPainterPath& path) {
            auto ink = outline.transform.map(path);
            ink.translate(outline.pos);
            ink.setFillRule(Qt::WindingFill);
            return ink;
        };
        const auto ink = mapped(outline.path);
        const auto finalBody = mapped(sub.baseGlyphs.back().path).boundingRect();
        check(std::abs(finalBody.bottom() - ink.boundingRect().bottom()) < 0.001, "Final letter lost its bowl");
        const auto& join = *sub.joins.rbegin();
        if (QString(f.form) == "hah-stacked-fina") {
            check(sub.kashidaPaths.front().isEmpty(), "Stacked junction generated a kashida");
            check(join.bottomBaseLine.time == f.corner &&
                  join.topBaseLine.point.x() < join.bottomBaseLine.point.x() &&
                  std::abs(join.topBaseLine.point.y() - join.bottomBaseLine.point.y()) < 1e-6,
                  "Shared cut must start at the entry corner and cross left");
            const auto upperBody = mapped(sub.baseGlyphs.front().path).boundingRect();
            check(std::abs(upperBody.bottom() - join.bottomBaseLine.point.y()) < 0.001 &&
                  upperBody.top() < finalBody.top(), "Upper tooth lost its attaching stroke");
            continue;
        }
        check(!sub.kashidaPaths.back().isEmpty(), "Ordinary connector was lost");
        const auto curves = baseContourCurves(ink);
        const auto& flank = curves.at(f.corner);
        const auto bottom = join.bottomLeft;
        const double t = bottom.time - f.corner;
        check(t > 0 && t < 1 && std::abs(flank.getSubRange(0, t).length(0.001) - 0.5) < 0.001,
              "Final cut must be half a page unit along the descending flank");
        const auto tangent = flank.pointAt(t + 1e-5) - flank.pointAt(t - 1e-5);
        const auto chord = join.topLeft.point - bottom.point;
        check(std::abs(QPointF::dotProduct(tangent, chord)) <
              1e-4 * std::hypot(tangent.x(), tangent.y()) * std::hypot(chord.x(), chord.y()),
              "Final cut must cross the descending stroke along its normal");
        if (f.page == 25) {
            auto preview = config;
            auto finals = preview.value("final").toArray();
            for (int i = 0; i < finals.size(); ++i) {
                auto rule = finals[i].toObject();
                if (rule.value("id") != "hah-fina") continue;
                rule.insert("penWidth", 0.25);
                finals[i] = rule;
            }
            preview.insert("final", finals);
            check(analysis.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.subword - 1, &font, true, &preview),
                  "Pen-width preview failed");
            const double changedT = sub.joins.rbegin()->bottomLeft.time - f.corner;
            check(changedT > 0 && changedT < t && std::abs(flank.getSubRange(0, changedT).length(0.001) - 0.25) < 0.001,
                  "Preview must use the configured pen width");
        }
    }
}
