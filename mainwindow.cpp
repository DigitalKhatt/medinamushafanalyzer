
#include <QtWidgets>

#include "mainwindow.h"
#include "CustomOutputDev.h"
#include "PDFDoc.h"
#include "GlobalParams.h"
#include "renderarea.h"
#include "graphicsview.h"
#include <iostream>

MainWindow::MainWindow()
{



	loadFile(fontFileName);

	createActions();
	createStatusBar();
	createDockWindows();

	readSettings();

	setUnifiedTitleAndToolBarOnMac(true);

	integerSpinBox->setValue(3);


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



	pathListDock->raise();


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

	connect(integerSpinBox, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged),
		[=](int i) {
		loadPage(i);
	});

	auto jutifyToolbar = addToolBar(tr("Quran"));
	jutifyToolbar->addWidget(integerSpinBox);

	showWordBBButton = new QPushButton(tr("&ShowWordBB"));
	showWordBBButton->setCheckable(true);
	showWordBBButton->setChecked(true);

	connect(showWordBBButton, &QPushButton::toggled, [&](bool checked) {
		loadPage(integerSpinBox->value());
	});



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
	connect(segmentSubwordsButton, &QPushButton::clicked, [&](bool checked) {
		detectSubWords(true);

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
					bool CharacterSegCorrect = page.segmentSubword(pageIndex, lineIndex, wordIndex, subWordIndex, &font);
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


					for (int i = 1; i < subWord.paths.size(); i++) {
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

	auto app = QApplication::instance();

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
						int baseGlyphIndex = 0;
						for (auto& join : subWord->joins) {
							QPainterPath chartacterPath;
							auto& shape = word.paths[0];
							auto itemPath = subWord->baseGlyphs[baseGlyphIndex].path;
							auto pos = shape.pos;
							itemPath = itemPath * shape.transform;
							itemPath.translate(pos.x(), pos.y());
							chartacterPath.addPath(itemPath);
							results.insert({ join.length, WordMatch{ pageIndex ,lineIndex,wordIndex,0,{},chartacterPath } });

							baseGlyphIndex++;
						}
					}
					else {
						QPainterPath subWordPath;
						auto& shape = word.paths[0];
						auto itemPath = shape.path;
						auto pos = shape.pos;
						itemPath = itemPath * shape.transform;
						itemPath.translate(pos.x(), pos.y());
						subWordPath.addPath(itemPath);
						results.insert({ 0, WordMatch{ pageIndex ,lineIndex,wordIndex,0,{},subWordPath } });
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
				qreal maxDist = 0.0;
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
							auto& shape = word.paths[shapeIndex];
							auto itemPath = shape.path;
							auto pos = shape.pos;
							itemPath = itemPath * shape.transform;
							itemPath.translate(pos.x(), pos.y());
							subWordPath.addPath(itemPath);
						}
					}
					else if (subWord.paths.size() > 0) {
						auto& shape = word.paths[subWord.paths[0]];
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
void MainWindow::handleContextMenu(const QPoint& pos)
{

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

				auto& shape = word.paths[subword.paths[0]];
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

		QVector<int>* set = new QVector<int>();

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
void MainWindow::loadPage(int pageNumber) {

	view->setScene(nullptr);

	//testAnalyze();

	scene->clear();
	delete scene;

	scene = new QGraphicsScene(this);

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
		}
		else {
			auto& tt = pagesCache[pageNumber];
			tt.loadPage(pageNumber, &font, debugBBButton->isChecked(), scene);
			added = true;
		}
	}



	PageAnalysisResult& pageResult = takeFromCache ? pagesCache[pageNumber] : result;

	QVBoxLayout* layout = new QVBoxLayout();

	int lineIndex = -1;
	for (auto& line : pageResult.page.lines) {
		lineIndex++;
		QPainterPath linePath;
		int wordIndex = -1;
		for (auto& word : line.words) {
			wordIndex++;
			if (!added) {
				for (auto path : word.paths) {
					auto item = new QGraphicsPathItem(path.path);
					item->setFlag(QGraphicsItem::ItemIsSelectable, true);
					item->setFlag(QGraphicsItem::ItemIsMovable, true);
					item->setTransform(path.transform);
					item->setPos(path.pos);
					item->setPen(Qt::NoPen);
					item->setBrush(Qt::black);
					item->setData(0, QVariant::fromValue(WordMatch{ pageNumber - 1,lineIndex,wordIndex }));
					scene->addItem(item);
				}

			}
			if (showWordBBButton->isChecked()) {
				QPainterPath wordPath;
				for (auto shape : word.paths) {
					auto pos = shape.pos;
					auto itemPath = shape.path;
					itemPath = itemPath * shape.transform;
					itemPath.translate(pos.x(), pos.y());
					wordPath.addPath(itemPath);
				}
				auto bbox = wordPath.boundingRect();
				auto item = new QGraphicsRectItem(bbox);
				//item->setFlag(QGraphicsItem::ItemIsSelectable, true);
				//item->setFlag(QGraphicsItem::ItemIsMovable, true);
				//item->setTransform(path.transform);
				//item->setPos(path.pos);
				QPen pen;
				pen.setWidth(0);
				item->setPen(pen);
				//item->setBrush(Qt::black);
				scene->addItem(item);

				auto itemLline = new QGraphicsLineItem(bbox.left(), line.baseline, bbox.right(), line.baseline);
				itemLline->setPen(QPen(Qt::blue, 0));
				scene->addItem(itemLline);
			}

			QPainterPath wordPath;
			for (auto shape : word.paths) {
				auto itemPath = shape.path;
				auto pos = shape.pos;
				itemPath = itemPath * shape.transform;
				itemPath.translate(pos.x(), pos.y());
				wordPath.addPath(itemPath);
			}
			if (showWordBBButton->isChecked()) {
				auto bbox = wordPath.boundingRect();
				auto penWidth = 0.1;
				wordPath.addRect(bbox.x(), bbox.y() - penWidth, bbox.width(), penWidth);
				wordPath.addRect(bbox.x(), bbox.y() + bbox.height(), bbox.width(), penWidth);
				wordPath.addRect(bbox.x(), bbox.y(), penWidth, bbox.height());
				wordPath.addRect(bbox.x() + bbox.width(), bbox.y(), penWidth, bbox.height());
			}

			linePath.addPath(wordPath);

		}
		layout->addWidget(new RenderArea(linePath));
	}


	view->setScene(scene);

	auto widget = new QWidget(this);
	widget->setLayout(layout);

	pathListWidget->setWidget(widget);

	statusBar()->showMessage(tr("File loaded"), 2000);

}