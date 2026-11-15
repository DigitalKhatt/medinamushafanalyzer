
#include <QtWidgets>

#include "mainwindow.h"
#include "CustomOutputDev.h"
#include "PDFDoc.h"
#include "GlobalParams.h"
#include "renderarea.h"
#include "graphicsview.h"
#include "lettersegmentationcache.h"
#include "bodycutpanel.h"
#include <iostream>
#include "qvector.h"

MainWindow::MainWindow()
{



	loadFile(fontFileName);

	createActions();
	createStatusBar();
	createDockWindows();

	readSettings();
    if (!qEnvironmentVariableIsSet("MEDINA_LETTER_FORMS")) {
        letterFormsSavePath = QSettings().value("letterFormsPath").toString();
        QFile file(letterFormsSavePath);
        if (!letterFormsSavePath.isEmpty() && file.open(QIODevice::ReadOnly)) {
            auto config = QJsonDocument::fromJson(file.readAll()).object();
            const auto problem = PageAnalysisResult::prepareLetterForms(config);
            if (problem.isEmpty()) PageAnalysisResult::activateLetterForms(config);
            else statusBar()->showMessage(tr("Saved templates could not be loaded. %1").arg(problem), 8000);
        }
    }

	setUnifiedTitleAndToolBarOnMac(true);

    const int startPage = std::clamp(QSettings().value("lastPage", 3).toInt(), 1, 604);
    if (integerSpinBox->value() == startPage) loadPage(startPage);
    else integerSpinBox->setValue(startPage);


}

void MainWindow::createDockWindows() {

	pathListDock = new QDockWidget(tr("Path List"), this);
	pathListDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

	pathListWidget = new QScrollArea(this);



	pathListDock->setWidget(pathListWidget);
	addDockWidget(Qt::LeftDockWidgetArea, pathListDock);
	viewMenu->addAction(pathListDock->toggleViewAction());

	searchDock = new QDockWidget(tr("Search"), this);
	searchDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

	addDockWidget(Qt::RightDockWidgetArea, searchDock);
	viewMenu->addAction(searchDock->toggleViewAction());



	pathListDock->hide();
    connect(pathListDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible && !pathListWidget->widget()) loadPage(integerSpinBox->value());
    });


	createSearchWindows();

	scene = new QGraphicsScene(this);
	view = new GraphicsView(this);
	view->setRenderHints(QPainter::Antialiasing);
	view->scale(4, 4);
	view->setDragMode(QGraphicsView::RubberBandDrag);

	tabWidget = new QTabWidget(this);

	tabWidget->addTab(view, "Quran Page");

	shapeScene = new QGraphicsScene(this);
	shapeView = new GraphicsView(this);
	shapeView->setRenderHints(QPainter::Antialiasing);
	shapeView->setDragMode(QGraphicsView::RubberBandDrag);

	shapeView->setScene(shapeScene);

	tabWidget->addTab(shapeView, "Shapes");


	setCentralWidget(tabWidget);


	integerSpinBox = new QSpinBox;
	integerSpinBox->setRange(1, 604);
	integerSpinBox->setSingleStep(1);
	integerSpinBox->setValue(1);
	integerSpinBox->setKeyboardTracking(false);

    connect(integerSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int number) {
        QSettings().setValue("lastPage", number);
        loadPage(number);
    });

	auto jutifyToolbar = addToolBar(tr("Quran"));
	jutifyToolbar->addWidget(integerSpinBox);

    inspectCutsButton = new QPushButton(tr("Inspect cuts"), this);
    inspectCutsButton->setCheckable(true);
    inspectCutsButton->setChecked(QSettings().value("inspectCuts", false).toBool());
    jutifyToolbar->addWidget(inspectCutsButton);
    connect(inspectCutsButton, &QPushButton::toggled, this, [this](bool enabled) {
        QSettings().setValue("inspectCuts", enabled);
        if (enabled) {
            tabWidget->setCurrentWidget(view);
            statusBar()->showMessage(tr("Click a subword on the page to inspect its cuts."));
        } else {
            statusBar()->clearMessage();
        }
    });

    showWordBBButton = new QPushButton(tr("Word boxes"), this);
    showWordBBButton->setCheckable(true);
    showWordBBButton->setChecked(QSettings().value("showWordBoxes", false).toBool());
    connect(showWordBBButton, &QPushButton::toggled, this, [this](bool checked) {
        QSettings().setValue("showWordBoxes", checked);
        loadPage(integerSpinBox->value());
    });
    jutifyToolbar->addWidget(showWordBBButton);

    showLetterSegmentationButton = new QPushButton(tr("Letter segmentation"), this);
    showLetterSegmentationButton->setCheckable(true);
    showLetterSegmentationButton->setChecked(QSettings().value("showLetterSegmentation", true).toBool());
    connect(showLetterSegmentationButton, &QPushButton::toggled, this, [this](bool checked) {
        QSettings().setValue("showLetterSegmentation", checked);
        loadPage(integerSpinBox->value());
    });
    jutifyToolbar->addWidget(showLetterSegmentationButton);



	showSubWordMarks = new QPushButton(tr("&Show subword Marks"));
	showSubWordMarks->setCheckable(true);
	showSubWordMarks->setChecked(false);

	jutifyToolbar->addWidget(showSubWordMarks);

	sordByWidth = new QPushButton(tr("&Sort by width"));
	sordByWidth->setCheckable(true);
	sordByWidth->setChecked(true);

	jutifyToolbar->addWidget(sordByWidth);




	debugBBButton = new QPushButton(tr("&Debug"));
	debugBBButton->setCheckable(true);
	debugBBButton->setChecked(false);

	connect(debugBBButton, &QPushButton::toggled, [&](bool checked) {
		loadPage(integerSpinBox->value());
	});

	jutifyToolbar->addWidget(debugBBButton);

	pointerPosition = new QLabel("Hello");

	statusBar()->addPermanentWidget(pointerPosition);

	auto button = new QPushButton(tr("&Search Stretching"));
	connect(button, &QPushButton::clicked, [&](bool checked) {
		searchStretching();
	});

	jutifyToolbar->addWidget(button);

	button = new QPushButton(tr("&Search Kashidas"));
	connect(button, &QPushButton::clicked, [&](bool checked) {
		searchKashidas();
	});

	jutifyToolbar->addWidget(button);

	auto button2 = new QPushButton(tr("&Find Expandables"));
	connect(button2, &QPushButton::clicked, [&](bool checked) {
		findExpandables();
	});

	jutifyToolbar->addWidget(button2);

	auto button3 = new QPushButton(tr("&Display All SubWords"));
	connect(button3, &QPushButton::clicked, [&](bool checked) {
		if (!maxSubWords) {
			detectSubWords(false);
		}

		for (int i = 0; i < 6; i++) {
			WordResultFlags state = (WordResultFlags)(1 << i);
			displaySubWords(-1, -1, -1, state);
		}

	});

	jutifyToolbar->addWidget(button3);

	button3 = new QPushButton(tr("&Display Current SubWords"));
	connect(button3, &QPushButton::clicked, [&](bool checked) {

		auto pageIndex = integerSpinBox->value() - 1;

		detectSubWords(true, pageIndex);


		for (int i = 0; i < 1; i++) {
			WordResultFlags state = (WordResultFlags)(1 << i);
			displaySubWords(pageIndex, -1, -1, state);
		}

	});

	jutifyToolbar->addWidget(button3);

	auto segmentSubwordsButton = new QPushButton(tr("&Segment Subwords"));
	segmentSubwordsButton->setToolTip(tr("Recompute and save letter cuts for the current page"));
	connect(segmentSubwordsButton, &QPushButton::clicked, this, [this] {
		loadPage(integerSpinBox->value(), true);
	});

	jutifyToolbar->addWidget(segmentSubwordsButton);
}

void MainWindow::closeEvent(QCloseEvent* event)
{

}

void MainWindow::open() {

}
bool MainWindow::saveFont() {
	return saveFile(fontFileName);
}

void MainWindow::savePageFile(int pageNumber, bool overrideFile) {

	auto fileName = QString("./pages/page%1.dat").arg(pageNumber);

	QFile file(fileName);

	if (!overrideFile && file.exists()) return;

	PageAnalysisResult pageResult;
	pageResult.loadPage(pageNumber, &font, false);

	file.open(QIODevice::WriteOnly);
	QDataStream out(&file);
	out << pageResult.page;
}
bool MainWindow::saveThisPage() {

	int pageNumber = integerSpinBox->value();

	savePageFile(pageNumber, true);

	pagesCache.remove(pageNumber);

	return true;

}

static void drawPath(QTextStream& out, const QPainterPath& path, bool newPath, QString classes, QString fill = "") {
	if (newPath) {
		out << "<path ";
		if (!classes.isEmpty()) {
			out << "class=\"" << classes << "\" ";
		}
		if (!fill.isEmpty()) {
			out << "fill=\"" << fill << "\" ";
		}
		out << "d = \"";
	}
	for (int i = 0; i < path.elementCount(); ++i) {
		const QPainterPath::Element& e = path.elementAt(i);
		switch (e.type) {
		case QPainterPath::MoveToElement:
			out << 'M' << e.x << ',' << e.y;
			break;
		case QPainterPath::LineToElement:
			out << 'L' << e.x << ',' << e.y;
			break;
		case QPainterPath::CurveToElement:
			out << 'C' << e.x << ',' << e.y;
			++i;
			while (i < path.elementCount()) {
				const QPainterPath::Element& e = path.elementAt(i);
				if (e.type != QPainterPath::CurveToDataElement) {
					--i;
					break;
				}
				else
					out << ' ';
				out << e.x << ',' << e.y;
				++i;
			}
			break;
		default:
			break;
		}
		if (i != path.elementCount() - 1) {
			out << ' ';
		}
	}
	if (newPath) {
		out << "\"/>" << Qt::endl;
	}
}

struct Color
{
	int r, g, b;

	QString toCss() {
		return QString("#%1%2%3").arg(r, 2, 16, QLatin1Char('0')).arg(g, 2, 16, QLatin1Char('0')).arg(b, 2, 16, QLatin1Char('0'));
	}
};
static int blend(int a, int b, float alpha)
{
	return (1.f - alpha) * a + alpha * b;
}

static Color color_blend(Color a, Color b, float alpha)
{
	struct Color x;

	x.r = blend(a.r, b.r, alpha);
	x.g = blend(a.g, b.g, alpha);
	x.b = blend(a.b, b.b, alpha);

	return x;
}

static QString numToSvg(qreal v)
{
    return QString::number(v, 'f', 3);
}

static QString painterPathToSvgD(const QPainterPath& path)
{
    QString d;
    QTextStream s(&d);

    int i = 0;
    while (i < path.elementCount()) {
        const QPainterPath::Element e = path.elementAt(i);

        if (e.isMoveTo()) {
            s << "M " << numToSvg(e.x) << " " << numToSvg(e.y) << " ";
            ++i;
        } else if (e.isLineTo()) {
            s << "L " << numToSvg(e.x) << " " << numToSvg(e.y) << " ";
            ++i;
        } else if (e.type == QPainterPath::CurveToElement) {
            if (i + 2 >= path.elementCount())
                break;

            const QPainterPath::Element c1 = path.elementAt(i);
            const QPainterPath::Element c2 = path.elementAt(i + 1);
            const QPainterPath::Element end = path.elementAt(i + 2);

            s << "C "
              << numToSvg(c1.x) << " " << numToSvg(c1.y) << " "
              << numToSvg(c2.x) << " " << numToSvg(c2.y) << " "
              << numToSvg(end.x) << " " << numToSvg(end.y) << " ";

            i += 3;
        } else {
            ++i;
        }
    }

    if (!d.isEmpty())
        d += "Z";

    return d.trimmed();
}

static QRectF computePathsBounds(const std::vector<QPainterPath>& paths)
{
    QRectF bounds;
    bool first = true;

    for (const auto& path : paths) {
        if (path.isEmpty())
            continue;

        QRectF b = path.boundingRect();
        if (first) {
            bounds = b;
            first = false;
        } else {
            bounds = bounds.united(b);
        }
    }

    return bounds;
}

bool writePathsToSvg(const std::vector<QPainterPath>& paths,
                     const QString& fileName,
                     bool flipY = false,
                     qreal margin = 0.0)
{
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    std::vector<QPainterPath> outPaths;
    outPaths.reserve(paths.size());

    for (const auto& p : paths) {
        if (p.isEmpty())
            continue;

        QPainterPath out = p;
        if (flipY) {
            out = QTransform::fromScale(1.0, -1.0).map(out);
        }
        outPaths.push_back(out);
    }

    QRectF bounds = computePathsBounds(outPaths);
    if (bounds.isEmpty())
        bounds = QRectF(0, 0, 100, 100);

    const qreal minX = bounds.left() - margin;
    const qreal minY = bounds.top() - margin;
    const qreal width = bounds.width() + 2 * margin;
    const qreal height = bounds.height() + 2 * margin;

    QTextStream ts(&file);
    ts.setCodec("UTF-8");

    ts << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ts << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
       << "version=\"1.1\" "
       << "viewBox=\""
       << numToSvg(minX) << " "
       << numToSvg(minY) << " "
       << numToSvg(width) << " "
       << numToSvg(height) << "\">\n";

    ts << "  <g fill=\"black\" stroke=\"none\">\n";

    for (size_t i = 0; i < outPaths.size(); ++i) {
        const QString d = painterPathToSvgD(outPaths[i]);
        if (d.isEmpty())
            continue;

        ts << "    <path id=\"path_" << i << "\" d=\"" << d << "\"/>\n";
    }

    ts << "  </g>\n";
    ts << "</svg>\n";

    return true;
}

void MainWindow::exportSurahNames(){

	QString suraWord = "سُورَةُ";

	QString surapattern = "^("
		+ suraWord + " .*"	
		+ ")$";

	QRegularExpression re(surapattern, QRegularExpression::MultilineOption);

	int pageNumber = 0;
	
	std::map<int,std::vector<bool>> linesWithSurah;

	std::vector<int> pagesWithSurah;
	

	for (auto textLines : PageAnalysisResult::quranText) {
		pageNumber++;		
		bool exists = false;
		for (int i = 0; i < textLines.size(); i++) {
			auto textLine = textLines[i];
			auto match = re.match(textLine);
			if (match.hasMatch()) {
				linesWithSurah[pageNumber].push_back(true);	
				exists = true;			
			} else  {
				linesWithSurah[pageNumber].push_back(false);			
			}
		}
		if(exists){
			pagesWithSurah.push_back(pageNumber);
		}
	}

	std::vector<QPainterPath> surahs;

	for(auto pageNumber : pagesWithSurah){		

		//if (pageNumber > 3) break;
		PageAnalysisResult pageResult;
		pageResult.loadPage(pageNumber, &font, true);	
		auto& lines = linesWithSurah[pageNumber]	;

		for (int i = 0; i < pageResult.page.lines.size(); i++) {
			if(!lines[i]) continue;

			QPainterPath path;
			auto& line = pageResult.page.lines[i];
			for(auto& word :  line.words){
				for(auto& apath: word.paths){
					path.addPath(apath.path);
				}
			}
			surahs.push_back(path);
		}
	}
	
	auto fileName = QString("./svg/surahs.svg");

	writePathsToSvg(surahs, fileName, true);
	

}

bool MainWindow::exportPageToSVG(int pageNumber) {

	auto fileName = QString("./svg/page%1.svg").arg(pageNumber);


	QFile file(fileName);

	double ayaWidth = 13.04205;
	double ayaHeight = 17.036715;

	double width = 260;
	double height = 410;
	double leftViewBox = 40;
	if (pageNumber % 2 == 0) {
		leftViewBox = 85;
	}
	QRegExp reNumber("\\d*");
	bool openend = file.open(QIODevice::WriteOnly);
	if (openend) {
		int pageIndex = pageNumber - 1;
		QTextStream  out(&file);
		out << "<?xml-stylesheet type=\"text/css\" href=\"svg-stylesheet.css\"?>" << Qt::endl;
		out << "<svg version=\"1.1\"" << Qt::endl;
		out << "width=\"" << width * 8 << "\" height=\"" << height * 8 << "\"" << Qt::endl;
		out << "viewBox=\"" << leftViewBox << " 70 " << width << " " << height << "\"" << Qt::endl;
		out << "onload=\"makeDraggable(evt)\"" << Qt::endl;
		out << "xmlns=\"http://www.w3.org/2000/svg\"" << Qt::endl;
		out << "xmlns:xlink=\"http://www.w3.org/1999/xlink\">" << Qt::endl;
		out << "<script xlink:href=\"myscript.js\"/>" << Qt::endl;
		//out<< "<style>" <<  Qt::endl;
		//out << "<![CDATA[" << Qt::endl;
		//out << ".lgray {fill:   #999999;}" << Qt::endl;
		//out << "]]>" << Qt::endl;
		//out << "</style>" << Qt::endl;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto& line = page.page.lines[lineIndex];
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
				auto& word = line.words[wordIndex];
				auto wordText = page.QuranTextByWord[pageIndex][lineIndex][wordIndex];
				if (reNumber.exactMatch(wordText)) {
					QPainterPath wordPath;
					for (auto& shape : word.paths) {
						auto itemPath = shape.path;
						auto pos = shape.pos;
						itemPath = itemPath * shape.transform;
						itemPath.translate(pos.x(), pos.y());
						wordPath.addPath(itemPath);
					}
					auto bbox = wordPath.boundingRect();

					double heightop = 1;

					double x = bbox.left() - (ayaWidth - bbox.width()) / 2;
					double y = bbox.top() - (ayaHeight - heightop - bbox.height()) / 2 - heightop;

					out << "<use href=\"aya.svg#aya\" x=\"" << x << "\" y=\"" << y << "\"/>" << Qt::endl;
				}
				page.detectSubWords(pageIndex, lineIndex, wordIndex, &font);
				for (int subWordIndex = 0; subWordIndex < word.wordResultInfo.subWords.size(); subWordIndex++) {
					auto& subWord = word.wordResultInfo.subWords[subWordIndex];
					page.segmentSubword(pageIndex, lineIndex, wordIndex, subWordIndex, &font);
					QString classes = "draggable";
					classes = "";
					if (subWord.text == "و۟" || subWord.text == "ا۟") {
						classes += " lgray";
					}
					else if (subWord.text.contains("ٓ")) {
						//classes += " medd6saturated";
					}


					Color start{ 255,0,0 };
					Color end{ 0,0,255 };
					float step = subWord.baseGlyphs.size() != 0 ? 1.0 / subWord.baseGlyphs.size() : 1;
					float alpha = 0;
					int iter = 0;
					auto& shape = word.paths[subWord.paths[0]];
					for (auto baseGlyphInfo : subWord.baseGlyphs) {
						QPainterPath baseGlyph;
						auto itemPath = baseGlyphInfo.path;
						auto pos = shape.pos;
						itemPath = itemPath * shape.transform;
						itemPath.translate(pos.x(), pos.y());
						auto color = color_blend(start, end, alpha);
						auto fill = color.toCss();
						fill = iter % 2 ? "#3200cc" : "#17B169";
						//fill = "";
						drawPath(out, itemPath, true, classes, fill);
						alpha += step;
						iter++;
					}


                    for (const auto& kashida : subWord.kashidaPaths) {
                        if (kashida.isEmpty()) continue;
                        auto itemPath = shape.transform.map(kashida);
                        itemPath.translate(shape.pos);
                        drawPath(out, itemPath, true, classes + " kashida", "#df8b27");
                    }


					for (int i = 1; i < subWord.paths.size(); i++) {
						if (subWord.paths[i] == subWord.secondaryBodyPath) continue;
						QPainterPath marksPath;
						auto& shape = word.paths[subWord.paths[i]];
						auto itemPath = shape.path;
						auto pos = shape.pos;
						itemPath = itemPath * shape.transform;
						itemPath.translate(pos.x(), pos.y());
						marksPath.addPath(itemPath);
						drawPath(out, marksPath, true, classes);
					}
				}

			}
		}



		out << "</svg>";
	}


	return true;
}
bool MainWindow::saveAllPages() {

	auto pool = QThreadPool::globalInstance();

	auto nbThread = pool->maxThreadCount();

	std::cout << "nbThread = " << nbThread << std::endl;

	auto start = std::chrono::system_clock::now();
	std::time_t start_time = std::chrono::system_clock::to_time_t(start);

	std::cout << "started computation at " << std::ctime(&start_time) << std::endl;

	class MyTask : public QRunnable
	{
	public:
		int begin;
		int pageperthread;
		MainWindow* main;
		MyTask(MainWindow* main, int begin, int pageperthread) : begin{ begin }, pageperthread{ pageperthread }, main{ main } {}
		void run() override
		{
			for (int i = begin; i < begin + pageperthread; i++) {
				int pageNumber = i + 1;
				auto start = std::chrono::system_clock::now();
				main->savePageFile(pageNumber, false);
				auto end = std::chrono::system_clock::now();
				std::chrono::duration<double> elapsed = end - start;
				std::cout << "finished page number " << pageNumber << " after " << int(elapsed.count() / 60) << " min " << (int)elapsed.count() % 60 << " sec\n";
			}
		}
	};
	/*
	int i = 0;
	auto last = start;
	while (i < 604) {
		for (int nb = 0; nb < nbThread; nb++) {
			pool->start(new MyTask(this, i, 1));
			i++;
		}
		pool->waitForDone();
		auto fin = std::chrono::system_clock::now();
		std::chrono::duration<double> elapsed = fin - last;
		std::cout << "finished " << nbThread << " pages " << " after " << int(elapsed.count() / 60) << " min " << (int)elapsed.count() % 60 << " sec\n";
		elapsed = fin - start;
		std::cout << "finished " << i << " pages " << " after " << int(elapsed.count() / 60) << " min " << (int)elapsed.count() % 60 << " sec\n";
		last = fin;
	}*/


	for (int i = 0; i < 604; i++) {
		pool->start(new MyTask(this, i, 1));
	}



	auto ret = pool->waitForDone();

	auto end = std::chrono::system_clock::now();

	std::chrono::duration<double> elapsed_seconds = end - start;
	std::time_t end_time = std::chrono::system_clock::to_time_t(end);

	std::cout << "finished computation at " << std::ctime(&end_time) << " elapsed time: " << int(elapsed_seconds.count() / 60) << " min " << (int)elapsed_seconds.count() % 60 << " sec\n";

	return ret;

}
bool MainWindow::saveAll() {
	saveFont();
	saveAllPages();

	return true;
}
void MainWindow::about() {
	QMessageBox::about(this, tr("About Application"),
		tr("The <b>Application</b> example demonstrates how to "
			"write modern GUI applications using Qt, with a menu bar, "
			"toolbars, and a status bar."));
}
void MainWindow::createActions() {

	fileMenu = menuBar()->addMenu(tr("&File"));
	QToolBar* fileToolBar = addToolBar(tr("File"));



	QAction* saveAction = new QAction(tr("&Save this page"), this);
	saveAction->setStatusTip(tr("Save this page to disk"));
	connect(saveAction, &QAction::triggered, this, &MainWindow::saveThisPage);
	fileMenu->addAction(saveAction);
	fileToolBar->addAction(saveAction);

	saveAction = new QAction(tr("&Save font"), this);
	saveAction->setStatusTip(tr("Save font"));
	connect(saveAction, &QAction::triggered, this, &MainWindow::saveFont);
	fileMenu->addAction(saveAction);
	fileToolBar->addAction(saveAction);

	saveAction = new QAction(tr("&Save all pages"), this);
	saveAction->setStatusTip(tr("Save all pages"));
	connect(saveAction, &QAction::triggered, this, &MainWindow::saveAllPages);
	fileMenu->addAction(saveAction);
	fileToolBar->addAction(saveAction);

	saveAction = new QAction(tr("&Export this page to SVG"), this);
	saveAction->setStatusTip(tr("Export this page to SVG"));
	connect(saveAction, &QAction::triggered, [this]() {
		int pageNumber = this->integerSpinBox->value();
		this->exportPageToSVG(pageNumber);
	});
	fileMenu->addAction(saveAction);
	fileToolBar->addAction(saveAction);

    auto* exportSegmentation = fileMenu->addAction(tr("Export letter segmentation as JSON…"));
    connect(exportSegmentation, &QAction::triggered, this, [this] {
        const int number = integerSpinBox->value();
        const auto filename = QFileDialog::getSaveFileName(this, tr("Export letter segmentation"),
            QString("page%1-segmentation.json").arg(number, 3, 10, QLatin1Char('0')), tr("JSON files (*.json)"));
        if (filename.isEmpty()) return;
        const auto data = loadLetterSegmentation(getPageResult(number), number, font);
        QString error;
        if (!saveLetterSegmentation(filename, data, &error)) {
            QMessageBox::warning(this, tr("Export failed"), error);
        } else {
            statusBar()->showMessage(tr("Exported segmentation to %1").arg(filename), 6000);
        }
    });

	saveAction = new QAction(tr("&Export Surah Names"), this);
	saveAction->setStatusTip(tr("Export Surah Names"));
	connect(saveAction, &QAction::triggered, [this]() {		
		this->exportSurahNames();
	});
	fileMenu->addAction(saveAction);



	fileMenu->addSeparator();

	const QIcon exitIcon = QIcon::fromTheme("application-exit");
	QAction* exitAct = fileMenu->addAction(exitIcon, tr("E&xit"), this, &QWidget::close);
	exitAct->setShortcuts(QKeySequence::Quit);

	exitAct->setStatusTip(tr("Exit the application"));

	viewMenu = menuBar()->addMenu(tr("&View"));

	QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
	QAction* aboutAct = helpMenu->addAction(tr("&About"), this, &MainWindow::about);
	aboutAct->setStatusTip(tr("Show the application's About box"));

	QAction* aboutQtAct = helpMenu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
	aboutQtAct->setStatusTip(tr("Show the Qt library's About box"));

}

void MainWindow::createStatusBar() {
	statusBar()->showMessage(tr("Ready"));
}

void MainWindow::readSettings() {
	QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());
	const QByteArray geometry = settings.value("geometry", QByteArray()).toByteArray();
	if (geometry.isEmpty()) {
		const QRect availableGeometry = screen()->availableGeometry();
		resize(availableGeometry.width() / 3, availableGeometry.height() / 2);
		move((availableGeometry.width() - width()) / 2,
			(availableGeometry.height() - height()) / 2);
	}
	else {
		restoreGeometry(geometry);
	}
}

void MainWindow::writeSettings() {
	QSettings settings(QCoreApplication::organizationName(), QCoreApplication::applicationName());
	settings.setValue("geometry", saveGeometry());
}


void MainWindow::loadFile(const QString& fileName)
{
	QFile file(fileName);
	if (file.open(QIODevice::ReadOnly)) {
		QDataStream in(&file);   // we will serialize the data into the file
		in >> font;
	}


}

void MainWindow::setMousePosition(const QPointF& point) {
	QString string = QString("%1, %2")
		.arg(point.x() * 100 / constants::SCALE_GLYPH)
		.arg(point.y() * 100 / constants::SCALE_GLYPH);
	pointerPosition->setText(string);
}

bool MainWindow::saveFile(const QString& fileName)
{

	QFile file(fileName);
	file.open(QIODevice::WriteOnly);
	QDataStream out(&file);   // we will serialize the data into the file
	out << font;

	return true;
}

void MainWindow::scaleView(qreal scaleFactor)
{

	QWidget* fw = qApp->focusWidget();

	QGraphicsView* gg = nullptr;

	if (fw == view) {
		gg = view;
	}
	else if (fw == shapeView) {
		gg = shapeView;
	}

	if (gg != nullptr) {
		qreal factor = gg->transform().scale(scaleFactor, scaleFactor).mapRect(QRectF(0, 0, 1, 1)).width();
		if (factor < 0.07 || factor > 100)
			return;

		gg->scale(scaleFactor, scaleFactor);
	}




}
void MainWindow::zoomIn()
{
	scaleView(qreal(1.2));
}

void MainWindow::zoomOut()
{
	scaleView(1 / qreal(1.2));
}
void MainWindow::keyPressEvent(QKeyEvent* event)
{
	//centerOn(contour);
	switch (event->key()) {
	case Qt::Key_Plus:
		zoomIn();
		break;
	case Qt::Key_Minus:
		zoomOut();
		break;
	case Qt::Key_Space:
	default:
		QMainWindow::keyPressEvent(event);
	}
}
void MainWindow::createSearchWindows() {

	checkBoxMarks = new QCheckBox(tr("Marks"));
	checkBoxIsol = new QCheckBox(tr("Isol"));

	connect(checkBoxIsol, &QCheckBox::toggled, [&](bool checked) {
		//checkBoxInit->setEnabled(!checked);
		//checkBoxMedi->setEnabled(!checked);
		//checkBoxFina->setEnabled(!checked);
	});

	checkBoxInit = new QCheckBox(tr("Init"));
	checkBoxMedi = new QCheckBox(tr("Medi"));
	checkBoxFina = new QCheckBox(tr("Fina"));
	checkBoxShapes = new QCheckBox(tr("Shapes"));
	checkBoxSubWords = new QCheckBox(tr("Subwords"));


	checkBoxIsol->setChecked(true);
	checkBoxInit->setChecked(true);
	checkBoxMedi->setChecked(true);
	checkBoxFina->setChecked(true);
	checkBoxShapes->setChecked(true);
	checkBoxSubWords->setChecked(true);

	QHBoxLayout* hbox1 = new QHBoxLayout;

	hbox1->addWidget(checkBoxMarks);
	hbox1->addWidget(checkBoxShapes);
	hbox1->addWidget(checkBoxSubWords);
	hbox1->addStretch(1);

	QHBoxLayout* hbox2 = new QHBoxLayout;
	hbox2->addWidget(checkBoxIsol);
	hbox2->addWidget(checkBoxInit);
	hbox2->addWidget(checkBoxMedi);
	hbox2->addWidget(checkBoxFina);
	hbox2->addStretch(1);

	QVBoxLayout* vbox = new QVBoxLayout;

	vbox->addLayout(hbox1);
	vbox->addLayout(hbox2);

	QVBoxLayout* vbox1 = new QVBoxLayout;

	vbox1->addLayout(vbox);

	searchTextLineEdit = new QLineEdit();
	vbox1->addWidget(searchTextLineEdit);

	QPushButton* searchButton = new QPushButton(tr("&Search"));
	vbox1->addWidget(searchButton);

	connect(searchButton, &QPushButton::clicked, this, &MainWindow::searchText);

	vbox1->addStretch(1);

	auto widget = new QWidget(this);
	widget->setLayout(vbox1);

	widget->setMinimumWidth(500);

	searchDock->setWidget(widget);



}
static APage loadCache(QString cacheFileName)
{
	APage page;
	QFile file(cacheFileName);
	if (file.open(QIODevice::ReadOnly)) {
		QDataStream in(&file);   // we will serialize the data into the file
		in >> page;
	}

	return page;
}

PageAnalysisResult& MainWindow::getPageResult(int pageNumber) {
	bool cached = pagesCache.contains(pageNumber);

	if (!cached) {
		QString cacheFileName = QString("./pages/page%1.dat").arg(pageNumber);

		QFile cacheFile(cacheFileName);
		if (cacheFile.exists()) {
			pagesCache[pageNumber].page = loadCache(cacheFileName);
			pagesCache[pageNumber].resolveWordBoundaries(pageNumber, &font);
		}
		else {
			auto& tt = pagesCache[pageNumber];
			tt.loadPage(pageNumber, &font, false);
		}
	}

	return pagesCache[pageNumber];
}


void MainWindow::searchText() {

	auto textToSearch = searchTextLineEdit->text().trimmed();

	if (textToSearch.isEmpty()) {
		QMessageBox::information(this, tr("Info"),
			QString("Please enter the text to search"));
		return;
	}

	if (!checkBoxInit->isChecked() && !checkBoxFina->isChecked() && !checkBoxMedi->isChecked() && !checkBoxIsol->isChecked()) {
		QMessageBox::information(this, tr("Info"),
			QString("Please specify at least Isol, Init, Med or Fina"));
		return;
	}

	auto matches = quransearch.searchText(textToSearch,
		checkBoxMarks->isChecked(),
		checkBoxSubWords->isChecked(),
		checkBoxShapes->isChecked(),
		checkBoxIsol->isChecked(),
		checkBoxInit->isChecked(),
		checkBoxMedi->isChecked(),
		checkBoxFina->isChecked()
	);

	displaySearch(textToSearch, matches, sordByWidth->isChecked());

	statusBar()->showMessage(QString("Nb words found = %1").arg(matches.size()));



}

void MainWindow::searchKashidas() {

	detectSubWords(true);

	std::multimap<qreal, WordMatch, std::greater <qreal> > results;

	for (int pageIndex = 0; pageIndex < PageAnalysisResult::QuranTextByWord.size(); pageIndex++) {
		int pageNumber = pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto line = page.page.lines[lineIndex];
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
				auto& word = line.words[wordIndex];
				for (auto subWord = word.wordResultInfo.subWords.begin(); subWord != word.wordResultInfo.subWords.end(); ++subWord) {
					if (subWord->baseGlyphs.size() < 2) continue;
					if (subWord->correct && (subWord->joins.size() + 1) == subWord->baseGlyphs.size()) {
                        const auto& shape = word.paths[subWord->paths[0]];
                        const auto subwordIndex = static_cast<int>(subWord - word.wordResultInfo.subWords.begin());
                        for (size_t index = 0; index < subWord->kashidaPaths.size(); ++index) {
                            const auto& kashida = subWord->kashidaPaths[index];
                            if (kashida.isEmpty()) continue;
                            const auto& endpoints = subWord->kashidaCuts[index];
                            const double span = std::abs(endpoints.first.center().x() - endpoints.second.center().x());
                            auto itemPath = shape.transform.map(kashida);
                            itemPath.translate(shape.pos);
                            results.insert({span, WordMatch{pageIndex, lineIndex, wordIndex, subwordIndex, {}, itemPath}});
                        }
					}
					else {
						QPainterPath subWordPath;
						const auto shape = word.bodyShape(*subWord);
						auto itemPath = shape.path;
						auto pos = shape.pos;
						itemPath = itemPath * shape.transform;
						itemPath.translate(pos.x(), pos.y());
						subWordPath.addPath(itemPath);
						results.insert({ 0, WordMatch{ pageIndex ,lineIndex,wordIndex,static_cast<int>(subWord - word.wordResultInfo.subWords.begin()),{},subWordPath } });
					}
				}
			}
		}
	}

	/*
	for (int pageIndex = 0; pageIndex < PageAnalysisResult::QuranTextByWord.size(); pageIndex++) {
		int pageNumber = pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto line = page.page.lines[lineIndex];

			// find max distance between y extremmum
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
				auto& word = line.words[wordIndex];
				for (auto it = word.wordResultInfo.subWords.rbegin(); it != word.wordResultInfo.subWords.rend(); ++it) {
					auto& subWord = *it;
					QPainterPath subWordPath;
					QPainterPath maxSubWordPath;

					qreal maxDist = 0.0;
					int i = 0;
					for (auto shapeIndex : subWord.paths) {
						if (i > 0) {
							break;
						}
						auto& shape = word.paths[shapeIndex];
						auto itemPath = shape.path;
						auto pos = shape.pos;
						itemPath = itemPath * shape.transform;
						itemPath.translate(pos.x(), pos.y());
						subWordPath.addPath(itemPath);
						qreal maxDistShape = 0.0;
						std::vector<QPainterPath> subPaths;
						QVector<PathExtrema> extr;
						ShapeItem::getExtrema(itemPath, subPaths, extr);
						std::sort(extr.begin(), extr.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.x() > b.point.x(); });
						for (int extIndex = 1; extIndex < extr.length(); extIndex++) {
							if (!extr[extIndex].isX && std::abs(extr[extIndex].point.y() - line.baseline) < 3) {
								qreal diff = extr[extIndex - 1].point.x() - extr[extIndex].point.x();
								if (diff > maxDistShape) {
									maxDistShape = diff;
								}
							}

						}
						if (maxDistShape > maxDist || i == 0) {
							maxDist = maxDistShape;
							maxSubWordPath = subWordPath;
						}
						i++;
					}

					results.insert({ sordByWidth->isChecked() ? maxDist : maxSubWordPath.boundingRect().width(), WordMatch{ pageIndex ,lineIndex,wordIndex,0,{},maxSubWordPath } });
				}
			}
		}
	}
	*/

	showResuts("St Sub", results);

}


void MainWindow::searchStretching() {

	std::multimap<qreal, WordMatch, std::greater <qreal> > results;

	for (int pageIndex = 0; pageIndex < PageAnalysisResult::QuranTextByWord.size(); pageIndex++) {
		int pageNumber = pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto line = page.page.lines[lineIndex];

			// find max distance between y extremmum
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
				auto& word = line.words[wordIndex];
				for (auto& shape : word.paths) {
					auto pos = shape.pos;
					auto itemPath = shape.path;
					itemPath = itemPath * shape.transform;
					itemPath.translate(pos.x(), pos.y());

					Stretchings stretchings;
					ShapeItem::searchStretchings(itemPath, stretchings);

					for (auto& stretching : stretchings) {
						results.insert({ stretching.length, WordMatch{ pageIndex ,lineIndex,wordIndex } });
					}
				}


			}
		}
	}


	showResuts("Kashidas", results);

}

void MainWindow::displaySubWords(int ppageIndex, int plineIndex, int pwordIndex, WordResultFlags state) {
	QTableWidget* tableWidget = new	QTableWidget(this);
	tableWidget->setItemDelegate(new PathDelegate);
	tableWidget->horizontalHeader()->hide();
	tableWidget->setColumnCount(2 + (maxSubWords == 0 ? 10 : maxSubWords));
	tableWidget->horizontalHeader()->setResizeContentsPrecision(-1);
	tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);

	connect(tableWidget, &QTableWidget::cellClicked, [this, tableWidget](int row, int column) {


		auto item = tableWidget->item(row, column);

		if (item == nullptr) return;

		auto vardata = item->data(Qt::UserRole);

		if (vardata.canConvert<WordMatch>()) {
			auto data = qvariant_cast<WordMatch>(vardata);
			int pageNumber = data.pageIndex + 1;
			auto& page = getPageResult(pageNumber);
			if (column == 0) {
				auto word = page.page.lines[data.lineIndex].words[data.wordIndex];
				QPainterPath wordPath;
				for (auto shape : word.paths) {
					auto itemPath = shape.path;
					auto pos = shape.pos;
					itemPath = itemPath * shape.transform;
					itemPath.translate(pos.x(), pos.y());
					wordPath.addPath(itemPath);
				}

				auto bbox = wordPath.boundingRect();
				wordPath.translate(-bbox.x(), -bbox.y());

				QTransform transform{ constants::SCALE_GLYPH,0,0,constants::SCALE_GLYPH,0,0 };


				delete shapeScene;

				shapeScene = new QGraphicsScene(this);

				this->shapeScene->addItem(new ShapeItem(wordPath * transform));

				shapeView->setScene(shapeScene);

				/*
				this->shapeScene->clear();
				this->shapeScene->addItem(new ShapeItem(wordPath * transform));

				this->shapeScene->setSceneRect(QRectF());
				this->shapeView->setSceneRect(QRectF());*/


			}
			else if (column == 1) {
				integerSpinBox->setValue(pageNumber);
			}
			else if (column > 1) {

				page.segmentSubword(data.pageIndex, data.lineIndex, data.wordIndex, data.subWordIndex, &font, true);

				auto& word = page.page.lines[data.lineIndex].words[data.wordIndex];


				this->shapeScene->clear();

				this->shapeScene->setSceneRect(QRectF());

				this->shapeScene->addItem(new WordItem(word, data.subWordIndex));

			}

		}


	});

	int maxIndex = ppageIndex == -1 ? PageAnalysisResult::QuranTextByWord.size() : ppageIndex + 1;

	int rowIndex = 0;
	for (int pageIndex = ppageIndex == -1 ? 0 : ppageIndex; pageIndex < maxIndex; pageIndex++) {
		int pageNumber = pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto line = page.page.lines[lineIndex];
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {


				if (!((plineIndex == -1 || plineIndex == lineIndex) && (pwordIndex == -1 || pwordIndex == wordIndex))) continue;


				page.detectSubWords(pageIndex, lineIndex, wordIndex, &font);
				auto& word = line.words[wordIndex];

				if ((word.wordResultInfo.state & state) != state) continue;

				QPainterPath wordPath;
				for (auto shape : word.paths) {
					auto itemPath = shape.path;
					auto pos = shape.pos;
					itemPath = itemPath * shape.transform;
					itemPath.translate(pos.x(), pos.y());
					wordPath.addPath(itemPath);
				}

				WordMatch match{ pageIndex,lineIndex,wordIndex };

				tableWidget->insertRow(rowIndex);

				auto pathItem = new QTableWidgetItem();
				pathItem->setData(Qt::DisplayRole, QVariant::fromValue(PathImage(wordPath)));
				pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
				tableWidget->setItem(rowIndex, 0, pathItem);

				pathItem = new QTableWidgetItem();
				pathItem->setData(Qt::DisplayRole, QString("%1-%2-%3").arg(pageNumber).arg(match.lineIndex + 1).arg(match.wordIndex + 1));
				pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
				tableWidget->setItem(rowIndex, 1, pathItem);
				int colIndex = 2;
				int subWordIndex = word.wordResultInfo.subWords.size() - 1;;
				for (auto it = word.wordResultInfo.subWords.rbegin(); it != word.wordResultInfo.subWords.rend(); ++it) {
					auto& subWord = *it;
					QPainterPath subWordPath;
					if (showSubWordMarks->isChecked()) {
						for (auto shapeIndex : subWord.paths) {
							if (shapeIndex == subWord.secondaryBodyPath) continue;
							const auto shape = shapeIndex == subWord.paths.front() ? word.bodyShape(subWord) : word.paths[shapeIndex];
							auto itemPath = shape.path;
							auto pos = shape.pos;
							itemPath = itemPath * shape.transform;
							itemPath.translate(pos.x(), pos.y());
							subWordPath.addPath(itemPath);
						}
					}
					else if (subWord.paths.size() > 0) {
						const auto shape = word.bodyShape(subWord);
						auto itemPath = shape.path;
						auto pos = shape.pos;
						itemPath = itemPath * shape.transform;
						itemPath.translate(pos.x(), pos.y());
						subWordPath.addPath(itemPath);
					}


					pathItem = new QTableWidgetItem();
					pathItem->setData(Qt::DisplayRole, QVariant::fromValue(PathImage(subWordPath)));
					match.subWordIndex = subWordIndex;
					pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
					tableWidget->setItem(rowIndex, colIndex, pathItem);

					colIndex++;
					subWordIndex--;

				}
				rowIndex++;
			}
		}
	}



	tableWidget->resizeColumnsToContents();
	tableWidget->resizeRowsToContents();

	auto newDock = new QDockWidget(QString("SubWord Result%1").arg((int)state), this);
	newDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

	newDock->setAttribute(Qt::WA_DeleteOnClose);

	addDockWidget(Qt::LeftDockWidgetArea, newDock);
	viewMenu->addAction(newDock->toggleViewAction());

	newDock->setWidget(tableWidget);

	tabifyDockWidget(pathListDock, newDock);
	newDock->show();
	newDock->raise();
}

void MainWindow::detectSubWords(bool characterSegmentation, int pPageIndex) {

	int maxIndex = pPageIndex == -1 ? PageAnalysisResult::QuranTextByWord.size() : pPageIndex + 1;

	int nbWords = 0;
	int nbSubWords = 0;
	int totalCorrect = 0;
	int totalIncorrect = 0;

	for (int pageIndex = pPageIndex == -1 ? 0 : pPageIndex; pageIndex < maxIndex; pageIndex++) {
		int pageNumber = pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto& line = page.page.lines[lineIndex];
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
				nbWords++;
				page.detectSubWords(pageIndex, lineIndex, wordIndex, &font);
				auto& word = line.words[wordIndex];
				nbSubWords += word.wordResultInfo.subWords.size();
				if (maxSubWords < word.wordResultInfo.subWords.size()) {
					maxSubWords = word.wordResultInfo.subWords.size();
				}
				if (characterSegmentation) {
					for (int subWordIndex = 0; subWordIndex < word.wordResultInfo.subWords.size(); subWordIndex++) {
						bool correct = page.segmentSubword(pageIndex, lineIndex, wordIndex, subWordIndex, &font);
						if (correct) {
							totalCorrect++;
						}
						else {
							totalIncorrect++;
						}
					}
				}
			}
		}
	}

	std::cout << "nbWords=" << nbWords << std::endl;
	std::cout << "nbSubWords=" << nbSubWords << std::endl;
	std::cout << "totalCorrect=" << totalCorrect << std::endl;
	std::cout << "totalIncorrect=" << totalIncorrect << std::endl;


}
namespace {
class CutPreviewView final : public QGraphicsView {
public:
    explicit CutPreviewView(QWidget* parent) : QGraphicsView(parent) {
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QGraphicsView::resizeEvent(event);
        QTimer::singleShot(0, this, [this] {
            if (scene() && !scene()->items().isEmpty()) fitInView(sceneRect(), Qt::KeepAspectRatio);
        });
    }
    void wheelEvent(QWheelEvent* event) override {
        const int delta = event->angleDelta().y();
        if (delta == 0 || scene()->items().isEmpty()) {
            QGraphicsView::wheelEvent(event);
            return;
        }
        const double current = transform().m11();
        const double next = std::clamp(current * std::pow(1.2, delta / 120.0), 0.02, 32.0);
        scale(next / current, next / current);
        event->accept();
    }
};
} // namespace

void MainWindow::inspectSubword(const QPoint& pos) {
    WordMatch hit{};
    int pathIndex = -1;
    for (const auto* item : view->items(pos)) {
        if (item->data(0).canConvert<WordMatch>() && item->data(1).isValid()) {
            hit = item->data(0).value<WordMatch>();
            pathIndex = item->data(1).toInt();
            break;
        }
    }
    if (pathIndex < 0) {
        statusBar()->showMessage(tr("Click the ink of a subword in the standard page view."), 3000);
        return;
    }
    auto& original = getPageResult(hit.pageIndex + 1);
    original.detectSubWords(hit.pageIndex, hit.lineIndex, hit.wordIndex, &font);
    const auto& word = original.page.lines[hit.lineIndex].words[hit.wordIndex];
    const auto& subwords = word.wordResultInfo.subWords;
    const auto subword = std::find_if(subwords.begin(), subwords.end(), [pathIndex](const auto& sub) {
        return std::find(sub.paths.begin(), sub.paths.end(), pathIndex) != sub.paths.end();
    });
    if (subword == subwords.end() || subword->paths.empty()) {
        statusBar()->showMessage(tr("This ink is outside the detected subwords."), 3000);
        return;
    }
    const int subwordIndex = static_cast<int>(subword - subwords.begin());

    // Use a private copy of the word for both views so a draft cannot update the page cache.
    PageAnalysisResult sample;
    sample.page.lines.resize(1);
    sample.page.lines[0].words.append(word);
    const auto& sampleWord = sample.page.lines[0].words[0];
    const auto& result = sampleWord.wordResultInfo.subWords[subwordIndex];

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Segmentation · Page %1 · Line %2 · Word %3 · %4")
        .arg(hit.pageIndex + 1).arg(hit.lineIndex + 1).arg(hit.wordIndex + 1).arg(subword->text));
    dialog.setObjectName("segmentationDialog");
    dialog.resize(1280, 900);
    auto* layout = new QHBoxLayout(&dialog);
    auto* comparisons = new QVBoxLayout;
    auto* editing = new QVBoxLayout;
    layout->addLayout(comparisons, 1);
    layout->addLayout(editing, 1);
    auto* contourPoints = new QCheckBox(tr("Show all contour points"), &dialog);
    comparisons->addWidget(contourPoints);

    auto makeView = [&](const QString& title) {
        auto* heading = new QHBoxLayout;
        heading->addWidget(new QLabel(title, &dialog), 1);
        auto* fit = new QPushButton(tr("Fit"), &dialog);
        fit->setToolTip(tr("Fit %1 to the pane").arg(title));
        heading->addWidget(fit);
        comparisons->addLayout(heading);
        auto* graphics = new CutPreviewView(&dialog);
        graphics->setScene(new QGraphicsScene(graphics));
        graphics->setRenderHint(QPainter::Antialiasing);
        graphics->setMinimumHeight(140);
        graphics->setToolTip(tr("Scroll to zoom. Drag to pan."));
        comparisons->addWidget(graphics, 1);
        connect(fit, &QPushButton::clicked, &dialog, [graphics] {
            if (!graphics->scene()->items().isEmpty())
                graphics->fitInView(graphics->sceneRect(), Qt::KeepAspectRatio);
        });
        return graphics;
    };
    auto* current = makeView(tr("Current cuts"));
    auto* preview = makeView(tr("Draft preview"));
    connect(contourPoints, &QCheckBox::toggled, &dialog, [current, preview](bool visible) {
        for (auto* target : {current, preview})
            for (auto* item : target->scene()->items())
                if (auto* word = dynamic_cast<WordItem*>(item)) word->setShowLandmarks(visible);
    });
    auto* trace = new QPlainTextEdit(&dialog);
    trace->setReadOnly(true);
    trace->setMaximumHeight(135);
    comparisons->addWidget(trace);
    auto* rules = new QComboBox(&dialog);
    editing->addWidget(new QLabel(tr("Template to inspect (matched and applicable alternatives)"), &dialog));
    editing->addWidget(rules);
    auto* editor = new QPlainTextEdit(&dialog);
    editor->setObjectName("letterFormsDraft");
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setPlainText(QString::fromUtf8(QJsonDocument(PageAnalysisResult::letterForms()).toJson()));
    auto* bodyCuts = new BodyCutPanel(editor, &dialog);
    editing->addWidget(bodyCuts, 1);
    auto* status = new QLabel(tr("Edit the JSON and preview it before saving."), &dialog);
    status->setWordWrap(true);
    editing->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* add = buttons->addButton(tr("Add template"), QDialogButtonBox::ActionRole);
    auto* duplicate = buttons->addButton(tr("Duplicate template"), QDialogButtonBox::ActionRole);
    auto* run = buttons->addButton(tr("Preview"), QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton(tr("Save templates…"), QDialogButtonBox::ActionRole);
    save->setEnabled(false);
    editing->addWidget(buttons);

    const auto render = [&](QGraphicsView* target) {
        target->scene()->clear();
        if (result.baseGlyphs.empty()) return;
        auto* item = new WordItem(sampleWord, subwordIndex, true);
        item->setShowLandmarks(contourPoints->isChecked());
        target->scene()->addItem(item);
        target->setSceneRect(item->boundingRect());
        target->fitInView(item->boundingRect(), Qt::KeepAspectRatio);
    };
    const auto describe = [&](bool complete) {
        const auto spans = std::count_if(result.kashidaPaths.begin(), result.kashidaPaths.end(),
                                         [](const QPainterPath& path) { return !path.isEmpty(); });
        return tr("%1 · %2 body regions · %3 kashida spans\n%4")
            .arg(complete ? tr("Complete") : tr("Partial"))
            .arg(result.baseGlyphs.size()).arg(spans)
            .arg((result.matchedTemplates.isEmpty() ? QString{} :
                    tr("Used templates · %1\n").arg(result.matchedTemplates.join(", "))) + result.templateTrace.join('\n'));
    };
    const bool complete = sample.segmentSubword(hit.pageIndex, 0, 0, subwordIndex, &font, true);
    render(current);
    const auto currentTrace = describe(complete);
    const auto currentTemplates = result.matchedTemplates;
    const auto currentAttempts = result.templateTrace;
    trace->setPlainText(currentTrace);
    QStringList inspectIds = currentTemplates;
    const auto letters = PageAnalysisResult::bodyLetters(subword->text);
    for (const auto* phase : {"initial", "medial", "final"}) {
        const auto applicable = QString(phase) == "initial" ? letters.left(1) :
            QString(phase) == "final" ? letters.right(1) : letters.mid(1, std::max(0, letters.size() - 2));
        for (const auto& value : PageAnalysisResult::letterForms().value(phase).toArray()) {
            const auto form = value.toObject();
            if (!form.contains("body")) continue;
            for (const auto letter : applicable)
                if (form.value("current").toString().contains(letter)) { inspectIds.append(form.value("id").toString()); break; }
        }
    }
    inspectIds.removeDuplicates();
    rules->addItems(inspectIds);
    for (const auto& id : currentTemplates) if (resolvedLetterForm(PageAnalysisResult::letterForms(), id).contains("body")) {
        rules->setCurrentText(id); break;
    }
    bodyCuts->selectForm(rules->currentText());
    duplicate->setEnabled(!inspectIds.isEmpty());
    auto* locate = new QPushButton(tr("Locate template in JSON"), &dialog);
    locate->setEnabled(!inspectIds.isEmpty());
    editing->insertWidget(2, locate);
    const auto focusRule = [&](const QString& id) {
        if (rules->findText(id) < 0) rules->addItem(id);
        rules->setCurrentText(id);
        bodyCuts->showFullEditor();
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::Start);
        editor->setTextCursor(cursor);
        editor->find('"' + id + '"');
        editor->setFocus();
    };
    connect(locate, &QPushButton::clicked, &dialog, [&] { focusRule(rules->currentText()); });

    const auto readDraft = [&]() {
        QJsonObject config;
        QString problem;
        if (bodyCuts->readConfig(config, problem)) problem = PageAnalysisResult::prepareLetterForms(config);
        if (!problem.isEmpty()) {
            status->setText(problem);
            return QJsonObject{};
        }
        return config;
    };

    connect(duplicate, &QPushButton::clicked, &dialog, [&] {
        auto config = readDraft();
        if (config.isEmpty()) return;
        const QString sourceId = rules->currentText();
        for (const auto* phase : {"initial", "medial", "final"}) {
            auto entries = config.value(phase).toArray();
            for (int index = 0; index < entries.size(); ++index) {
                auto rule = entries[index].toObject();
                if (rule.value("id").toString() != sourceId) continue;
                QString id;
                for (int suffix = 1; ; ++suffix) {
                    id = sourceId + "-variant-" + QString::number(suffix);
                    bool exists = false;
                    for (const auto* list : {"initial", "medial", "final"}) {
                        for (const auto& entry : config.value(list).toArray())
                            exists |= entry.toObject().value("id").toString() == id;
                    }
                    if (!exists) break;
                }
                rule.insert("id", id);
                const auto copyEligibility = [&](QJsonObject& plan) {
                    auto forms = plan.value("forms").toObject();
                    if (!forms.contains(sourceId)) return false;
                    forms.insert(id, forms.value(sourceId));
                    plan.insert("forms", forms);
                    return true;
                };
                auto expansion = config.value("fixedAnchorExpansion").toObject();
                auto continuation = expansion.value("continuation").toObject();
                const bool expanded = copyEligibility(expansion);
                const bool continued = copyEligibility(continuation);
                if (continued) expansion.insert("continuation", continuation);
                if (expanded || continued) config.insert("fixedAnchorExpansion", expansion);
                entries.insert(index, rule);
                config.insert(phase, entries);
                editor->setPlainText(QString::fromUtf8(QJsonDocument(config).toJson()));
                focusRule(id);
                status->setText(tr("Draft variant added before %1. Edit its constraints, then preview.").arg(sourceId));
                return;
            }
        }
    });

    connect(add, &QPushButton::clicked, &dialog, [&] {
        auto config = readDraft();
        if (config.isEmpty()) return;
        const auto baseText = PageAnalysisResult::bodyLetters(subword->text);
        if (baseText.size() < 2) {
            status->setText(tr("This subword has no join. Select a subword with two or more letters."));
            return;
        }
        QStringList choices;
        for (int index = 0; index < baseText.size(); ++index) {
            const auto position = index == 0 ? QStringLiteral("initial") :
                index + 1 == baseText.size() ? QStringLiteral("final") : QStringLiteral("medial");
            choices.append(QString("%1 · %2 · %3").arg(index + 1).arg(baseText.mid(index, 1), position));
        }
        int defaultLetter = 0;
        for (const auto& value : result.letterDiagnostics) {
            const auto diagnostic = value.toObject();
            if (diagnostic.value("status") != "matched" && diagnostic.value("status") != "not-required") {
                defaultLetter = diagnostic.value("letterIndex").toInt() - 1;
                break;
            }
        }
        bool accepted = false;
        const auto selected = QInputDialog::getItem(&dialog, tr("Add letter form"),
            tr("Letter and position"), choices, defaultLetter, false, &accepted);
        if (!accepted) return;
        const int letter = choices.indexOf(selected);
        const auto position = letter == 0 ? QStringLiteral("initial") :
            letter + 1 == baseText.size() ? QStringLiteral("final") : QStringLiteral("medial");
        const auto suffix = position == "initial" ? "init" : position == "medial" ? "medi" : "fina";
        const auto id = QString("form-%1-%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8), suffix);
        QJsonObject rule{{"id", id}, {"current", baseText.mid(letter, 1)}, {"fixedAnchors", true}};
        if (position == "medial") {
            rule.insert("body", QJsonObject{{"shape", "paired-route"}, {"routes", QJsonObject{
                {"anchor", QJsonObject{{"xExtremum", true}}},
                {"right", QJsonObject{{"top", QJsonArray{"down", "right"}}}},
                {"left", QJsonObject{{"top", QJsonArray{"down", "left"}}}}}}});
            rule.insert("cuts", QJsonObject{{"boundaries", QJsonObject{{"right", "route-right"}, {"left", "route-left"}}},
                {"routes", QJsonObject{{"right", QJsonObject{{"bottomProbe", "normal"}}},
                                       {"left", QJsonObject{{"bottomProbe", "normal"}}}}}});
        } else {
            const bool terminal = position == "final";
            QJsonObject route{{"top", QJsonArray{"down", "left"}}};
            if (terminal) route.insert("start", "left");
            rule.insert("body", QJsonObject{{"shape", "contour-route"}, {"route", route}});
            rule.insert("cuts", QJsonObject{{"route", QJsonObject{{"bottomProbe", "normal"}}},
                {"boundaries", QJsonObject{{"right", terminal ? "contour-route" : "free"},
                                           {"left", terminal ? "free" : "contour-route"}}}});
        }
        auto entries = config.value(position).toArray();
        entries.prepend(rule);
        config.insert(position, entries);
        editor->setPlainText(QString::fromUtf8(QJsonDocument(config).toJson()));
        focusRule(id);
        status->setText(tr("Draft %1 body template added. Define its landmarks, then preview.").arg(position));
    });

    auto* copyReview = new QPushButton(tr("Copy review details"), &dialog);
    comparisons->addWidget(copyReview);
    connect(copyReview, &QPushButton::clicked, &dialog, [&] {
        const auto& active = PageAnalysisResult::letterForms();
        QJsonObject activeRules;
        for (const auto* phase : {"initial", "medial", "final"}) {
            for (const auto& entry : active.value(phase).toArray()) {
                const auto id = entry.toObject().value("id").toString();
                activeRules.insert(id, entry);
            }
        }

        QString report = tr("Medina Mushaf Analyzer · Page %1 · Line %2 · Word %3 · Subword %4\nText: %5\n\n%6")
            .arg(hit.pageIndex + 1).arg(hit.lineIndex + 1).arg(hit.wordIndex + 1)
            .arg(subwordIndex + 1).arg(subword->text, trace->toPlainText());
        QJsonArray usedRules;
        for (const auto& id : currentTemplates)
            if (activeRules.contains(id)) usedRules.append(activeRules.value(id));
        if (!usedRules.isEmpty()) {
            report += tr("\n\nUsed templates\n");
            report += QString::fromUtf8(QJsonDocument(usedRules).toJson());
        }
        QJsonArray otherRules;
        for (const auto& entry : currentAttempts) {
            const auto fields = entry.split(QStringLiteral(" · "));
            for (const auto& field : fields) {
                if (!activeRules.contains(field) || currentTemplates.contains(field)) continue;
                const auto rule = activeRules.value(field);
                if (!otherRules.contains(rule)) otherRules.append(rule);
            }
        }
        if (!otherRules.isEmpty()) {
            report += tr("\n\nTemplates tried\n");
            report += QString::fromUtf8(QJsonDocument(otherRules).toJson());
        }

        QJsonParseError parse;
        const auto draft = QJsonDocument::fromJson(editor->toPlainText().toUtf8(), &parse).object();
        if (parse.error == QJsonParseError::NoError) {
            QJsonObject changes;
            for (const auto* phase : {"initial", "medial", "final"}) {
                QJsonArray changed;
                for (const auto& entry : draft.value(phase).toArray()) {
                    if (activeRules.value(entry.toObject().value("id").toString()) != entry)
                        changed.append(entry);
                }
                if (!changed.isEmpty()) changes.insert(phase, changed);
            }
            for (const auto* key : {"joinDefaults", "kashidaDefaults"}) {
                if (draft.value(key) != active.value(key)) changes.insert(key, draft.value(key));
            }
            if (!changes.isEmpty()) {
                report += tr("\n\nEdited or added rules and defaults\n");
                report += QString::fromUtf8(QJsonDocument(changes).toJson());
            }
        }
        if (!bodyCuts->reviewDetails().isEmpty()) {
            report += "\n\nBody / cut inspection · " + rules->currentText() + "\n" + bodyCuts->reviewDetails();
        }
        QApplication::clipboard()->setText(report);
        statusBar()->showMessage(tr("Review details copied."), 3000);
        copyReview->setText(tr("Review details copied"));
        QTimer::singleShot(1500, copyReview, [copyReview] { copyReview->setText(tr("Copy review details")); });
    });

    QJsonObject previewed;
    connect(bodyCuts, &BodyCutPanel::draftChanged, &dialog, [&] {
        bodyCuts->invalidate();
        previewed = {};
        save->setEnabled(false);
        preview->scene()->clear();
        trace->setPlainText(currentTrace);
        status->setText(tr("Preview the edited JSON to enable saving."));
    });
    connect(run, &QPushButton::clicked, &dialog, [&] {
        previewed = readDraft();
        save->setEnabled(!previewed.isEmpty());
        if (previewed.isEmpty()) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const auto inspection = inspectLetterForm(sample, hit.pageIndex, 0, 0, subwordIndex, &font,
                                                   previewed, rules->currentText());
        QApplication::restoreOverrideCursor();
        const bool ok = inspection.complete;
        bodyCuts->setInspection(inspection);
        render(preview);
        trace->setPlainText(tr("Current\n%1\n\nDraft\n%2").arg(currentTrace, describe(ok)));
        status->setText(ok ? tr("Preview ready.") : tr("Preview is partial. Check the rule trace before saving."));
    });
    QString selectedForm = rules->currentText();
    connect(rules, &QComboBox::currentTextChanged, &dialog, [&](const QString& id) {
        if (!bodyCuts->selectForm(id)) {
            const QSignalBlocker blocked(rules);
            rules->setCurrentText(selectedForm);
            status->setText(tr("Fix the invalid JSON before switching templates."));
            return;
        }
        selectedForm = id;
        run->click();
    });
    connect(save, &QPushButton::clicked, &dialog, [&] {
        const auto initialPath = letterFormsSavePath.isEmpty()
            ? qEnvironmentVariable("MEDINA_LETTER_FORMS", QDir::home().filePath("letter-forms.json"))
            : letterFormsSavePath;
        const auto path = QFileDialog::getSaveFileName(&dialog, tr("Save letter templates"), initialPath,
                                                       tr("JSON files (*.json)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto bytes = QJsonDocument(previewed).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            status->setText(tr("Could not save templates. %1").arg(file.errorString()));
            return;
        }
        letterFormsSavePath = path;
        QSettings().setValue("letterFormsPath", path);
        PageAnalysisResult::activateLetterForms(previewed);
        for (auto& cached : pagesCache)
            for (auto& line : cached.page.lines)
                for (auto& cachedWord : line.words)
                    for (auto& sub : cachedWord.wordResultInfo.subWords)
                        sub.baseGlyphs.clear();
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QTimer::singleShot(0, &dialog, [current, run] {
        run->click();
        if (!current->scene()->items().isEmpty())
            current->fitInView(current->sceneRect(), Qt::KeepAspectRatio);
    });
    if (dialog.exec() == QDialog::Accepted) {
        loadPage(hit.pageIndex + 1);
        statusBar()->showMessage(tr("Saved and activated templates. %1").arg(letterFormsSavePath), 6000);
    }
}

void MainWindow::showResuts(QString dockName, const std::multimap<qreal, WordMatch, std::greater <qreal> >& results) {


	int rowIndex = 0;
	QTableWidget* tableWidget = new	QTableWidget(results.size(), 4, this);
	tableWidget->setItemDelegate(new PathDelegate);
	tableWidget->horizontalHeader()->hide();
	tableWidget->setColumnCount(4);
	tableWidget->horizontalHeader()->setResizeContentsPrecision(-1);
	tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
	tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(tableWidget, &QTableWidget::customContextMenuRequested, this, [this, tableWidget](const QPoint& pos) {
		//auto gloPos = mapToGlobal(pos);
		QTableWidgetItem* item = tableWidget->itemAt(pos);
		if (item) {
			auto vardata = item->data(Qt::UserRole);
			if (vardata.canConvert<WordMatch>()) {
				auto data = qvariant_cast<WordMatch>(vardata);
				int pageNumber = data.pageIndex + 1;

				QMenu menu;
				menu.addAction("Save Picture");
				QAction* a = menu.exec(QCursor::pos());
				if (a != NULL) {
					if (a->text() == "Save Picture") {

						QFileDialog dialog(this);
						dialog.setFileMode(QFileDialog::AnyFile);

						QString fileName = QFileDialog::getSaveFileName(this, tr("Save File"),
							"test.jpg",
							tr("Images (*.jpg)"));

						if (fileName.isEmpty()) return;

						auto& page = getPageResult(pageNumber);
						auto word = page.page.lines[data.lineIndex].words[data.wordIndex];
						QPainterPath wordPath;
						for (auto shape : word.paths) {
							auto itemPath = shape.path;
							auto pos = shape.pos;
							itemPath = itemPath * shape.transform;
							itemPath.translate(pos.x(), pos.y());
							wordPath.addPath(itemPath);
						}

						QTransform transform{ constants::SCALE_GLYPH,0,0,constants::SCALE_GLYPH ,0,0 };

						wordPath = wordPath * transform;
						auto box1 = wordPath.boundingRect();
						wordPath.translate(-box1.left() + 50, -box1.top() + 50);

						auto bbox = wordPath.boundingRect();

						QImage image(bbox.size().toSize() + QSize(100, 100), QImage::Format_ARGB32_Premultiplied);
						image.fill(qRgba(255, 255, 255, 255));
						QPainter painter(&image);

						painter.setRenderHint(QPainter::Antialiasing, true);

						painter.setPen(Qt::NoPen);

						painter.setBrush(QBrush(qRgba(174, 234, 174, 255)));
						painter.drawPath(wordPath);
						painter.end();



						image.save(fileName, "jpeg");


					}
				}
			}
		}
	});

	connect(tableWidget, &QTableWidget::cellClicked, [this, tableWidget](int row, int column) {


		auto item = tableWidget->item(row, column);

		auto vardata = item->data(Qt::UserRole);

		if (vardata.canConvert<WordMatch>()) {
			auto data = qvariant_cast<WordMatch>(vardata);
			int pageNumber = data.pageIndex + 1;
			auto& page = getPageResult(pageNumber);
			if (column == 0) {
				auto word = page.page.lines[data.lineIndex].words[data.wordIndex];
				QPainterPath wordPath;
				for (auto shape : word.paths) {
					auto itemPath = shape.path;
					auto pos = shape.pos;
					itemPath = itemPath * shape.transform;
					itemPath.translate(pos.x(), pos.y());
					wordPath.addPath(itemPath);
				}

				auto bbox = wordPath.boundingRect();
				wordPath.translate(-bbox.x(), -bbox.y());

				QTransform transform{ constants::SCALE_GLYPH,0,0,constants::SCALE_GLYPH,0,0 };

				delete shapeScene;

				shapeScene = new QGraphicsScene(this);

				this->shapeScene->addItem(new ShapeItem(wordPath * transform));

				shapeView->setScene(shapeScene);
			}
			else if (column == 1) {
				integerSpinBox->setValue(pageNumber);
				auto info = page.detectSubWords(data.pageIndex, data.lineIndex, data.wordIndex, &this->font, true);
				this->displaySubWords(data.pageIndex, data.lineIndex, data.wordIndex, WordResultFlags::NONE);
			}

		}


	});

	for (auto& result : results) {

		auto wordMatch = result.second;

		int pageNumber = wordMatch.pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		auto word = page.page.lines[wordMatch.lineIndex].words[wordMatch.wordIndex];
		QPainterPath wordPath;
		for (auto shape : word.paths) {
			auto itemPath = shape.path;
			auto pos = shape.pos;
			itemPath = itemPath * shape.transform;
			itemPath.translate(pos.x(), pos.y());
			wordPath.addPath(itemPath);
		}

		//tableWidget->insertRow(rowIndex);

		auto pathItem = new QTableWidgetItem();
		pathItem->setData(Qt::DisplayRole, QVariant::fromValue(PathImage(wordPath)));
		pathItem->setData(Qt::UserRole, QVariant::fromValue(wordMatch));
		tableWidget->setItem(rowIndex, 0, pathItem);

		pathItem = new QTableWidgetItem();
		pathItem->setData(Qt::DisplayRole, QString("%1-%2-%3").arg(pageNumber).arg(wordMatch.lineIndex + 1).arg(wordMatch.wordIndex + 1));
		pathItem->setData(Qt::UserRole, QVariant::fromValue(wordMatch));
		tableWidget->setItem(rowIndex, 1, pathItem);

		pathItem = new QTableWidgetItem();
		pathItem->setData(Qt::DisplayRole, QVariant::fromValue(PathImage(wordMatch.path)));
		pathItem->setData(Qt::UserRole, QVariant::fromValue(wordMatch));
		tableWidget->setItem(rowIndex, 2, pathItem);

		pathItem = new QTableWidgetItem();
		pathItem->setData(Qt::DisplayRole, wordMatch.path.boundingRect().width());
		pathItem->setData(Qt::UserRole, QVariant::fromValue(wordMatch));
		tableWidget->setItem(rowIndex, 3, pathItem);

		rowIndex++;
	}


	tableWidget->resizeColumnsToContents();
	tableWidget->resizeRowsToContents();

	auto newDock = new QDockWidget(dockName, this);
	newDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

	newDock->setAttribute(Qt::WA_DeleteOnClose);

	addDockWidget(Qt::LeftDockWidgetArea, newDock);
	viewMenu->addAction(newDock->toggleViewAction());

	newDock->setWidget(tableWidget);

	tabifyDockWidget(pathListDock, newDock);
	newDock->show();
	newDock->raise();
}
void MainWindow::displaySearch(QString textToSearch, const std::vector<WordMatch>& matches, bool sortByWidth) {

	if (matches.size() == 0) return;



	std::multimap<qreal, WordMatch, std::greater <qreal> > results;
	int index = 0;

	for (auto wordMatch : matches) {

		/*
		if (match.pageIndex == 29 && match.lineIndex == 10 && match.wordIndex == 8) {
			std::cout << "stop at " << match.pageIndex + 1 << "-" << match.lineIndex + 1 << "-" << match.wordIndex + 1 << std::endl;
		}*/

		auto pageNumber = wordMatch.pageIndex + 1;


		auto& page = getPageResult(pageNumber);
		auto& line = page.page.lines[wordMatch.lineIndex];
		auto& word = line.words[wordMatch.wordIndex];

		page.detectSubWords(pageNumber - 1, wordMatch.lineIndex, wordMatch.wordIndex, &font);

		auto& wordInfo = word.wordResultInfo;


		int start = -1;
		int end = -1;
		for (int captType = 0; captType < 4; captType++) {
			auto captName = (QString("capt%1").arg(captType));
			start = wordMatch.match.capturedStart(captName);
			if (start != -1) {
				end = wordMatch.match.capturedEnd(captName);
				break;
			}
		}

		if (start == -1) {
			throw new std::runtime_error("Error");
		}

		std::set<int> subWords;

		for (int i = start; i < end; i++) {
			auto& charInfo = wordInfo.charInfos[i];
			subWords.insert(charInfo.subWord);
		}

		bool correct = true;

		for (auto subWordIndex : subWords) {
			correct = correct && page.segmentSubword(wordMatch.pageIndex, wordMatch.lineIndex, wordMatch.wordIndex, subWordIndex, &font);
		}

		wordMatch.path.clear();
		if (!correct) {
			for (auto subWordIndex : subWords) {
				auto& subword = wordInfo.subWords[subWordIndex];

				const auto shape = word.bodyShape(subword);
				auto itemPath = shape.path;
				auto pos = shape.pos;
				itemPath = itemPath * shape.transform;
				itemPath.translate(pos.x(), pos.y());
				wordMatch.path.addPath(itemPath);
			}
			results.insert({ sortByWidth ? 0 : index,wordMatch });
		}
		else {

			int currentCharIndex = 0;
			for (auto& subWord : wordInfo.subWords) {

				//if (start >= currentCharIndex + subword.text.size()) continue;

				//if (end < currentCharIndex) continue;

				int baseGlyphIndex = 0;
				auto endSubWord = currentCharIndex + subWord.text.size();
				auto& shape = word.paths[subWord.paths[0]];

				for (int charIndex = currentCharIndex; charIndex < endSubWord; charIndex++) {

					auto isMark = word.text[charIndex].isMark();

					if (charIndex >= start && charIndex < end && !isMark) {
						if (baseGlyphIndex < subWord.baseGlyphs.size()) {
							auto& baseGlyphInfo = subWord.baseGlyphs[baseGlyphIndex];
							auto itemPath = baseGlyphInfo.path;
							auto pos = shape.pos;
							itemPath = itemPath * shape.transform;
							itemPath.translate(pos.x(), pos.y());
							wordMatch.path.addPath(itemPath);
						}
						else {
							std::cout << "Problem displaySearch at pageNumber=" << pageNumber
								<< ",lineNumber=" << wordMatch.lineIndex + 1
								<< ",wordNumber=" << wordMatch.wordIndex + 1
								<< ",wordText=" << word.text.toStdString()
								<< std::endl;
						}

					}

					if (!isMark) {
						baseGlyphIndex++;
					}

				}

				currentCharIndex = endSubWord;

			}

			auto bbox = wordMatch.path.boundingRect();
			results.insert({ sortByWidth ? bbox.width() : index,wordMatch });
		}



		index--;

	}

	showResuts(textToSearch, results);
}

void MainWindow::findExpandables() {


	auto text = { "ا","ب","ج","د","ر","س","ص","ط","ع","ف","ق","ك","ل", "م","ن","ه","و","ي","ء","ۥ","ۦ" };
	//auto text = { "ۥ","ۦ" };
	//auto text = { "ء" };
	//auto text = { "ا" };
	//auto text = {"ر" };
	//auto text = { "ب" };
	//auto text = { "و" };
	//auto text = { "ر" };
	//auto text = { "د" };
	for (auto& textToSearch : text) {
		std::cout << textToSearch << std::endl;
		auto matches = quransearch.searchText(textToSearch,
			false,
			true,
			true,
			true,
			false,
			false,
			false
		);

		displaySearch(textToSearch, matches, sordByWidth->isChecked());

	}




}

void MainWindow::testAnalyze() {

	std::vector<QThread*> threads;

	int totalpageNb = 604;

	int nbthreads = 16;
	int pageperthread = totalpageNb / nbthreads;
	int remainingPages = totalpageNb;

	while (remainingPages != 0) {

		int begin = totalpageNb - remainingPages;
		if (pageperthread == 0 || remainingPages < pageperthread) {
			pageperthread = remainingPages;
		}

		remainingPages -= pageperthread;

		QThread* thread = QThread::create([this, begin, pageperthread] {
			for (int i = begin; i < begin + pageperthread; i++) {
				PageAnalysisResult pageResult;
				pageResult.loadPage(i, &font, true);
			}
		});

		threads.push_back(thread);

		thread->start();
	}


	for (auto t : threads) {
		t->wait();
		delete t;
	}

	/*

	for (int i = 1; i < 605; i++) {
		delete scene;
		scene = new QGraphicsScene(this);
		PageAnalysisResult pageResult;
		pageResult.loadPage2(i, &font, scene);
	}*/
}
static int drawLetterSegmentation(QGraphicsScene& scene, const QJsonObject& segmentation) {
    const QColor colors[]{QColor("#24618e"), QColor("#39734c"), QColor("#705194")};
    int incomplete = 0;
    const auto coordinate = [](const QJsonValue& value) {
        const auto xy = value.toArray();
        return QPointF(xy.at(0).toDouble(), xy.at(1).toDouble());
    };
    for (const auto& item : segmentation.value("subwords").toArray()) {
        const auto subword = item.toObject();
        const bool complete = subword.value("complete").toBool();
        int index = 0;
        for (const auto& body : subword.value("bodies").toArray()) {
            auto* path = scene.addPath(letterSegmentationPath(body.toObject()), Qt::NoPen, colors[index++ % 3]);
            path->setZValue(1);
        }
        for (const auto& item : subword.value("joins").toArray()) {
            const auto join = item.toObject();
            if (join.contains("kashida")) {
                auto* path = scene.addPath(letterSegmentationPath(join.value("kashida").toObject()), Qt::NoPen, QColor("#df8b27"));
                path->setZValue(2);
            }
            const auto cuts = join.contains("endpoints") ? join.value("endpoints").toArray()
                                                        : QJsonArray{join.value("anchor")};
            for (const auto& value : cuts) {
                const auto cut = value.toArray();
                if (cut.size() != 2) continue;
                QPen pen(QColor("#ffffff"), 0);
                auto* line = scene.addLine(QLineF(coordinate(cut.at(0)), coordinate(cut.at(1))), pen);
                line->setZValue(3);
            }
        }
        if (!complete) {
            ++incomplete;
            const auto bounds = subword.value("bounds").toArray();
            if (bounds.size() != 4) continue;
            const QRectF box(bounds.at(0).toDouble(), bounds.at(1).toDouble(),
                             bounds.at(2).toDouble(), bounds.at(3).toDouble());
            QPen pen(QColor("#bd3d37"), 0, Qt::DashLine);
            auto* warning = scene.addRect(box.adjusted(-0.4, -0.4, 0.4, 0.4), pen);
            warning->setZValue(4);
            warning->setToolTip(QObject::tr("Partial segmentation"));
        }
    }
    return incomplete;
}

void MainWindow::loadPage(int pageNumber, bool forceSegmentation) {

    const auto oldCenter = view->scene() ? view->mapToScene(view->viewport()->rect().center()) : QPointF();
    const bool samePage = scene->property("pageNumber").toInt() == pageNumber;
	view->setScene(nullptr);

	//testAnalyze();

	scene->clear();
	delete scene;

	scene = new QGraphicsScene(this);
    scene->setProperty("pageNumber", pageNumber);

	PageAnalysisResult result;

	bool takeFromCache = true;

	bool cached = pagesCache.contains(pageNumber);

	auto added = false;

	if (debugBBButton->isChecked()) {
		takeFromCache = false;
		result.loadPage(pageNumber, &font, true, scene);
		added = true;
	}
	else if (!cached) {
		QString cacheFileName = QString("./pages/page%1.dat").arg(pageNumber);

		QFile cacheFile(cacheFileName);
		if (cacheFile.exists()) {
			pagesCache[pageNumber].page = loadCache(cacheFileName);
			pagesCache[pageNumber].resolveWordBoundaries(pageNumber, &font);
		}
		else {
			auto& tt = pagesCache[pageNumber];
			tt.loadPage(pageNumber, &font, false);
		}
	}



	PageAnalysisResult& pageResult = takeFromCache ? pagesCache[pageNumber] : result;

    const bool showPathList = !pathListDock->isHidden();
    const bool showBoxes = showWordBBButton->isChecked();
    auto* layout = showPathList ? new QVBoxLayout : nullptr;

	int lineIndex = -1;
	for (auto& line : pageResult.page.lines) {
		lineIndex++;
		QPainterPath linePath;
		int wordIndex = -1;
		for (auto& word : line.words) {
			wordIndex++;
			if (!added) {
				for (int pathIndex = 0; pathIndex < word.paths.size(); ++pathIndex) {
					const auto& path = word.paths[pathIndex];
					auto item = new QGraphicsPathItem(path.path);
					item->setFlag(QGraphicsItem::ItemIsSelectable, true);
					item->setFlag(QGraphicsItem::ItemIsMovable, true);
					item->setTransform(path.transform);
					item->setPos(path.pos);
					item->setPen(Qt::NoPen);
					item->setBrush(Qt::black);
					item->setData(0, QVariant::fromValue(WordMatch{ pageNumber - 1,lineIndex,wordIndex }));
					item->setData(1, pathIndex);
					scene->addItem(item);
				}

			}
            if (showBoxes || showPathList) {
                QPainterPath wordPath;
                for (const auto& shape : word.paths) {
                    auto path = shape.transform.map(shape.path);
                    path.translate(shape.pos);
                    wordPath.addPath(path);
                }
                if (showBoxes && !wordPath.isEmpty()) {
                    const auto bounds = wordPath.boundingRect();
                    scene->addRect(bounds, QPen(Qt::black, 0));
                    scene->addLine(bounds.left(), line.baseline, bounds.right(), line.baseline, QPen(Qt::blue, 0));
                }
                if (showPathList) linePath.addPath(wordPath);
            }
        }
        if (layout) layout->addWidget(new RenderArea(linePath));
    }

    QString message = tr("Page %1 loaded").arg(pageNumber);
    if (takeFromCache && (forceSegmentation || showLetterSegmentationButton->isChecked())) {
        bool cacheHit = false;
        const auto segmentation = loadLetterSegmentation(pageResult, pageNumber, font, &cacheHit, forceSegmentation);
        if (showLetterSegmentationButton->isChecked()) {
            const int incomplete = drawLetterSegmentation(*scene, segmentation);
            message = tr("Page %1 · %2 incomplete · %3")
                .arg(pageNumber).arg(incomplete)
                .arg(cacheHit ? tr("cached segmentation") : tr("segmentation computed"));
        } else {
            message = tr("Page %1 · segmentation computed").arg(pageNumber);
        }
    }
	view->setScene(scene);
    if (samePage) view->centerOn(oldCenter);

    if (layout) {
        auto* widget = new QWidget(this);
        widget->setLayout(layout);
        pathListWidget->setWidget(widget);
    } else {
        delete pathListWidget->takeWidget();
    }

	statusBar()->showMessage(message, 5000);

}
