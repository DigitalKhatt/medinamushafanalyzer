#pragma once
#include "CommonPCH.h"
#include <set>
#include <vector>
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

struct PathExtrema {
	int pathIndex;
	qreal time;
	QPointF point;
	bool isX;
	bool isY;
	bool isSharp = false;
};

struct Stretching {
	double length;
	PathExtrema topLeft;
	PathExtrema topBaseLine;
	PathExtrema topRight;
	PathExtrema bottomRight;
	PathExtrema bottomBaseLine;
	PathExtrema bottomLeft;
	bool clockWise;

	struct cmpbyx {
		bool operator()(Stretching a, Stretching b) const {
			if (a.bottomBaseLine.point.x() != b.bottomBaseLine.point.x()) {
				return a.bottomBaseLine.point.x() > b.bottomBaseLine.point.x();
			}
			else return a.topBaseLine.point.x() > b.topBaseLine.point.x();
		}
	};

	struct cmpbylength {
		bool operator()(Stretching a, Stretching b) const {
			if (a.length != b.length) {
				return a.length > b.length;
			}
			else if (a.bottomBaseLine.point.x() != b.bottomBaseLine.point.x()) {
				return a.bottomBaseLine.point.x() > b.bottomBaseLine.point.x();
			}
			else return a.topBaseLine.point.x() > b.topBaseLine.point.x();
		}
	};

};


using Stretchings = std::set < Stretching, Stretching::cmpbylength>;
using Joins = std::set < Stretching, Stretching::cmpbyx>;



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

template<typename T>
inline QDataStream& operator<< (QDataStream& out, const std::vector<T>& args)
{
	out << (int)args.size();
	for (const auto& val : args)
	{
		out << val;
	}
	return out;
}
template<typename T>
inline QDataStream& operator>> (QDataStream& in, std::vector<T>& args)
{
	int length = 0;
	in >> length;
	args.clear();
	args.reserve(length);
	for (int i = 0; i < length; ++i)
	{
		typedef typename std::vector<T>::value_type valType;
		valType obj;
		in >> obj;
		args.push_back(obj);
	}
	return in;
}

struct GlyphInfo {
	QString text;
	QPainterPath path;
};

enum class Orientation {
	None,
	ClockWise,
	CounterClockwise
};

struct WordResultInfo {
	struct SubWordInfo {
		QString text;
		//int startIndex;
		//int endIndex;
		std::vector<int> paths;
		std::vector<GlyphInfo> baseGlyphs;
		QVector<PathExtrema> extrema;
		Joins joins;
		bool correct;
		QPainterPath debugPath;
		Orientation basePathOrientation;
		/* overload the operators */
		friend QDataStream& operator<< (QDataStream& out, const SubWordInfo& rhs) {
			out << rhs.text;
			out << rhs.paths;
			return out;
		}

		friend QDataStream& operator>> (QDataStream& in, SubWordInfo& rhs) {
			in >> rhs.text;
			in >> rhs.paths;

			return in;
		}
	};
	struct CharInfo {
		int subWord;
		/* overload the operators */
		friend QDataStream& operator<< (QDataStream& out, const CharInfo& rhs)
		{
			out << rhs.subWord;
			return out;
		}

		friend QDataStream& operator>> (QDataStream& in, CharInfo& rhs)
		{
			in >> rhs.subWord;
			return in;
		}
	};
	std::vector<SubWordInfo> subWords;
	std::vector<CharInfo> charInfos;
	WordResultFlags state = WordResultFlags::NONE;

	/* overload the operators */
	friend QDataStream& operator<< (QDataStream& out, const WordResultInfo& rhs)
	{
		out << rhs.state;
		out << rhs.subWords;
		out << rhs.charInfos;

		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, WordResultInfo& rhs)
	{
		in >> rhs.state;
		in >> rhs.subWords;
		in >> rhs.charInfos;

		return in;
	}
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
		out << rhs.wordResultInfo;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, Word& rhs)
	{
		in >> rhs.paths;
		in >> rhs.text;
		in >> rhs.wordResultInfo;
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
		out << rhs.version;
		out << rhs.lines;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, APage& rhs)
	{
		in >> rhs.version;
		in >> rhs.lines;
		return in;
	}
private:
	double version = 1;
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

	static const arabic_state_table_entry arabic_state_table[][4];

	static const unsigned int joining_mapping[6];

	static QVector<TextInfo> arabic_joining(QString text);

	struct NotMatched {
		int lineIndex;
		Apath item;
	};

	int loadPage(int pageNumber, AFont* font, bool debug, QGraphicsScene* extScene = nullptr);
	APage page;

	WordResultInfo detectSubWords(int pageIndex, int lineIndex, int wordIndex, AFont* font, bool recompute = false);

	bool segmentSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font, bool refresh = false);


private:

	void analyzeSubwords(int pageIndex, int lineIndex, int wordIndex, WordResultInfo& wordResultInfo, AFont* font);
	bool cutSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font, bool refresh = false);	
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