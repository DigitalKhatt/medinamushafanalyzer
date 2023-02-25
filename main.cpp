#include <QApplication>
#include <SplashOutputDev.h>
#include "CustomOutputDev.h"
#include <QtCore/QDebug>
#include "PDFDoc.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>

#include "mainwindow.h"

int main(int argc, char** argv) {

    SetConsoleOutputCP(65001);
    //setlocale(LC_ALL, ".UTF8");

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
    //parser.addPositionalArgument("file", "The file to open.");
    parser.process(app);

    globalParams = std::make_unique<GlobalParams>();

    MainWindow mainWin;

    mainWin.showMaximized();

    return app.exec();



}