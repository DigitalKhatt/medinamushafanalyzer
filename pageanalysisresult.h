#pragma once
#include "CommonPCH.h"

struct AFont;

template <typename t> void rotateVector(std::vector<t>& v, size_t oldIndex, size_t newIndex)
{
	if (oldIndex > newIndex)
		std::rotate(v.rend() - oldIndex - 1, v.rend() - oldIndex, v.rend() - newIndex);
	else
		std::rotate(v.begin() + oldIndex, v.begin() + oldIndex + 1, v.begin() + newIndex + 1);
}


enum class CustomData : int {
	NBPATH,
	TYPE,
};

enum class CustomDataType : int {
	WAQF = 1,
	SIMILAR,
	TWODOTS,
};

enum class ShapeException {
	NONE,
	LamMediAlefFinaSeparated,
	LamMediHahMediSeparated,
	SeenMediHehMediSeparated,
	AlefHamzaJoined,
	NoonMediJeemMediSeparated,
	InnerContourCollidingProblem,
};

struct ShapeExceptionRecord {
	ShapeException shapeException;
	int subWordPosition;
};



struct Apath {
	QPainterPath path;
	QString name;
	int nbpath;
	CustomDataType type;
	int unicode;
	QTransform transform;
	QPointF pos;

	/* overload the operators */
	friend QDataStream& operator<< (QDataStream& out, const Apath& rhs)
	{
		out << rhs.path;
		out << rhs.name;
		out << rhs.nbpath;
		out << rhs.type;
		out << rhs.unicode;
		out << rhs.transform;
		out << rhs.pos;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, Apath& rhs)
	{
		in >> rhs.path;
		in >> rhs.name;
		in >> rhs.nbpath;
		in >> rhs.type;
		in >> rhs.unicode;
		in >> rhs.transform;
		in >> rhs.pos;
		return in;
	}
};

struct ASubWord {
	QVector<Apath> paths;
	QString text;
};

enum class WordResultFlags : int
{
	NONE = 1,
	DIFFERENT_PATH_NUMBER = 2,
	NBPATH_OVERFLOW_DETECTED = 4,
	NBPATH_OVERFLOW = 8,
	SUBWORD_OVERFLOW = 16,
	NO_INTERSECT_BASELINE = 32,
};

inline WordResultFlags operator|(WordResultFlags lhs, WordResultFlags rhs)
{
	return static_cast<WordResultFlags>(static_cast<int>(lhs) | static_cast<int>(rhs));
}
inline WordResultFlags operator &(WordResultFlags a, WordResultFlags b)
{
	return static_cast<WordResultFlags>(static_cast<int>(a) & static_cast<int>(b));
}

inline WordResultFlags& operator |=(WordResultFlags& a, WordResultFlags b)
{
	return a = a | b;
}

struct WordResultInfo {
	struct SubWordInfo {
		QString text;
		//int startIndex;
		//int endIndex;
		std::vector<int> paths;
	};
	struct CharInfo {
		int subWord;
		//int startIndex;
		//int endIndex;
	};
	std::vector<SubWordInfo> subWords;
	std::vector<CharInfo> charInfos;
	WordResultFlags state = WordResultFlags::NONE;
};

struct Word {
	QVector<Apath> paths;
	QString text;
	WordResultInfo wordResultInfo;

	/* overload the operators */
	friend QDataStream& operator<< (QDataStream& out, const Word& rhs)
	{
		out << rhs.paths;
		out << rhs.text;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, Word& rhs)
	{
		in >> rhs.paths;
		in >> rhs.text;
		return in;
	}
};

struct ALine {
	QVector<Word> words;
	int type;
	qreal baseline;

	/* overload the operators */
	friend QDataStream& operator<< (QDataStream& out, const ALine& rhs)
	{
		out << rhs.words;
		out << rhs.type;
		out << rhs.baseline;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, ALine& rhs)
	{
		in >> rhs.words;
		in >> rhs.type;
		in >> rhs.baseline;
		return in;
	}
};

struct AALine {
	QVector<Apath> words;

	/* overload the operators */
	friend QDataStream& operator<< (QDataStream& out, const AALine& rhs)
	{
		out << rhs.words;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, AALine& rhs)
	{
		in >> rhs.words;
		return in;
	}
};

struct APage {
	QVector<ALine> lines;

	/* overload the operators */
	friend QDataStream& operator<< (QDataStream& out, const APage& rhs)
	{
		out << rhs.lines;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, APage& rhs)
	{
		in >> rhs.lines;
		return in;
	}
};

struct Mushaf {
	QMap<int, APage> pages;
};

struct WordInfo {
	int nbContoursByByWord;
};

struct LineInfo {
	bool isSura;
	qreal baseline;
	qreal ascendant;
	qreal descendant;
	QString text;
	QVector<WordInfo> wordInfos;
};

//From HarfBuzz
enum arabic_joining_t {
	ISOL,
	FINA,
	MEDI,
	INIT,
	NONE,
};

struct TextInfo {
	arabic_joining_t joining;
};

struct UnicodeProp {
	unsigned int nb_paths[5];

	UnicodeProp(unsigned int value = 0) {
		for (int i = 0; i < 5; i++) {
			nb_paths[i] = value;
		}
	}
	UnicodeProp(unsigned int isol, unsigned int init, unsigned int medi, unsigned int fina) {
		nb_paths[ISOL] = isol;
		nb_paths[INIT] = init;
		nb_paths[MEDI] = medi;
		nb_paths[FINA] = fina;
		nb_paths[NONE] = isol;
	}

};

struct arabic_state_table_entry {
	arabic_joining_t prev_action;
	arabic_joining_t curr_action;
	uint16_t next_state;
};



struct PageAnalysisResult {

	static QMap<ushort, UnicodeProp> charProps;

	static QMap<int, QMap<int, QMap<int, ShapeExceptionRecord>>> nbShapeExceptions;

	static QVector<QStringList> quranText;

	static QVector<QVector<QStringList>> QuranTextByWord;

	static void initQuranText();

	struct NotMatched {
		int lineIndex;
		Apath item;
	};
	int loadPage(int pageNumber, AFont* font, bool debug, QGraphicsScene* extScene = nullptr);
	APage page;

	WordResultInfo detectSubWords(int pageIndex, int lineIndex, int wordIndex, AFont* font, bool recompute = false);

	static const arabic_state_table_entry arabic_state_table[][4];

	static const unsigned int joining_mapping[6];

	static QVector<TextInfo> arabic_joining(QString text);


private:

	void analyzeSubwords(int pageIndex, int lineIndex, int wordIndex, WordResultInfo& wordResultInfo, AFont* font);

	QVector<NotMatched> notMatchedItems;
	void analyzePage(int pageNumber, AFont* font, QGraphicsScene* scene, QVector<QSet<QGraphicsPathItem*>>& lines);
	void analyzeTextPage(int pageNumber);
	void setLinesInfo(int pageNumber, AFont* font, QGraphicsScene* scene);
	void debugLines(int pageNumber, AFont* font, QGraphicsScene* scene);
	qreal detectBaseline(const ALine& line, AFont* font);
	qreal detectBaseline2(const ALine& line, AFont* font);
	void detectWords(int pageNumber, AFont* font, QGraphicsScene* scene);
	QString pageText;
	QString suraWord = "سُورَةُ";
	QString bism = "بِسْمِ ٱللَّهِ ٱلرَّحْمَٰنِ ٱلرَّحِيمِ";

	QString surapattern = "^("
		+ suraWord + " .*|"
		+ bism
		+ "|" + "بِّسْمِ ٱللَّهِ ٱلرَّحْمَٰنِ ٱلرَّحِيمِ"
		+ ")$";

	QVector<LineInfo> LinesInfo;

	qreal left;

	qreal firstliney = 92;
	qreal textHeight = 376;
	qreal interline = textHeight / 14;
	qreal ascendant = 18;
	qreal desendant = interline - ascendant; // 6;
	qreal lineWidth = 260;
	qreal leftEvent = 80;
	qreal leftOdd = 40;
};