#include "mainwindow.h"
#include "bodycutpanel.h"
#include "GlobalParams.h"
#include <QtWidgets>

static void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("MedinaInspectorTest");
    QCoreApplication::setApplicationName("SegmentationDialog");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    globalParams = std::make_unique<GlobalParams>();
    const auto active = PageAnalysisResult::letterForms();
    MainWindow window;
    window.resize(1400, 950);
    window.show(); app.processEvents();
    auto buttons = window.findChildren<QPushButton*>();
    for (auto* button : buttons) if (button->text() == "Inspect cuts") button->setChecked(true);
    QGraphicsView* page = nullptr;
    for (auto* view : window.findChildren<QGraphicsView*>())
        if (view->scene() && view->scene()->property("pageNumber") == 3) page = view;
    check(page, "Page view missing");
    QGraphicsItem* body = nullptr;
    for (auto* item : page->scene()->items()) {
        if (!item->data(0).canConvert<WordMatch>()) continue;
        const auto match = item->data(0).value<WordMatch>();
        if (match.lineIndex == 0 && match.wordIndex == 4 &&
            (!body || item->boundingRect().width() > body->boundingRect().width())) body = item;
    }
    check(body, "Page 3 word 5 body missing");
    page->centerOn(body); app.processEvents();
    QPoint point;
    bool found = false;
    const auto box = body->boundingRect();
    for (int row = 1; !found && row < 100; ++row) for (int column = 1; !found && column < 100; ++column) {
        const auto p = box.topLeft() + QPointF(column * box.width()/100., row * box.height()/100.);
        if (!body->contains(p)) continue;
        const auto pixel = page->mapFromScene(body->mapToScene(p));
        if (page->items(pixel).contains(body)) { point = pixel; found = true; }
    }
    check(found, "No clickable ink point");
    bool inspected = false;
    QTimer::singleShot(150, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("segmentationDialog");
        check(dialog && dialog->isVisible(), "Click did not open the segmentation dialog");
        auto* panel = dialog->findChild<BodyCutPanel*>();
        check(panel, "Body/cut panel missing");
        auto* cuts = panel->findChild<QPlainTextEdit*>("cutsDraft");
        auto* reasons = panel->findChild<QPlainTextEdit*>("bodyCutReasons");
        check(!reasons->toPlainText().isEmpty(), "Initial preview did not run");
        QPushButton *save = nullptr, *preview = nullptr;
        for (auto* button : dialog->findChildren<QPushButton*>()) {
            if (button->text().startsWith("Save templates")) save = button;
            if (button->text() == "Preview") preview = button;
        }
        check(save && save->isEnabled() && preview, "Valid preview did not enable saving");
        const auto original = cuts->toPlainText();
        cuts->setPlainText("{invalid");
        check(!save->isEnabled() && reasons->toPlainText().isEmpty(), "Invalid edit retained a saveable/stale preview");
        preview->click();
        check(!save->isEnabled(), "Invalid JSON enabled saving");
        cuts->setPlainText(original);
        preview->click();
        check(save->isEnabled() && !reasons->toPlainText().isEmpty(), "Restored draft did not preview");
        auto* tabs = panel->findChild<QTabWidget*>("bodyCutTabs");
        const auto directory = qEnvironmentVariable("MEDINA_INSPECTOR_SCREENSHOTS");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            app.processEvents();
            dialog->grab().save(directory + "/dialog-body.png");
            tabs->setCurrentIndex(1); app.processEvents(); app.processEvents();
            dialog->grab().save(directory + "/dialog-cuts.png");
        }
        // Switch to an applicable tooth alternative from the same dialog.
        QComboBox* forms = nullptr;
        for (auto* combo : dialog->findChildren<QComboBox*>())
            if (combo->findText("beh-tooth-medi") >= 0) forms = combo;
        check(forms, "Applicable tooth form is absent from the selector");
        forms->setCurrentText("beh-tooth-medi");
        check(panel->findChild<QPlainTextEdit*>("bodyDraft")->toPlainText().contains("tooth-pair") &&
              reasons->toPlainText().contains("beh-tooth-medi"), "Switching forms did not refresh settings and evidence");
        inspected = true;
        dialog->reject();
    });
    QMouseEvent click(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(page->viewport(), &click);
    check(inspected, "Dialog interaction did not complete");
    check(PageAnalysisResult::letterForms() == active, "Closing a draft activated changes");
    qInfo("Real dialog: ink click, initial preview, invalid edit, preview recovery, form switching and cancel passed");
}
