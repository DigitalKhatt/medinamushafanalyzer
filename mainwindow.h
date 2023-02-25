#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>


QT_BEGIN_NAMESPACE
class QGraphicsView;
class QGraphicsScene;
class QAction;
class QMenu;
class QPlainTextEdit;
class QSessionManager;
class QScrollArea;
class QSpinBox;
class QLabel;
QT_END_NAMESPACE
#include "afont.h"

#include "quransearch.h"


class MainWindow : public QMainWindow
{
	
	Q_OBJECT

public:
	MainWindow();

	void setMousePosition(const QPointF& point);
	AFont font;
	void displaySubWords(int pageIndex, int lineIndex, int wordIndex, WordResultFlags state);
	void displaySearch(QString textToSearch, const std::vector<WordMatch>& matches, bool sortByWidth = true);
	void showResuts(QString dockName, const std::multimap<qreal, WordMatch, std::greater <qreal> >& results);
	friend class GraphicsView;

protected:
	void closeEvent(QCloseEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;

private slots:

	void open();
	bool saveFont();
	bool saveThisPage();
	bool saveAllPages();
	bool saveAllPages2();
	bool saveAll();
	void searchText();
	

	void about();


private:
	QString fontFileName = "./input/font.dat";
	QString fontSubwordsFileName = "./input/font.subwords.dat";
	void createActions();
	void initRexpClasses();
	void createStatusBar();
	void createDockWindows();
	void readSettings();
	void writeSettings();
	bool saveFile(const QString& fileName);
	void loadFile(const QString& fileName);
	void createSearchWindows();
	PageAnalysisResult& getPageResult(int pageNumber);
	void findExpandables();
	void detectSubWords();
	void handleContextMenu(const QPoint& pos);
	
	
	void searchStretching();
	void searchStretchingBySubWord();
	void savePageFile(int pageNumber, bool overrideFile);

	int maxSubWords = 0;


	QString curFile;
	QSpinBox* integerSpinBox;
	QPushButton* showWordBBButton;
	QPushButton* showSubWordMarks;
	QPushButton* sordByWidth;
	QPushButton* debugBBButton;
	QLabel* pointerPosition;

	QGraphicsScene* scene;
	QGraphicsView* view;

	QMenu* viewMenu;
	QMenu* editMenu;
	QMenu* fileMenu;

	QScrollArea* pathListWidget;
	
	QDockWidget* pathListDock;
	QDockWidget* searchDock;
	
	QLineEdit* searchTextLineEdit;
	QCheckBox* checkBoxMarks;
	QCheckBox* checkBoxIsol;
	QCheckBox* checkBoxInit;
	QCheckBox* checkBoxMedi;
	QCheckBox* checkBoxFina;
	QCheckBox* checkBoxShapes;
	QCheckBox* checkBoxSubWords;

	QTabWidget* tabWidget;

	QGraphicsScene* shapeScene;
	QGraphicsView* shapeView;



	void zoomIn();
	void zoomOut();
	void scaleView(qreal scaleFactor);
	void loadPage(int pageNumber);

	void  testAnalyze();

	QMap<int, PageAnalysisResult> pagesCache;

	QuranSearch quransearch;
};

#endif