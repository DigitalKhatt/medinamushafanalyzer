// A tiny reverse bend must not hide a terminal bowl's own shoulder.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static PageAnalysisResult fixture(int number = 442) {
    QFile file(QString("pages/page%1.dat").arg(number));
    check(file.open(QIODevice::ReadOnly), "Bowl shoulder fixture is required");
    PageAnalysisResult page; QDataStream in(&file); in >> page.page;
    return page;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    const auto form = resolvedLetterForm(config, "beh-return-fina");
    const auto settings = shapeSettings(form, "tail");
    check(settings.value("flankSampleWidths").toDouble() == .25, "Missing intrinsic flank sampling distance");
    auto page = fixture();
    auto& word = page.page.lines[6].words[5];
    const auto source = word.bodyShape(word.wordResultInfo.subWords[0]);
    auto ink = source.transform.map(source.path).translated(source.pos);
    ink.setFillRule(Qt::WindingFill);
    QLineF expected;
    for (double scale : {1., .7, 1.6}) {
        const QTransform map(scale, 0, 0, scale, 37, -21);
        WordResultInfo::SubWordInfo empty;
        SearchContext context{.transformedPath=map.map(ink), .subword=empty};
        OutlineSearchCache cache;
        check(preparePassOutline(context, cache), "Cannot prepare bowl outline");
        context.bbox = context.transformedPath.boundingRect();
        const auto body = recognizeTailBranches(inkOutline(context), form.value("body").toObject().value("tail").toObject());
        check(!body.branches.empty() && body.branches.front().corner.time == 19,
              "Terminal bowl borrowed the medial tooth instead of its own shoulder");
        const auto& branch = body.branches.front();
        const auto at = context.timeSorted.find(branch.corner);
        const auto micro = branch.forward ? context.prev(at) : context.next(at);
        check(micro->point.x() < at->point.x() && QLineF(at->point, micro->point).length() < .1 * branch.width,
              "Fixture no longer contains the microscopic reverse bend");
        Stretching cut{};
        check(cutTailBranches(context, body, settings, cut), "Shoulder did not produce a valid normal cut");
        const QLineF chord(cut.topBaseLine.point, cut.bottomBaseLine.point);
        check(QLineF(chord.p2(), branch.corner.point).length() <= .5 * branch.width + 1e-5,
              "The half-width walk moved away from the bowl's shoulder");
        if (scale == 1) expected = QLineF(map.inverted().map(chord.p1()), map.inverted().map(chord.p2()));
        else {
            const double error = std::max(QLineF(chord.p1(), map.map(expected.p1())).length(),
                                          QLineF(chord.p2(), map.map(expected.p2())).length());
            qInfo() << "Scaled shoulder" << scale << "endpoint error" << error;
            // Normal intersections and arc integration use page-unit tolerances.
            check(error < .01 * branch.width,
                  "Shoulder recognition changed under scaling or translation");
        }

        // Removing an insignificant intermediate landmark must not move the anchor.
        context.timeSorted.erase(micro);
        const auto simplified = recognizeTailBranches(inkOutline(context), form.value("body").toObject().value("tail").toObject());
        Stretching same{};
        check(cutTailBranches(context, simplified, settings, same) &&
              same.topBaseLine.point == cut.topBaseLine.point && same.bottomBaseLine.point == cut.bottomBaseLine.point,
              "The cut depends on a microscopic intermediate landmark");

        // Neighboring joins and transcript do not participate in body recognition.
        Stretching unrelated{}; unrelated.topBaseLine.point={-200,300}; unrelated.bottomBaseLine.point={-100,500};
        empty.joins.insert(unrelated); context.baseText=QStringLiteral("اختبار"); context.pairedStrokeWidth=1000;
        const auto independent = recognizeTailBranches(inkOutline(context), form.value("body").toObject().value("tail").toObject());
        check(cutTailBranches(context, independent, settings, same) &&
              same.topBaseLine.point == cut.topBaseLine.point && same.bottomBaseLine.point == cut.bottomBaseLine.point,
              "Bowl shoulder depends on a neighboring cut");
    }
    check(page.segmentSubword(441,6,5,0,nullptr,true), "Shoulder change left the subword incomplete");
    const auto& sub = word.wordResultInfo.subWords[0];
    check(sub.baseGlyphs.size()==3 && sub.letterDiagnostics.last().toObject().value("template")=="beh-return-fina",
          "Assembly replaced the bowl's own shoulder with another template");
    check(sub.kashidaCuts.size()==2 && !sub.kashidaPaths.back().isEmpty(), "The genuine connector was lost");
    check(QLineF(sub.kashidaCuts.back().second.p1(), expected.p1()).length()<1e-5 &&
          QLineF(sub.kashidaCuts.back().second.p2(), expected.p2()).length()<1e-5,
          "Assembly moved the final cut away from its intrinsic shoulder");

    // A correct new final proposal must not let assembly reuse that bowl's
    // shoulder as a medial tooth and give the actual tooth to initial kaf.
    auto adjacent = fixture(402);
    auto& otherWord = adjacent.page.lines[5].words[6];
    auto& otherSub = otherWord.wordResultInfo.subWords[0];
    const auto otherSource = otherWord.bodyShape(otherSub);
    auto otherInk = otherSource.transform.map(otherSource.path).translated(otherSource.pos);
    otherInk.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo empty;
    SearchContext geometry{.transformedPath=otherInk, .subword=empty};
    OutlineSearchCache cache;
    check(preparePassOutline(geometry, cache), "Cannot prepare adjacent tooth outline");
    geometry.bbox = otherInk.boundingRect();
    const auto bowl = recognizeTailBranches(inkOutline(geometry), form.value("body").toObject().value("tail").toObject());
    check(!bowl.branches.empty() && bowl.branches.front().corner.time == 23,
          "Adjacent tooth replaced the bowl shoulder");
    check(adjacent.segmentSubword(401,5,6,0,nullptr,true) && otherSub.baseGlyphs.size()==3,
          "Bowl ownership check prevented complete assembly");
    auto entry = *otherSub.joins.rbegin();
    if (!otherSub.kashidaPaths.back().isEmpty()) {
        entry.topBaseLine = entry.topLeft;
        entry.bottomBaseLine = entry.bottomLeft;
    }
    check(getJoinTime(entry, geometry.orientation).isLeft(bowl.branches.front().corner.time),
          "Shared assembly gave the bowl shoulder to the medial tooth");
    const QPointF noonCore(148,223);
    check(otherInk.contains(noonCore), "Reviewed noon core left the source ink");
    for (size_t i=0;i<otherSub.baseGlyphs.size();++i) {
        const auto mask = otherSource.transform.map(otherSub.baseGlyphs[i].path).translated(otherSource.pos);
        check(mask.contains(noonCore) == (i==1), "Kaf or final beh took the medial noon's core");
    }

    // The same shoulder may be rounded. Skipping it would identify the tall
    // neighboring tooth as the bowl and then prevent a valid shared assembly.
    auto rounded = fixture(218);
    auto& roundedWord = rounded.page.lines[12].words[2];
    auto& roundedSub = roundedWord.wordResultInfo.subWords[2];
    const auto roundedSource = roundedWord.bodyShape(roundedSub);
    auto roundedInk = roundedSource.transform.map(roundedSource.path).translated(roundedSource.pos);
    roundedInk.setFillRule(Qt::WindingFill);
    SearchContext roundedGeometry{.transformedPath=roundedInk, .subword=empty};
    OutlineSearchCache roundedCache;
    check(preparePassOutline(roundedGeometry, roundedCache), "Cannot prepare rounded shoulder");
    roundedGeometry.bbox = roundedInk.boundingRect();
    const auto roundedBowl = recognizeTailBranches(inkOutline(roundedGeometry), form.value("body").toObject().value("tail").toObject());
    check(!roundedBowl.branches.empty() && roundedBowl.branches.front().corner.time == 13 &&
          !roundedBowl.branches.front().corner.isSharp,
          "Rounded bowl shoulder was skipped in favor of a neighboring sharp tooth");
    check(rounded.segmentSubword(217,12,2,2,nullptr,true) && roundedSub.baseGlyphs.size()==3,
          "Rounded shoulder left a formerly complete subword unresolved");
    auto roundedEntry = *roundedSub.joins.rbegin();
    if (!roundedSub.kashidaPaths.back().isEmpty()) {
        roundedEntry.topBaseLine = roundedEntry.topLeft;
        roundedEntry.bottomBaseLine = roundedEntry.bottomLeft;
    }
    check(getJoinTime(roundedEntry, roundedGeometry.orientation).isLeft(roundedBowl.branches.front().corner.time),
          "Assembly moved the cut inside the rounded bowl shoulder");

    // The exit flank has turned right after half a width, but is still
    // slightly left of the shoulder. Recover that shoulder and preserve
    // its stroke-normal cut through assembly.
    auto hooked = fixture(105);
    auto& hookedWord = hooked.page.lines[0].words[1];
    auto& hookedSub = hookedWord.wordResultInfo.subWords[1];
    const auto hookedSource = hookedWord.bodyShape(hookedSub);
    auto hookedInk = hookedSource.transform.map(hookedSource.path).translated(hookedSource.pos);
    hookedInk.setFillRule(Qt::WindingFill);
    SearchContext hookedGeometry{.transformedPath=hookedInk, .subword=empty};
    OutlineSearchCache hookedCache;
    check(preparePassOutline(hookedGeometry, hookedCache), "Cannot prepare hooked bowl outline");
    hookedGeometry.bbox = hookedInk.boundingRect();
    const auto hookedBody = recognizedSharedShape(hookedGeometry, form.value("body").toObject());
    PathExtrema shoulder{};
    Stretching hookedCut{};
    check(hookedBody->tail && cutTailBranches(hookedGeometry, *hookedBody->tail, settings, hookedCut, &shoulder)
          && shoulder.time == 33, "Hooked bowl skipped its shoulder and cut at a neighboring landmark");
    check(hooked.segmentSubword(104,0,1,1,nullptr,true) &&
          hookedSub.letterDiagnostics.last().toObject().value("template") == "beh-return-fina" &&
          !hookedSub.kashidaCuts.empty(), "Hooked bowl did not assemble with the returning form");
    const auto cap = hookedSub.kashidaCuts.back().second;
    check(QLineF(cap.p1(), hookedCut.topBaseLine.point).length() < 1e-5 &&
          QLineF(cap.p2(), hookedCut.bottomBaseLine.point).length() < 1e-5,
          "Assembly replaced the hooked bowl's stroke-normal cut");
    qInfo() << "Bowl shoulder passed: point 19, normal cut" << expected
            << "; scale, landmarks, independent recognition, rounded shoulder and adjacent tooth ownership.";
}
