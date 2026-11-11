// Keep the folded ligature's crossing test stable at exact Bezier corners.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char* message) { if (!ok) qFatal("%s", message); }

static const LamBodyRecognition& bodyEvidence(const SubwordBodyRecognition& recognition, const QJsonObject& config) {
    const auto body = formDescription(config, namedLetterForm(config, "lam-medi"), "body");
    const auto* found = recognizedLamBody(recognition, body);
    check(found != nullptr, "Missing declared lam body evidence");
    return *found;
}

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

static void checkDeclaredLamRoutes() {
    const auto config = PageAnalysisResult::letterForms();
    QFile file("pages/page3.dat");
    check(file.open(QIODevice::ReadOnly), "Missing declarative route fixture");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    analysis.resolveWordBoundaries(3, nullptr);
    analysis.resolveCompoundBody(2, 0, 1);
    const auto& word = analysis.page.lines[0].words[1];
    const auto& sub = word.wordResultInfo.subWords[1];
    const auto shape = word.bodyShape(sub);
    WordResultInfo::SubWordInfo dummy{};
    auto context = geometry(shape.transform.map(shape.path).translated(shape.pos), dummy, {}, {});
    const auto declaration = namedLetterForm(config, "lam-init");
    const auto body = formDescription(config, declaration, "body").value("route").toObject();
    const auto evidence = recognizeContourRoute(context, body);
    check(evidence.has_value(), "Declared initial stem landmarks not found");
    check(projectContourRoute(context, withOverrides(body, QJsonObject{{"bottomProbe", "down"}}), *evidence),
          "Vertical projection of declared landmarks failed");
    const auto vertical = context.res;
    context.res = {};
    check(projectContourRoute(context, withOverrides(body, QJsonObject{{"bottomProbe", "normal"}}), *evidence),
          "Normal projection of the same landmarks failed");
    check(vertical.topBaseLine.time == context.res.topBaseLine.time &&
          vertical.bottomBaseLine.time != context.res.bottomBaseLine.time,
          "Cut-policy change must preserve the recognized landmark and change its projection");
    // Even a usable legacy walk cannot rescue a failed declaration.
    auto invalid = declaration;
    auto invalidBody = formDescription(config, declaration, "body");
    invalidBody.insert("shape", "unknown");
    invalid.insert("body", invalidBody);
    invalid.insert("method", "walk");
    invalid.insert("top", body.value("top"));
    invalid.insert("bottomProbe", "down");
    check(!declaredLamBoundary(context, config, invalid), "Failed lām declaration fell back to a legacy walk");
}

static void checkBodyRecognitionSeparation() {
    Word absent{};
    WordResultInfo::SubWordInfo missing{};
    missing.text = QStringLiteral("ل");
    missing.paths = {0};
    check(!recognizeSubwordBodies(absent, missing, PageAnalysisResult::letterForms()).valid,
          "Missing source must not be passed to body geometry");
    QFile file("pages/page296.dat");
    check(file.open(QIODevice::ReadOnly), "Missing body recognition fixture");
    PageAnalysisResult analysis;
    QDataStream stream(&file);
    stream >> analysis.page;
    analysis.resolveWordBoundaries(296, nullptr);
    auto& word = analysis.page.lines[14].words[3];
    auto& sub = word.wordResultInfo.subWords[0];
    analysis.resolveCompoundBody(295, 14, 3);
    const auto config = PageAnalysisResult::letterForms();
    const auto evidence = recognizeSubwordBodies(word, sub, config);
    check(evidence.valid && !bodyEvidence(evidence, config).lamBodies.forks.empty() && !bodyEvidence(evidence, config).lamBodies.crests.empty(),
          "Uncut ink did not produce reusable lam body evidence");

    const auto hasProfile = [&](const SubwordBodyRecognition& recognized, const char* id) {
        return recognizedLamBody(recognized, formDescription(config, namedLetterForm(config, id), "body")) != nullptr;
    };
    auto initialLam = sub;
    initialLam.text = QStringLiteral("لَهُمْ");
    const auto initialEvidence = recognizeSubwordBodies(word, initialLam, config);
    check(hasProfile(initialEvidence, "lam-init") && !hasProfile(initialEvidence, "lam-medi") &&
          !hasProfile(initialEvidence, "lam-bowl-fina") && !hasProfile(initialEvidence, "lam-hah-stacked-init"),
          "Initial lām admitted a template for another position or neighbor");
    initialLam.text = QStringLiteral("لَحَمَ");
    check(hasProfile(recognizeSubwordBodies(word, initialLam, config), "lam-hah-stacked-init"),
          "An adjacent hah did not enable the stacked lām ligature");

    // Changing every lam cut option must not change body recognition. Nor may
    // leftover masks or joins from a previous preview affect its landmarks.
    auto changed = config;
    auto forms = changed.value("medial").toArray();
    for (int i = 0; i < forms.size(); ++i) {
        auto form = forms[i].toObject();
        if (form.value("id") != "lam-medi") continue;
        form.insert("cuts", QJsonObject{{"forkOffset", 1.25}});
        forms[i] = form;
    }
    changed.insert("medial", forms);
    auto contaminated = sub;
    contaminated.baseGlyphs = {{QStringLiteral("wrong preview"), QPainterPath{}}};
    contaminated.joins.insert(Stretching{});
    const auto repeated = recognizeSubwordBodies(word, contaminated, changed);
    check(repeated.source == evidence.source && bodyEvidence(repeated, config).lamBodies.crests.size() == bodyEvidence(evidence, config).lamBodies.crests.size() &&
          bodyEvidence(repeated, config).lamBodies.forks.size() == bodyEvidence(evidence, config).lamBodies.forks.size(), "Cuts changed body evidence");
    for (size_t i = 0; i < bodyEvidence(evidence, config).lamBodies.forks.size(); ++i) {
        const auto& a = bodyEvidence(evidence, config).lamBodies.forks[i];
        const auto& b = bodyEvidence(repeated, config).lamBodies.forks[i];
        check(a.tip.point == b.tip.point && a.tip.time == b.tip.time &&
              a.forwardFlank == b.forwardFlank && a.backwardFlank == b.backwardFlank &&
              a.rightForward == b.rightForward && a.fork == b.fork && a.positiveTurn == b.positiveTurn,
              "A policy moved recognized landmarks");
    }
    check(analysis.segmentSubword(295, 14, 3, 0, nullptr, true), "Counter attachment proposal failed");
    auto plan = makeSubwordCutPlan(sub, config);
    check(std::any_of(plan.bodies.begin(), plan.bodies.end(), [](const auto& body) {
        return body.letterIndex == 2 && body.attachment == BodyAttachmentPolicy::PreserveCounterEntry;
    }), "Kaf-lam did not select the shared counter attachment policy");
    const auto saved = sub;
    check(resolveSubwordCutPlan(word, evidence, plan, config), "Independent cut-policy preview failed");
    check(sub.kashidaCuts == saved.kashidaCuts && sub.baseGlyphs.size() == saved.baseGlyphs.size(),
          "Policy preview changed the published partition");
    for (size_t i = 0; i < sub.baseGlyphs.size(); ++i)
        check(sub.baseGlyphs[i].path == saved.baseGlyphs[i].path, "Policy preview mutated a published body");
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
    check(outlines.size() == 3 && sub.baseGlyphs.size() == 4, "Expected four matched bodies");
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

    // Use the same ink with new selectors to isolate recipe dispatch from shape recognition.
    auto config = PageAnalysisResult::letterForms();
    auto prefix = namedLetterForm(config, "lam-return-fina");
    prefix.insert("id", "test-prefix-fina");
    prefix.insert("current", QStringLiteral("پ"));
    auto arm = namedLetterForm(config, "alef-detached-fina");
    arm.insert("id", "test-arm-fina");
    arm.insert("current", QStringLiteral("ڤ"));
    auto medial = namedLetterForm(config, "lam-medi");
    medial.insert("id", "test-prefix-medi");
    medial.insert("current", prefix.value("current"));
    medial.insert("terminalArmRecovery", QJsonObject{{"prefixForm", prefix.value("id")},
        {"detached", QJsonObject{{"form", arm.value("id")}, {"variant", "test-detached-arm"}}},
        {"cuts", medial.value("terminalArmRecovery").toObject().value("cuts")}});
    auto finals = config.value("final").toArray();
    finals.append(prefix);
    finals.append(arm);
    config.insert("final", finals);
    auto medials = config.value("medial").toArray();
    medials.append(medial);
    config.insert("medial", medials);
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Recovery recipe rejected other letter selectors");

    PageAnalysisResult sample;
    sample.page.lines.resize(1);
    sample.page.lines[0].words.resize(1);
    auto& sampleWord = sample.page.lines[0].words[0];
    sampleWord.paths.append(shape);
    auto& relabeled = sampleWord.wordResultInfo.subWords.emplace_back();
    relabeled.paths = {0};
    relabeled.text = PageAnalysisResult::bodyLetters(sub.text).chopped(2) + QStringLiteral("پڤ");
    check(sample.segmentSubword(-2, 0, 0, 0, nullptr, true, &config), "Recovery recipe did not run for other letters");
    const auto recovered = relabeled.letterDiagnostics[2].toObject();
    check(recovered.value("template") == medial.value("id") && recovered.value("prefixTemplate") == prefix.value("id") &&
          recovered.value("variant") == "test-detached-arm" &&
          relabeled.letterDiagnostics.last().toObject().value("template") == arm.value("id"),
          "Recovery ignored the declared prefix or arm profile");
    check(relabeled.baseGlyphs.back().path == outlines.back() && coversOriginalInk(sampleWord, relabeled),
          "Reused recovery lost the detached outline or changed ink ownership");
}

static void checkTerminalReturnPolicy() {
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing font fixture");
    AFont font;
    QDataStream fontStream(&fontFile);
    fontStream >> font;
    for (const auto fixture : {std::array{397, 4, 1, 1}, std::array{545, 15, 1, 2},
                               std::array{7, 10, 6, 2}}) {
        const auto [page, line, wordIndex, subIndex] = fixture;
        QFile pageFile(QString("pages/page%1.dat").arg(page));
        check(pageFile.open(QIODevice::ReadOnly), "Missing terminal-return fixture");
        PageAnalysisResult analysis;
        QDataStream stream(&pageFile);
        stream >> analysis.page;
        analysis.resolveWordBoundaries(page, &font);
        check(analysis.segmentSubword(page - 1, line - 1, wordIndex - 1, subIndex - 1, &font, true),
              "Terminal-return ligature became partial");
        const auto& word = analysis.page.lines[line - 1].words[wordIndex - 1];
        const auto& sub = word.wordResultInfo.subWords[subIndex - 1];
        const auto letters = PageAnalysisResult::bodyLetters(sub.text);
        check(letters.endsWith(QStringLiteral("لا")), "Unexpected terminal-return text");
        const auto body = sub.letterDiagnostics[letters.size() - 2].toObject();
        check(body.value("bodyForm") == "lam-terminal-return" && body.value("cutPolicy") == "lam-alef-shared",
              "Lam body recognition and its ligature policy were not recorded separately");
        check(sub.kashidaPaths.size() + 1 == sub.baseGlyphs.size() && sub.kashidaPaths.back().isEmpty(),
              "The terminal return was extracted as kashida");
        const auto shape = word.bodyShape(sub);
        const auto lam = shape.transform.map(sub.baseGlyphs[letters.size() - 2].path).translated(shape.pos);
        check(lam.boundingRect().height() > 10 && coversOriginalInk(word, sub),
              "Lam lost its lower return or the ligature lost ink");
        const auto masks = sub.baseGlyphs;
        check(analysis.segmentSubword(page - 1, line - 1, wordIndex - 1, subIndex - 1, &font, true),
              "Repeated terminal-return segmentation became partial");
        for (size_t i = 0; i < masks.size(); ++i)
            check(masks[i].path == sub.baseGlyphs[i].path, "Ligature policy is not deterministic");
    }
}

static void checkTaperedArmJunction() {
    QFile pageFile("pages/page588.dat"), fontFile("input/font.dat");
    check(pageFile.open(QIODevice::ReadOnly) && fontFile.open(QIODevice::ReadOnly), "Missing tapered arm fixture");
    PageAnalysisResult analysis;
    AFont font;
    QDataStream pageStream(&pageFile), fontStream(&fontFile);
    pageStream >> analysis.page;
    fontStream >> font;
    analysis.resolveWordBoundaries(588, &font);
    check(analysis.segmentSubword(587, 4, 9, 0, &font, true), "Tapered alef junction remains partial");
    const auto& word = analysis.page.lines[4].words[9];
    const auto& sub = word.wordResultInfo.subWords[0];
    check(sub.baseGlyphs.size() == 3 && sub.joins.size() == 2 &&
          sub.matchedTemplates.contains("alef-tapered-arm-fina"), "Missing three ligature bodies");
    const auto& cut = *sub.joins.rbegin();
    check(cut.topBaseLine.time == 4 && cut.bottomBaseLine.time == 9,
          "Tapered alef cut must join both inward attachment corners, not clip its tip");
    const auto& kaf = *sub.joins.begin();
    check(kaf.counterEntry.pathIndex == 1 && kaf.counterExit.pathIndex == 1 &&
          kaf.topBaseLine.time == 1 && std::abs(kaf.bottomBaseLine.time - 13.362153931450187) < 1e-7,
          "Alef junction adjustment disturbed the kaf-lam attachment");
    const auto shape = word.bodyShape(sub);
    const auto lam = shape.transform.map(sub.baseGlyphs[1].path).translated(shape.pos);
    const auto alef = shape.transform.map(sub.baseGlyphs[2].path).translated(shape.pos);
    const auto original = shape.transform.map(shape.path).translated(shape.pos);
    const QPointF tipInk(156.3, 195.55);
    check(alef.contains(tipInk) && !lam.contains(tipInk) &&
          std::abs(lam.boundingRect().bottom() - original.boundingRect().bottom()) < 1e-7,
          "Alef lost its tip or lam lost its lower return");
    check(std::all_of(sub.kashidaPaths.begin(), sub.kashidaPaths.end(), [](const auto& p) { return p.isEmpty(); }) &&
          coversOriginalInk(word, sub), "Shared ligature junction introduced kashida or lost source ink");
    const auto saved = sub.baseGlyphs;
    check(analysis.segmentSubword(587, 4, 9, 0, &font, true), "Repeated tapered alef segmentation failed");
    for (size_t i = 0; i < saved.size(); ++i)
        check(saved[i].path == sub.baseGlyphs[i].path, "Tapered alef junction is not deterministic");
}

static void checkSharedAlefJunctionPolicy() {
    QFile pageFile("pages/page444.dat"), fontFile("input/font.dat");
    check(pageFile.open(QIODevice::ReadOnly) && fontFile.open(QIODevice::ReadOnly), "Missing return-arm fixture");
    PageAnalysisResult analysis;
    AFont font;
    QDataStream pageStream(&pageFile), fontStream(&fontFile);
    pageStream >> analysis.page;
    fontStream >> font;
    analysis.resolveWordBoundaries(444, &font);
    auto& word = analysis.page.lines[12].words[5];
    auto& sub = word.wordResultInfo.subWords[1];
    const auto config = PageAnalysisResult::letterForms();
    auto routePolicy = config;
    auto forms = routePolicy.value("medial").toArray();
    for (int i = 0; i < forms.size(); ++i) {
        auto form = forms[i].toObject();
        if (form.value("id") != "lam-medi") continue;
        auto cuts = form.value("cuts").toObject();
        auto terminal = cuts.value("beforeFinal").toObject();
        terminal.remove("junction");
        cuts.insert("beforeFinal", terminal);
        form.insert("cuts", cuts);
        forms[i] = form;
    }
    routePolicy.insert("medial", forms);
    const auto evidence = recognizeSubwordBodies(word, sub, config);
    check(bodyEvidence(evidence, config).alefAttachment.has_value() && bodyEvidence(evidence, config).alefAttachment->first.time == 5 &&
          bodyEvidence(evidence, config).alefAttachment->second.time == 13, "Uncut alef junction was not recognized");
    auto separated = sub;
    separated.text = PageAnalysisResult::bodyLetters(sub.text);
    separated.text.insert(separated.text.size() - 1, QChar(0x0628));
    const auto separatedEvidence = recognizeSubwordBodies(word, separated, config);
    check(!bodyEvidence(separatedEvidence, config).alefAttachment, "A nonadjacent alef triggered arm recognition");
    const auto otherPolicy = recognizeSubwordBodies(word, sub, routePolicy);
    check(bodyEvidence(otherPolicy, config).alefAttachment && bodyEvidence(otherPolicy, config).alefAttachment->first.point == bodyEvidence(evidence, config).alefAttachment->first.point &&
          bodyEvidence(otherPolicy, config).alefAttachment->second.point == bodyEvidence(evidence, config).alefAttachment->second.point,
          "Changing the junction policy changed body recognition");
    check(analysis.segmentSubword(443, 12, 5, 1, &font, true, &routePolicy), "Original return-arm route failed");
    const auto old = sub;
    check(std::abs(old.joins.rbegin()->bottomBaseLine.time - 12.924620117624805) < 1e-7,
          "Disabled junction policy did not retain the contour route");
    check(analysis.segmentSubword(443, 12, 5, 1, &font, true, &config), "Shared junction policy left a partial body");
    const auto& cut = *sub.joins.rbegin();
    check(cut.topBaseLine.time == 5 && cut.bottomBaseLine.time == 13 &&
          sub.matchedTemplates.contains("alef-return-fina") &&
          sub.letterDiagnostics[1].toObject().value("junctionPolicy") == "inward-corners",
          "The return-arm recognition route bypassed the shared junction policy");
    check(sub.baseGlyphs[0].path == old.baseGlyphs[0].path && sub.kashidaPaths[0] == old.kashidaPaths[0] &&
          sub.kashidaCuts[0] == old.kashidaCuts[0] && sub.kashidaPaths.back().isEmpty() && coversOriginalInk(word, sub),
          "Shared alef junction disturbed the feh or its connector, or lost ink");
    const auto shape = word.bodyShape(sub);
    const auto alef = shape.transform.map(sub.baseGlyphs.back().path).translated(shape.pos);
    const auto lam = shape.transform.map(sub.baseGlyphs[1].path).translated(shape.pos);
    check(alef.contains({177.4, 410.15}) && !lam.contains({177.4, 410.15}), "Alef still lost its root to lam");

    // A preceding kaf can also contain a tapered diagonal. Its two corners
    // cannot be assigned to alef if that would enclose the earlier body.
    QFile kafFile("pages/page284.dat");
    check(kafFile.open(QIODevice::ReadOnly), "Missing diagonal ownership fixture");
    PageAnalysisResult kafPage;
    QDataStream kafStream(&kafFile);
    kafStream >> kafPage.page;
    kafPage.resolveWordBoundaries(284, &font);
    auto& kafWord = kafPage.page.lines[3].words[3];
    auto& ligature = kafWord.wordResultInfo.subWords[0];
    const auto retainsOuterArms = [&](const auto& result) {
        if (result.baseGlyphs.size() != 3) return false;
        const auto source = kafWord.bodyShape(result);
        const auto bounds = [&](const QPainterPath& path) {
            return source.transform.map(path).translated(source.pos).boundingRect();
        };
        const auto ink = bounds(source.path);
        const auto kaf = bounds(result.baseGlyphs.front().path);
        const auto alef = bounds(result.baseGlyphs.back().path);
        return std::abs(kaf.right() - ink.right()) < 1e-6 &&
            std::abs(alef.left() - ink.left()) < 1e-6 && alef.right() < ink.center().x() &&
            coversOriginalInk(kafWord, result);
    };
    check(kafPage.segmentSubword(283, 3, 3, 0, &font, true, &routePolicy), "Original diagonal ownership failed");
    const auto original = ligature;
    check(kafPage.segmentSubword(283, 3, 3, 0, &font, true, &config), "Junction policy crossed an earlier body");
    check(retainsOuterArms(original) && retainsOuterArms(ligature), "The kaf and alef arms were swapped");
    check(ligature.baseGlyphs.front().path == original.baseGlyphs.front().path &&
          ligature.kashidaPaths == original.kashidaPaths && ligature.kashidaPaths.back().isEmpty(),
          "The lam-alef seam moved an earlier cut or introduced a connector");
}

static void checkTerminalLamEntryPolicy() {
    const auto config = PageAnalysisResult::letterForms();
    auto disabled = config;
    for (const auto* phase : {"initial", "medial", "final"}) {
        auto forms = disabled.value(phase).toArray();
        for (int i = 0; i < forms.size(); ++i) {
            auto form = forms[i].toObject();
            if (!form.contains("cuts")) continue;
            auto cuts = form.value("cuts").toObject();
            if (QString(phase) == "final")
                cuts.insert("boundaries", QJsonObject{{"right", "preserve"}, {"left", "free"}});
            if (cuts.value("beforeFinal").isObject()) {
                auto terminal = cuts.value("beforeFinal").toObject();
                terminal.insert("boundaries", QJsonObject{{"right", "preserve"}, {"left", "shared-return"}});
                cuts.insert("beforeFinal", terminal);
            }
            form.insert("cuts", cuts);
            forms[i] = form;
        }
        disabled.insert(phase, forms);
    }
    struct Fixture { int page, line, word, sub, lam; bool preserved; };
    const std::array cases{
        Fixture{282, 10, 9, 1, 2, false}, // Final bowl after another lam.
        Fixture{444, 13, 6, 2, 1, false}, // Lam-alef: shorten only the incoming flank.
        Fixture{600, 2, 4, 2, 1, false},
        Fixture{4, 8, 1, 1, 1, true},    // Kaf with final lam remains a separate ligature.
        Fixture{588, 5, 10, 1, 1, true}, // Both kaf-lam bridges stay unchanged before alef.
    };
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing terminal lam font");
    AFont font;
    QDataStream fs(&fontFile);
    fs >> font;
    for (const auto& f : cases) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing terminal lam fixture");
        PageAnalysisResult a;
        QDataStream ps(&file);
        ps >> a.page;
        a.resolveWordBoundaries(f.page, &font);
        auto& word = a.page.lines[f.line - 1].words[f.word - 1];
        auto& sub = word.wordResultInfo.subWords[f.sub - 1];
        check(a.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.sub - 1, &font, true, &disabled),
              "Terminal lam without the entry policy became partial");
        const auto old = sub;
        check(a.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.sub - 1, &font, true, &config),
              "Terminal lam fork policy became partial");
        check(sub.baseGlyphs.size() == old.baseGlyphs.size() && sub.kashidaPaths.size() == old.kashidaPaths.size() &&
              coversOriginalInk(word, sub), "Terminal lam entry lost ink or a letter");
        for (size_t j = 0; j < old.baseGlyphs.size(); ++j)
            if (f.preserved || j != size_t(f.lam))
                check(sub.baseGlyphs[j].path == old.baseGlyphs[j].path, "Entry policy moved another letter body");
        for (size_t j = 0; j < old.kashidaPaths.size(); ++j)
            if (f.preserved || j != size_t(f.lam - 1))
                check(sub.kashidaPaths[j] == old.kashidaPaths[j] && sub.kashidaCuts[j] == old.kashidaCuts[j],
                      "Entry policy moved another connector or the shared alef seam");
        if (f.preserved) continue;
        const auto diagnostic = sub.letterDiagnostics[f.lam].toObject();
        check(diagnostic.value("entryPolicy") == "lam-fork-flank" && diagnostic.value("entryWalk").toDouble() == .5 &&
              sub.baseGlyphs[f.lam].path != old.baseGlyphs[f.lam].path,
              "Terminal lam did not use the half-unit fork policy");
        const auto shape = word.bodyShape(sub);
        const auto before = shape.transform.map(old.baseGlyphs[f.lam].path).boundingRect();
        const auto after = shape.transform.map(sub.baseGlyphs[f.lam].path).boundingRect();
        check(std::abs(before.top() - after.top()) < 1e-6 && std::abs(before.left() - after.left()) < 1e-6 &&
              std::abs(before.bottom() - after.bottom()) < 1e-6, "Terminal lam lost its shaft, bowl or lower return");
        if (f.page == 282) {
            const auto anchor = sub.kashidaCuts[1].second.p2();
            check(diagnostic.value("entryFork").toDouble() == 20 &&
                  QLineF(anchor, {102.0740872993966, 332.8100397868995}).length() < 1e-7,
                  "Final lam did not walk its own point-20 flank");
            const auto half = sub;
            auto custom = config;
            auto finals = custom.value("final").toArray();
            for (int i = 0; i < finals.size(); ++i) {
                auto form = finals[i].toObject();
                if (form.value("id") != "lam-bowl-fina") continue;
                // The template name and a final-only override must be data,
                // not hidden dispatch conditions in the new cutter.
                form.insert("id", "renamed-terminal-lam");
                auto cuts = form.value("cuts").toObject();
                cuts.insert("forkOffset", .25);
                form.insert("cuts", cuts);
                finals[i] = form;
            }
            custom.insert("final", finals);
            check(PageAnalysisResult::prepareLetterForms(custom).isEmpty(), "Independent final policy override rejected");
            const auto evidence = recognizeSubwordBodies(word, sub, config);
            const auto sameBody = recognizeSubwordBodies(word, sub, custom);
            check(evidence.lamProfiles.keys() == sameBody.lamProfiles.keys(), "A final cut override changed recognized bodies");
            check(a.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.sub - 1, &font, true, &custom),
                  "Renamed declarative final lam became partial");
            check(sub.letterDiagnostics[f.lam].toObject().value("template") == "renamed-terminal-lam" &&
                  sub.letterDiagnostics[f.lam].toObject().value("entryWalk").toDouble() == .25 &&
                  sub.baseGlyphs[f.lam].path != half.baseGlyphs[f.lam].path,
                  "The final template's declared cut override was not executed");
            for (int j = 0; j < f.lam; ++j)
                check(sub.baseGlyphs[j].path == half.baseGlyphs[j].path, "Final-only policy override moved another body");
        }
    }
}

static void checkForkCutPolicy() {
    struct Fixture { int page, line, word, sub, lam; QPointF fork; };
    const std::array cases{
        Fixture{2, 4, 7, 2, 2, {122.3504, 254.4515}}, // Tiny flat cap between the two flanks.
        Fixture{86, 15, 1, 1, 1, {330.706, 465.975}}, // Small reverse turn must retain the fork at the shaft.
        Fixture{88, 2, 4, 1, 2, {266.680, 117.406}}, // Tiny extremum before a flank that rises into kaf.
        Fixture{98, 9, 7, 1, 1, {147.2608, 303.3812}}, // Tight right boundary at the fork.
        Fixture{101, 7, 3, 1, 1, {253.0287, 251.251}}, // Detached final alef must not disable earlier policies.
        Fixture{131, 4, 3, 1, 1, {240.6186, 170.0878}}, // Folded foot before final yeh.
        Fixture{152, 1, 12, 1, 1, {138.6791, 90.5038}},
        Fixture{173, 4, 6, 1, 1, {149.3702, 169.8604}}, // A folded foot need not turn through 45 degrees.
        Fixture{254, 2, 4, 1, 1, {228.6079, 116.1752}},
        Fixture{398, 10, 4, 2, 2, {217.409, 331.518}}, // Recovery resolves a degenerate source winding estimate.
        Fixture{415, 11, 6, 1, 2, {180.5057, 359.9912}}, // Folded fork, initially shared entry and thick adjacent seen.
        Fixture{443, 7, 2, 1, 1, {254.3559, 251.1356}}, // Wide shaft beside a thin connector.
        Fixture{565, 3, 8, 1, 1, {121.1301, 140.6489}}, // Folded foot sharing its former exit with final yeh.
        Fixture{602, 4, 1, 1, 1, {330.699, 170.7533}}, // Smooth, concave fork.
    };
    QFile fontFile("input/font.dat");
    check(fontFile.open(QIODevice::ReadOnly), "Missing fork-policy font");
    AFont font;
    QDataStream fontStream(&fontFile);
    fontStream >> font;
    for (const auto& f : cases) {
        QFile pageFile(QString("pages/page%1.dat").arg(f.page));
        check(pageFile.open(QIODevice::ReadOnly), "Missing fork-policy page");
        PageAnalysisResult a;
        QDataStream stream(&pageFile);
        stream >> a.page;
        a.resolveWordBoundaries(f.page, &font);
        check(a.segmentSubword(f.page - 1, f.line - 1, f.word - 1, f.sub - 1, &font, true),
              "Fork cut policy left a partial subword");
        const auto& word = a.page.lines[f.line - 1].words[f.word - 1];
        const auto& sub = word.wordResultInfo.subWords[f.sub - 1];
        check(sub.kashidaCuts.size() > size_t(f.lam) && !sub.kashidaPaths[f.lam - 1].isEmpty() &&
              !sub.kashidaPaths[f.lam].isEmpty(), "Fork policy lost an adjacent connector");
        const double right = QLineF(f.fork, sub.kashidaCuts[f.lam - 1].second.p2()).length();
        const double left = QLineF(f.fork, sub.kashidaCuts[f.lam].first.p2()).length();
        if (!(right <= .51 && left <= .51 && std::max(right, left) >= .45 && coversOriginalInk(word, sub)))
            qFatal("page%03d:l%d-w%d-s%d: lām anchors must stay within the half-unit walk (right %.9f, left %.9f)",
                   f.page, f.line, f.word, f.sub, right, left);
        check(sub.letterDiagnostics[f.lam].toObject().value("cutPolicy") == "lam-fork-flanks",
              "Missing independent fork cut-policy diagnostic");
    }
}

static void checkSharedCounterForkPolicy(int page, int line, int wordIndex, int subIndex,
                                         int lamIndex, const QPointF& fork) {
    QFile pageFile(QString("pages/page%1.dat").arg(page)), fontFile("input/font.dat");
    check(pageFile.open(QIODevice::ReadOnly) && fontFile.open(QIODevice::ReadOnly), "Missing kaf-lam fork fixture");
    PageAnalysisResult a;
    AFont font;
    QDataStream pageStream(&pageFile), fontStream(&fontFile);
    pageStream >> a.page;
    fontStream >> font;
    a.resolveWordBoundaries(page, &font);
    const auto config = PageAnalysisResult::letterForms();
    auto without = config;
    auto forms = without.value("medial").toArray();
    for (int i = 0; i < forms.size(); ++i) {
        auto form = forms[i].toObject();
        if (form.value("id") != "lam-medi") continue;
        auto policy = form.value("cuts").toObject();
        policy.remove("counterJoin");
        form.insert("cuts", policy);
        forms[i] = form;
    }
    without.insert("medial", forms);
    check(a.segmentSubword(page - 1, line - 1, wordIndex - 1, subIndex - 1, &font, true, &without),
          "Unadjusted shared-counter fixture became partial");
    const auto& word = a.page.lines[line - 1].words[wordIndex - 1];
    const auto& sub = word.wordResultInfo.subWords[subIndex - 1];
    const auto old = sub;
    check(a.segmentSubword(page - 1, line - 1, wordIndex - 1, subIndex - 1, &font, true, &config),
          "Shared-counter fork policy became partial");
    const auto bridge = *std::next(sub.joins.begin(), lamIndex - 1);
    const auto oldBridge = *std::next(old.joins.begin(), lamIndex - 1);
    check(bridge.counterEntry.pathIndex == 1 && bridge.counterExit.pathIndex == 1 &&
          sub.kashidaPaths[lamIndex - 1].isEmpty() && !sub.kashidaPaths[lamIndex].isEmpty(),
          "Kaf-lam needs a shared two-bridge interface and a separate outgoing connector");
    const double distance = QLineF(fork, sub.kashidaCuts[lamIndex].first.p2()).length();
    check(distance > .49 && distance <= .5 + 1e-5, "Closed-loop lam exit did not walk half a unit from the fork");
    check(bridge.cutSegments() == oldBridge.cutSegments(), "Shared right-side bridges moved");
    for (size_t i = 0; i < sub.baseGlyphs.size(); ++i)
        if (i != size_t(lamIndex)) check(sub.baseGlyphs[i].path == old.baseGlyphs[i].path, "Kaf or unrelated body moved");
    check(sub.kashidaCuts[lamIndex].second == old.kashidaCuts[lamIndex].second && coversOriginalInk(word, sub),
          "Fork adjustment moved the following letter's boundary or lost source ink");
    check(sub.letterDiagnostics[lamIndex].toObject().value("cutPolicy") == "lam-fork-preserved-entry",
          "Shared-counter fork policy was not recorded");
    const auto bodies = sub.baseGlyphs;
    check(a.segmentSubword(page - 1, line - 1, wordIndex - 1, subIndex - 1, &font, true, &config),
          "Repeated shared-counter fork became partial");
    for (size_t i = 0; i < bodies.size(); ++i)
        check(bodies[i].path == sub.baseGlyphs[i].path, "Shared-counter fork policy is not deterministic");
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
    check(form.value("body").toObject().value("profile").toObject().value("minLandmarkCrossings").toInt() == 4, "Lam-alef crossing requirement was weakened");
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
        check(declaredSharedBoundary(context, context.form) && context.res.topBaseLine.time == 5 && context.res.bottomBaseLine.time == 14,
              "Lam-alef did not retain the exact 5/14 crossing corners");
    }
    const auto base = geometry(source, dummy, form, defaults);
    for (double scale : {0.7, 1.0, 1.6}) {
        auto scaledForm = form;
        auto scaledBody=scaledForm.value("body").toObject();
        auto bounds = scaledBody.value("neck").toObject();
        for (const auto* key : {"minX", "maxX", "minY", "maxY"})
            bounds.insert(key, bounds.value(key).toDouble() * scale);
        scaledBody.insert("neck",bounds);
        scaledForm.insert("body", scaledBody);
        const QTransform mapping(scale, 0, 0, scale, 37, -21);
        auto context = geometry(mapping.map(source), dummy, scaledForm, defaults);
        check(declaredSharedBoundary(context, context.form) && context.res.topBaseLine.time == 5 && context.res.bottomBaseLine.time == 14,
              "Lam-alef endpoint check changed under translation or page-unit scaling");
    }
    auto open = geometry(base.subPaths.front(), dummy, form, defaults);
    check(!declaredSharedBoundary(open, open.form), "Folded ligature accepted without its lower counter crossings");
    auto unrelated = geometry(source, dummy, form, defaults);
    unrelated.baseText = QStringLiteral("بإ");
    check(!declaredSharedBoundary(unrelated, unrelated.form), "Lam-alef rule accepted an unrelated letter pair");

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
        check(declaredSharedBoundary(context, context.form) && context.res.topBaseLine.time == 4 && context.res.bottomBaseLine.time == 12,
              "Attached lam-alef did not retain the exact 4/12 shared corners");
    }
    auto wrongShape = geometry(source, dummy, attachedForm, defaults);
    check(!declaredSharedBoundary(wrongShape, wrongShape.form), "Attached rule accepted an initial lam");
    auto wrongPair = geometry(attachedSource, dummy, attachedForm, defaults);
    wrongPair.baseText = QStringLiteral("فبا");
    check(!declaredSharedBoundary(wrongPair, wrongPair.form), "Attached rule accepted an unrelated suffix");
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
    checkDeclaredLamRoutes();
    checkBodyRecognitionSeparation();
    checkTerminalReturnPolicy();
    checkTaperedArmJunction();
    checkSharedAlefJunctionPolicy();
    checkTerminalLamEntryPolicy();
    checkForkCutPolicy();
    checkSharedCounterForkPolicy(296, 15, 4, 1, 2, {257.6499, 464.6648});
    checkSharedCounterForkPolicy(552, 7, 3, 1, 1, {298.6432, 249.3951});
    qInfo("Lam-alef passed: standalone and attached ligatures, separate diagonal and real-overlap rejection, ink ownership and deterministic masks.");
}
