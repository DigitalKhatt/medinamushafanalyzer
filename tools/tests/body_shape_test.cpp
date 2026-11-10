// Independent body constraints and baseline-entry teeth in the lam-kha ligature.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool value, const char* message) {
    if (!value) qFatal("%s", message);
}

static SearchContext geometry(QPainterPath path, WordResultInfo::SubWordInfo& dummy) {
    path.setFillRule(Qt::WindingFill);
    SearchContext c{.transformedPath = path, .subword = dummy};
    c.bbox = path.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(path, c.subPaths, extrema);
    for (const auto& p : extrema) if (p.pathIndex == 0) {
        c.timeSorted.insert(p); c.xSorted.insert(p); c.ySorted.insert(p);
    }
    c.setOrientation();
    c.nextIter = std::next(c.xSorted.begin());
    return c;
}

static bool same(const Stretching& a, const Stretching& b, const QTransform& transform = {}) {
    return QLineF(transform.map(a.topBaseLine.point), b.topBaseLine.point).length() < 1e-4 &&
        QLineF(transform.map(a.bottomBaseLine.point), b.bottomBaseLine.point).length() < 1e-4;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    const auto rule = [&](const char* phase, const char* id) {
        for (const auto& value : config.value(phase).toArray())
            if (value.toObject().value("id") == id) return value.toObject();
        qFatal("Missing test template"); return QJsonObject{};
    };
    const auto lam = bodyConstraintForm(config, rule("initial", "lam-init")),
        lamNormal = bodyConstraintForm(config, rule("initial", "lam-normal-init"));
    const auto tooth = rule("medial", "beh-tooth-medi");
    QFile file("pages/page600.dat");
    check(file.open(QIODevice::ReadOnly), "Page 600 cache missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    auto& word = analysis.page.lines[2].words[9];
    auto& sub = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[sub.paths.front()];
    auto source = shape.transform.map(shape.path); source.translate(shape.pos);
    WordResultInfo::SubWordInfo dummy;
    auto c = geometry(source, dummy);
    c.form = lam;
    check(declaredLamBoundary(c, config, c.form), "Expected the old overwide lam proposal");
    check(!matchesBoundaryBody(c, lam, c.res, true), "Overwide lam body was accepted");
    c.form = lamNormal; c.res = {};
    check(declaredLamBoundary(c, config, c.form) && matchesBoundaryBody(c, lamNormal, c.res, true), "Lam stem was rejected");
    const auto lamCut = c.res;
    const auto badShared = contourBodyBoundaries(c, pairedRouteSettings(rule("medial", "hah-stacked-medi")));
    check(!badShared.empty(), "Expected the former lam-tip ligature proposal");
    for (const auto& pair : badShared)
        check(!matchesBoundaryBody(c, lamNormal, pair.first, true), "Shared entry bypassed lam body constraints");
    const auto proposals = declaredToothBoundaries(c, tooth);
    const auto tall = std::find_if(proposals.begin(), proposals.end(), [](const auto& p) {
        return std::abs(p.tip.time - 15.41831769) < 1e-6;
    });
    check(tall != proposals.end() && tall->bend.time == 27, "Tall tooth was not recognized from its own extrema");
    const auto turn = std::find_if(c.timeSorted.begin(), c.timeSorted.end(), [](const auto& p) {
        return std::abs(p.time - 12.6578) < 1e-4;
    });
    check(turn != c.timeSorted.end(), "Baseline turn missing");
    const double width = QLineF(tall->right->topBaseLine.point, tall->right->bottomBaseLine.point).length();
    check(QLineF(tall->right->topBaseLine.point, turn->point).length() < .1 * width,
          "Right cut is not at the foot of the tooth's inclined stroke");
    const auto regions = cutRegions(source, {}, {*tall->right, tall->left}, c.orientation, true);
    const auto footSample = QLineF(tall->right->topBaseLine.point, tall->right->bottomBaseLine.point).center() - QPointF(.25 * width, 0);
    check(regions.size() == 3 && regions[1].path.contains(footSample), "Tooth's descending stroke was left in kashida");
    c.baseText = QStringLiteral("بدون سياق"); c.pairedStrokeWidth = 1000; c.nextIter = c.xSorted.end();
    const auto independent = declaredToothBoundaries(c, tooth);
    check(std::any_of(independent.begin(), independent.end(), [&](const auto& p) {
        return same(*tall->right, *p.right) && same(tall->left, p.left);
    }), "Neighbor state changed the baseline-entry tooth");
    for (double scale : {.7, 1.6}) {
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto scaled = geometry(mapping.map(source), dummy);
        const auto found = declaredToothBoundaries(scaled, tooth);
        check(std::any_of(found.begin(), found.end(), [&](const auto& p) {
            return same(*tall->right, *p.right, mapping) && same(tall->left, p.left, mapping);
        }), "Baseline-entry tooth changed under scale/translation");
        scaled.form = lamNormal;
        check(declaredLamBoundary(scaled, config, scaled.form) && same(lamCut, scaled.res, mapping) &&
              matchesBoundaryBody(scaled, lamNormal, scaled.res, true), "Lam body constraints changed under scaling");
    }
    check(analysis.segmentSubword(599, 2, 9, 0, nullptr, true), "Lam-kha example did not complete");
    check(sub.baseGlyphs.size() == 5 && sub.kashidaPaths.size() == 4 && sub.kashidaPaths[0].isEmpty(),
          "Lam-kha ligature has an artificial kashida");
    check(sub.matchedTemplates.contains("lam-normal-init") && !sub.matchedTemplates.contains("lam-init"),
          "Rejected lam template was still reported as matched");
    const std::vector<Stretching> joins(sub.joins.begin(), sub.joins.end());
    Stretching entry{}; entry.topBaseLine = joins[2].topLeft; entry.bottomBaseLine = joins[2].bottomLeft;
    check(same(*tall->right, entry) && same(tall->left, joins[3]), "Assembly reused the small tooth or moved the tall tooth cuts");
    qInfo("Body shapes passed: lam proportions/shared-entry guard, tall-tooth baseline turn, unique tooth ownership, independent geometry, scale/translation and no lam-kha kashida.");
}
