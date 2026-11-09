// Body evidence survives policy edits; geometry is generated from declarations.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void check(bool passed, const char* message) {
    if (!passed) qFatal("%s", message);
}

static bool same(const Stretching& a, const Stretching& b) {
    return a.topBaseLine.point == b.topBaseLine.point && a.bottomBaseLine.point == b.bottomBaseLine.point;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    const auto declaration = namedLetterForm(config, "beh-tooth-medi");
    const auto body = declaration.value("body").toObject();
    const auto cuts = declaration.value("cuts").toObject();
    check(toothPairDescription(body) && !declaration.contains("tooth"), "Tooth still uses the legacy schema");
    check(!body.contains("leftOffset") && !body.contains("rightOffset") &&
          !body.value("baselineEntry").toObject().contains("turnAngle"), "Placement leaked into recognition");

    // Shape selection, reference resolution and schema validation are shared
    // with lām; a new form ID needs no special code in the recognizer.
    auto inherited = declaration;
    inherited.insert("id", "test-inherited-tooth");
    inherited.insert("body", QJsonObject{{"inherit", "beh-tooth-medi"}});
    inherited.insert("cuts", QJsonObject{{"inherit", "beh-tooth-medi"}, {"leftOffset", .2}});
    auto draft = config;
    auto forms = draft.value("medial").toArray();
    forms.append(inherited);
    draft.insert("medial", forms);
    check(PageAnalysisResult::prepareLetterForms(draft).isEmpty(), "Inherited tooth declarations were rejected");
    const auto resolved = bodyCutProgram(draft).forms.value("test-inherited-tooth");
    check(resolved.body == body && resolved.cuts.value("leftOffset") == .2 &&
          resolved.cuts.value("rightOffset") == cuts.value("rightOffset"), "Body/cut inheritance was not independent");

    for (int failure = 0; failure < 12; ++failure) {
        auto invalid = config;
        auto rule = declaration;
        auto shape = body, policy = cuts;
        if (failure == 0) shape.insert("leftOffset", .3);
        if (failure == 1) policy.insert("minWidth", .3);
        if (failure == 2) policy.insert("offsetUnit", "unspecified");
        if (failure == 3) shape.insert("maxPairDistance", -1);
        if (failure == 4) shape.insert("minWidth", shape.value("maxWidth"));
        if (failure == 5) policy.insert("leftOffset", -1);
        if (failure == 6) policy.insert("baselineEntry", QJsonObject{{"turnAngle", 90}});
        if (failure == 7) policy.insert("boundaries", QJsonObject{{"left", "unknown"}, {"right", "tooth-normal"}});
        if (failure == 8) shape.insert("shape", "unknown");
        if (failure == 9) rule.insert("tooth", QJsonObject{{"maxPairDistance", 2.5}});
        if (failure == 10) shape.insert("inherit", "beh-tooth-medi");
        rule.insert("body", shape); rule.insert("cuts", policy);
        if (failure == 11) rule.remove("cuts");
        auto list = invalid.value("medial").toArray();
        for (int i = 0; i < list.size(); ++i)
            if (list[i].toObject().value("id") == "beh-tooth-medi") list[i] = rule;
        invalid.insert("medial", list);
        check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid body/cut declaration was accepted");
    }

    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Page 3 fixture missing");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    const auto& word = analysis.page.lines[1].words[3];
    const auto& sub = word.wordResultInfo.subWords[0];
    const auto& shape = word.paths[sub.paths.front()];
    auto ink = shape.transform.map(shape.path).translated(shape.pos);
    ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo empty;
    SearchContext context{.transformedPath = ink, .subword = empty};
    check(readTerminalOutline(context), "Fixture outline missing");
    const auto evidence = recognizeToothBodies(context, body);
    const auto projected = cutToothBodies(context, evidence, cuts);
    const auto target = std::find_if(projected.begin(), projected.end(), [](const auto& tooth) {
        return std::abs(tooth.tip.time - 5.53152858) < 1e-6 && tooth.bend.time == 17;
    });
    check(target != projected.end(), "Known tooth missing from declared body/policy path");

    auto changedPolicy = cuts;
    changedPolicy.insert("leftOffset", .2);
    const auto moved = cutToothBodies(context, evidence, changedPolicy);
    const auto changed = std::find_if(moved.begin(), moved.end(), [&](const auto& tooth) {
        return tooth.tip.time == target->tip.time && tooth.bend.time == target->bend.time;
    });
    check(changed != moved.end() && !same(changed->left, target->left) && same(*changed->right, *target->right),
          "A left-cut policy edit changed the recognized body or the opposite boundary");
    for (const auto* tooth : {&*target, &*changed}) {
        const auto regions = cutRegions(ink, {}, {*tooth->right, tooth->left}, context.orientation, true);
        check(regions.size() == 3 && tooth->retainedBy(regions[1].path), "Policy discarded the recognized tooth core");
    }
    const auto replay = cutToothBodies(context, evidence, cuts);
    check(replay.size() == projected.size(), "Projection changed the cached body evidence");
    for (size_t i = 0; i < replay.size(); ++i)
        check(same(replay[i].left, projected[i].left) && same(*replay[i].right, *projected[i].right),
              "Replaying the original policy did not restore the exact cuts");
    context.baseText = QStringLiteral("unrelated");
    context.pairedStrokeWidth = 1000; context.nextIter = context.xSorted.end();
    const auto independent = declaredToothBoundaries(context, declaration);
    check(independent.size() == projected.size(), "Neighbor state changed body recognition");
    for (size_t i = 0; i < independent.size(); ++i)
        check(same(independent[i].left, projected[i].left) && same(*independent[i].right, *projected[i].right),
              "Neighbor state changed the declared tooth cuts");
    qInfo("Body/cuts passed: shared inheritance, schema validation, independent recognition, policy edits, core ownership and exact replay.");
}
