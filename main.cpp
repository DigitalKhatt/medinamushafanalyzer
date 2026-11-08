#include <QApplication>
#include <SplashOutputDev.h>
#include "CustomOutputDev.h"
#include <QtCore/QDebug>
#include "PDFDoc.h"

#include <QCommandLineParser>
#include <QCommandLineOption>

#include "mainwindow.h"
#include "proposalexport.h"
#include "markassignment.h"

int main(int argc, char** argv) {

    Q_INIT_RESOURCE(application);
#ifdef Q_OS_ANDROID
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("DigitalKhatt");
    QCoreApplication::setApplicationName("Medina Mushaf Analyzer");
    QCoreApplication::setApplicationVersion( "0.1.000");
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::applicationName());
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"export-proposals", "Export draft vector segmentation for HTML review (no GUI).", "directory"});
    parser.addOption({"skip-template-scores", "Omit unused per-path template scores from the native proposal export."});
    parser.addOption({"export-baselines", "Export cached geometric line baselines without redoing shape matching.", "file"});
    parser.addOption({"export-transcript", "Export the current QuranTextByWord transcript without reading segmentation caches.", "file"});
    parser.addOption({"assign-marks", "Run native mark ownership/grouping on frozen native vector exports.", "directory"});
    parser.addOption({"mark-inputs", "Frozen native input directory for --assign-marks.", "directory"});
    parser.addOption({"export-mark-assignments", "Assign marks directly during native vector export.", "directory"});
    parser.addOption({"fresh-source", "Parse AI/PDF and segment from scratch instead of reading page caches."});
    parser.addOption({"test-mark-assignment", "Run native mark-assignment fixture operations from JSON.", "file"});
    parser.addOption({"test-mark-output", "Result file for native fixture operations.", "file"});
    parser.addOption({"export-letter-training", "Regenerate letter cuts and export weak masks with source provenance.", "directory"});
    parser.addOption({"segment-catalog-page", "Segment the letter bodies specified by a catalog page request.", "request.json"});
    parser.addOption({"catalog-output", "Write the catalog page partitions to this file.", "output.json"});
    parser.addOption({"svg-dir", "Read-only SVG artwork for workbench and catalog exports. Fresh mark exports can use AI/PDF alone.", "directory"});
    parser.addOption({"pages", "Pages to export: 3, 3-20, 3,7 or all.", "selection", "3"});
    parser.process(app);

    if (parser.isSet("export-mark-assignments") &&
        (!parser.isSet("export-proposals") || parser.isSet("skip-template-scores"))) {
        std::cerr << "Native mark export requires --export-proposals with template scores." << std::endl;
        return 1;
    }
    if (parser.isSet("fresh-source") && !parser.isSet("export-proposals")) {
        std::cerr << "--fresh-source belongs to native --export-proposals." << std::endl;
        return 1;
    }

    globalParams = std::make_unique<GlobalParams>();
    if (parser.isSet("segment-catalog-page")) {
        if (!parser.isSet("catalog-output")) parser.showHelp(1);
        return segmentCatalogPage(parser.value("segment-catalog-page"), parser.value("catalog-output"));
    }
    if (parser.isSet("test-mark-assignment")) {
        if (!parser.isSet("test-mark-output")) parser.showHelp(1);
        return testNativeMarkAssignment(parser.value("test-mark-assignment"), parser.value("test-mark-output"));
    }
    if (parser.isSet("assign-marks")) {
        if (!parser.isSet("mark-inputs")) parser.showHelp(1);
        return exportNativeMarkAssignments(parser.value("mark-inputs"), parser.value("assign-marks"), parser.value("pages"));
    }
    if (parser.isSet("export-transcript"))
        return exportReviewTranscript(parser.value("export-transcript"), parser.value("pages"));
    if (parser.isSet("export-letter-training"))
        return exportLetterTraining(parser.value("export-letter-training"), parser.value("pages"));
    if (parser.isSet("export-baselines"))
        return exportReviewBaselines(parser.value("export-baselines"),parser.value("pages"));

    if (parser.isSet("export-proposals")) {
        return exportReviewProposals(parser.value("export-proposals"), parser.value("svg-dir"), parser.value("pages"),
            !parser.isSet("skip-template-scores"), parser.value("export-mark-assignments"), parser.isSet("fresh-source"));
    }

    MainWindow mainWin;

    mainWin.showMaximized();

    return app.exec();



}
