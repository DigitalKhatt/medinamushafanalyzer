
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

	button = new QPushButton(tr("&Search Stretching By SubWords"));
	connect(button, &QPushButton::clicked, [&](bool checked) {
		searchStretchingBySubWord();
	});

	jutifyToolbar->addWidget(button);

	auto button2 = new QPushButton(tr("&Find Expandables"));
	connect(button2, &QPushButton::clicked, [&](bool checked) {
		findExpandables();
	});

	jutifyToolbar->addWidget(button2);

	auto button3 = new QPushButton(tr("&Detect SubWords"));
	connect(button3, &QPushButton::clicked, [&](bool checked) {
		detectSubWords();
	});

	jutifyToolbar->addWidget(button3);


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
bool MainWindow::saveAllPages2() {


	std::vector<QThread*> threads;

	int totalpageNb = 604;

	int nbthreads = 32;
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
				int pageNumber = i + 1;
				savePageFile(pageNumber, false);
			}
		});

		threads.push_back(thread);

		thread->start();
	}


	for (auto t : threads) {
		t->wait();
		delete t;
	}

	pagesCache.clear();

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
		.arg(point.x())
		.arg(point.y());
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

void MainWindow::searchStretchingBySubWord() {

	int nbWords = 0;
	int nbSubWords = 0;
	for (int pageIndex = 2; pageIndex < PageAnalysisResult::QuranTextByWord.size(); pageIndex++) {
		int pageNumber = pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto& line = page.page.lines[lineIndex];
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
				nbWords++;
				auto  info = page.detectSubWords(pageIndex, lineIndex, wordIndex, &font);
				nbSubWords += info.subWords.size();
			}
		}
	}

	std::cout << "nbWords=" << nbWords << std::endl;
	std::cout << "nbSubWords=" << nbSubWords << std::endl;

	std::multimap<qreal, WordMatch, std::greater <qreal> > results;


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
						auto extr = ShapeItem::getExtrema(itemPath, subPaths);
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

					results.insert({ sordByWidth->isChecked() ? maxDist : maxSubWordPath.boundingRect().width(), WordMatch{ pageIndex ,lineIndex,wordIndex,{},maxSubWordPath } });
				}
			}
		}
	}

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
					ShapeItem::searchStretchings(itemPath, stretchings, line);

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

	int rowIndex = 0;
	for (int pageIndex = 2; pageIndex < PageAnalysisResult::QuranTextByWord.size(); pageIndex++) {
		if (ppageIndex != -1 && !(ppageIndex == pageIndex))  continue;
		int pageNumber = pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
			auto line = page.page.lines[lineIndex];
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {

				if (ppageIndex != -1) {
					if (!(ppageIndex == pageIndex && plineIndex == lineIndex && pwordIndex == wordIndex)) continue;
				}

				page.detectSubWords(pageIndex, lineIndex, wordIndex, &font);
				auto& word = line.words[wordIndex];

				if (ppageIndex == -1 && (word.wordResultInfo.state & state) != state) continue;

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
				pathItem->setData(Qt::DisplayRole, QVariant::fromValue(StarRating(wordPath)));
				pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
				tableWidget->setItem(rowIndex, 0, pathItem);

				pathItem = new QTableWidgetItem();
				pathItem->setData(Qt::DisplayRole, QString("%1-%2-%3").arg(pageNumber).arg(match.lineIndex + 1).arg(match.wordIndex + 1));
				pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
				tableWidget->setItem(rowIndex, 1, pathItem);
				int colIndex = 2;
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
					pathItem->setData(Qt::DisplayRole, QVariant::fromValue(StarRating(subWordPath)));
					pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
					tableWidget->setItem(rowIndex, colIndex, pathItem);

					colIndex++;

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

void MainWindow::detectSubWords() {

	if (!maxSubWords) {
		for (int pageIndex = 2; pageIndex < PageAnalysisResult::QuranTextByWord.size(); pageIndex++) {
			int pageNumber = pageIndex + 1;
			auto& page = getPageResult(pageNumber);
			for (int lineIndex = 0; lineIndex < page.page.lines.length(); lineIndex++) {
				auto& line = page.page.lines[lineIndex];
				for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
					page.detectSubWords(pageIndex, lineIndex, wordIndex, &font);
					auto& word = line.words[wordIndex];
					if (maxSubWords < word.wordResultInfo.subWords.size()) {
						maxSubWords = word.wordResultInfo.subWords.size();
					}
				}
			}
		}
	}

	for (int i = 0; i < 6; i++) {
		WordResultFlags state = (WordResultFlags)(1 << i);
		displaySubWords(-1, -1, -1, state);
	}
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

					painter.setBrush(Qt::gray);
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

			this->shapeScene->clear();

			this->shapeScene->setSceneRect(QRectF());

			//this->shapeScene->setSceneRect(QRectF());

			this->shapeScene->addItem(new ShapeItem(wordPath * transform));
		}
		else if (column == 1) {
			integerSpinBox->setValue(pageNumber);
			auto info = page.detectSubWords(data.pageIndex, data.lineIndex, data.wordIndex, &this->font, true);
			this->displaySubWords(data.pageIndex, data.lineIndex, data.wordIndex, WordResultFlags::NONE);
		}

	}


	});

	for (auto& result : results) {

		auto match = result.second;

		int pageNumber = match.pageIndex + 1;
		auto& page = getPageResult(pageNumber);
		auto word = page.page.lines[match.lineIndex].words[match.wordIndex];
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
		pathItem->setData(Qt::DisplayRole, QVariant::fromValue(StarRating(wordPath)));
		pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
		tableWidget->setItem(rowIndex, 0, pathItem);

		pathItem = new QTableWidgetItem();
		pathItem->setData(Qt::DisplayRole, QString("%1-%2-%3").arg(pageNumber).arg(match.lineIndex + 1).arg(match.wordIndex + 1));
		pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
		tableWidget->setItem(rowIndex, 1, pathItem);

		pathItem = new QTableWidgetItem();
		pathItem->setData(Qt::DisplayRole, QVariant::fromValue(StarRating(match.path)));
		pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
		tableWidget->setItem(rowIndex, 2, pathItem);

		pathItem = new QTableWidgetItem();
		pathItem->setData(Qt::DisplayRole, match.path.boundingRect().width());
		pathItem->setData(Qt::UserRole, QVariant::fromValue(match));
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

	for (auto match : matches) {

		/*
		if (match.pageIndex == 29 && match.lineIndex == 10 && match.wordIndex == 8) {
			std::cout << "stop at " << match.pageIndex + 1 << "-" << match.lineIndex + 1 << "-" << match.wordIndex + 1 << std::endl;
		}*/

		auto pageNumber = match.pageIndex + 1;


		auto& page = getPageResult(pageNumber);
		auto& line = page.page.lines[match.lineIndex];
		auto& word = line.words[match.wordIndex];

		auto wordInfo = page.detectSubWords(pageNumber - 1, match.lineIndex, match.wordIndex, &font);


		int start = -1;
		int end = -1;
		for (int captType = 0; captType < 4; captType++) {
			auto captName = (QString("capt%1").arg(captType));
			start = match.match.capturedStart(captName);
			if (start != -1) {
				end = match.match.capturedEnd(captName);
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



		QPainterPath wordPath;
		for (auto subWordIndex : subWords) {
			auto subword = wordInfo.subWords[subWordIndex];

			auto& shape = word.paths[subword.paths[0]];
			auto itemPath = shape.path;
			auto pos = shape.pos;
			itemPath = itemPath * shape.transform;
			itemPath.translate(pos.x(), pos.y());
			wordPath.addPath(itemPath);
		}

		auto bbox = wordPath.boundingRect();
		match.path = wordPath;
		results.insert({ sortByWidth ? bbox.width() : index,match });

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
	else {
		QString cacheFileName = QString("./pages/page%1.dat").arg(pageNumber);

		QFile cacheFile(cacheFileName);
		if (cacheFile.exists()) {
			if (!cached) {
				pagesCache[pageNumber].page = loadCache(cacheFileName);
			}
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