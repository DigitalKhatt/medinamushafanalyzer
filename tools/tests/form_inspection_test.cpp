#include "pageanalysisresult.h"
#include "bodycutpanel.h"
#include <QtWidgets>

static void check(bool condition, const QString& message) {
    if (!condition) qFatal("%s", qPrintable(message));
}
static PageAnalysisResult fixture(int page) {
    QFile file(QString("pages/page%1.dat").arg(page));
    check(file.open(QIODevice::ReadOnly), "Missing inspection fixture");
    PageAnalysisResult analysis;
    QDataStream stream(&file); stream >> analysis.page;
    return analysis;
}
static void samePartition(const WordResultInfo::SubWordInfo& a, const WordResultInfo::SubWordInfo& b) {
    check(a.baseGlyphs.size() == b.baseGlyphs.size() && a.kashidaPaths == b.kashidaPaths &&
          a.kashidaCuts == b.kashidaCuts && a.letterDiagnostics == b.letterDiagnostics && a.correct == b.correct,
          "Inspection changed the partition or diagnostics");
    for (size_t i = 0; i < a.baseGlyphs.size(); ++i)
        check(a.baseGlyphs[i].path == b.baseGlyphs[i].path, "Inspection changed a body outline");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    struct Fixture { int page, line, word, sub; const char* id; };
    const Fixture fixtures[] = {
        {3,1,9,3,"heh-init"}, {3,1,6,5,"heh-body-medi"}, {3,2,5,2,"heh-counter-fina"},
        {3,1,5,1,"ain-init"}, {3,2,9,1,"ain-medi"},
        {600,2,9,1,"feh-init"}, {600,2,10,2,"feh-head-medi"}, {13,15,1,1,"feh-fina"},
        {3,2,2,2,"meem-init"}, {3,2,9,1,"meem-medi"}, {3,1,8,1,"meem-counter-fina"},
        {3,1,5,1,"lam-medi"}, {3,1,8,1,"lam-init"}, {588,5,10,1,"lam-medi"},
        {3,2,4,1,"beh-tooth-medi"}, {600,8,10,1,"beh-upright-medi"},
        {57,14,2,2,"beh-low-medi"}, {33,14,4,1,"beh-bent-medi"},
        {157,10,4,2,"beh-shoulder-medi"}, {174,6,6,2,"beh-hook-medi"}
    };
    LetterFormInspection display;
    for (const auto& f : fixtures) {
        auto analysis = fixture(f.page);
        const auto baseline = analysis.segmentSubword(f.page-1, f.line-1, f.word-1, f.sub-1, nullptr, true, &config);
        const auto expected = analysis.page.lines[f.line-1].words[f.word-1].wordResultInfo.subWords[f.sub-1];
        const auto inspection = inspectLetterForm(analysis, f.page-1, f.line-1, f.word-1, f.sub-1, nullptr, config, f.id);
        check(inspection.complete == baseline, "Inspection changed completion");
        samePartition(expected, analysis.page.lines[f.line-1].words[f.word-1].wordResultInfo.subWords[f.sub-1]);
        check(!inspection.ink.isEmpty() && !inspection.assembly.isEmpty(), "Missing preview/assembly information");
        qInfo().noquote() << f.id << "landmarks" << inspection.landmarks.size() << "cuts" << inspection.candidates.size()
                         << "attempted" << inspection.attempted;
        if (QString(f.id) == "lam-medi" && f.page == 3) {
            display = inspection;
            check(!inspection.landmarks.isEmpty() && !inspection.selectedBody.isEmpty(), "Lām evidence/selected mask missing");
        }
        if (QString(f.id) == "beh-bent-medi") {
            auto invalidPolicy = config;
            auto forms = invalidPolicy.value("medial").toArray();
            for (int i = 0; i < forms.size(); ++i) {
                auto form = forms[i].toObject();
                if (form.value("id") != f.id) continue;
                auto cuts = form.value("cuts").toObject(); cuts.insert("maxSectionWidth", .01);
                form.insert("cuts", cuts); forms[i] = form;
            }
            invalidPolicy.insert("medial", forms);
            const auto rejected = inspectLetterForm(analysis, f.page-1, f.line-1, f.word-1, f.sub-1, nullptr, invalidPolicy, f.id);
            check(std::any_of(rejected.reasons.keyBegin(), rejected.reasons.keyEnd(), [](const auto& reason) {
                return reason.contains("maxSectionWidth");
            }), "Rejected policy did not explain its section width limit");
            check(!rejected.landmarks.isEmpty(), "Policy rejection discarded the body evidence");
        }
        analysis.segmentSubword(f.page-1, f.line-1, f.word-1, f.sub-1, nullptr, true, &config);
        samePartition(expected, analysis.page.lines[f.line-1].words[f.word-1].wordResultInfo.subWords[f.sub-1]);
    }
    check(PageAnalysisResult::letterForms() == config, "Preview activated a draft globally");

    auto* full = new QPlainTextEdit;
    full->setPlainText(QString::fromUtf8(QJsonDocument(config).toJson()));
    BodyCutPanel panel(full);
    panel.resize(650, 880);
    check(panel.selectForm("beh-shoulder-medi"), "Could not select an inherited form");
    auto* cuts = panel.findChild<QPlainTextEdit*>("cutsDraft");
    auto* body = panel.findChild<QPlainTextEdit*>("bodyDraft");
    const auto originalBody = body->toPlainText();
    cuts->setPlainText("{invalid");
    QJsonObject edited; QString error;
    check(!panel.readConfig(edited, error) && error.startsWith("cuts:"), "Invalid focused JSON was accepted");
    check(!panel.selectForm("lam-medi"), "Switching templates lost an invalid edit");
    cuts->setPlainText("{\"inherit\":\"beh-bent-medi\",\"flankMode\":\"axis-projection\",\"flankOffset\":0.7}");
    check(panel.readConfig(edited, error) && PageAnalysisResult::prepareLetterForms(edited).isEmpty(), "Valid policy edit rejected");
    check(body->toPlainText() == originalBody, "Policy edit changed the body editor");
    const auto resolved = resolvedLetterForm(edited, "beh-shoulder-medi");
    check(resolved.value("cuts").toObject().value("flankOffset") == .7 &&
          resolved.value("body") == resolvedLetterForm(config, "beh-shoulder-medi").value("body"),
          "Focused editor flattened or changed inherited body settings");
    check(panel.selectForm("lam-medi"), "Could not return to lam");
    panel.setInspection(display);
    panel.show(); app.processEvents();
    const auto imageDirectory = qEnvironmentVariable("MEDINA_INSPECTOR_SCREENSHOTS");
    if (!imageDirectory.isEmpty()) {
        QDir().mkpath(imageDirectory);
        panel.grab().save(imageDirectory + "/body-view.png");
        panel.findChild<QTabWidget*>("bodyCutTabs")->setCurrentIndex(1);
        app.processEvents();
        panel.grab().save(imageDirectory + "/cut-policy-view.png");
    }
    panel.invalidate();
    check(panel.findChild<QComboBox*>("cutCandidate")->count() == 0 && panel.reviewDetails().isEmpty(),
          "Edited draft kept stale preview evidence");
    qInfo("Inspection parity, rejection reasons, inheritance, invalid drafts and preview invalidation passed");
}
