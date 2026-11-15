#pragma once
#include "CommonPCH.h"
#include <QJsonObject>
#include <QRegion>
#include <functional>
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

struct BodyContourInput {
    QRectF bounds;
    int contours;
    bool untyped;
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
	QJsonObject kashidaOverrides;
	bool fixedRightBoundary = false;
	bool fixedLeftBoundary = false;
	double topEndpointTolerance = 0;
	// Keep the template's measurement scale when its cut is rotated or shifted.
	double referenceWidth = 0;

	// A closed-loop ligature has two ink bridges to the same inner contour.
	// Baseline endpoints stay on the outer contour for reading-order tests.
	PathExtrema counterEntry{};
	PathExtrema counterExit{};

        // Recovery may place a cut on a returning stroke to the right of its predecessor.
        int readingOrder = -1;
        // Affine coordinates relative to the two corners of the same counter.
        // Zero denotes a straight upper bridge.
        QPointF counterBendWeights;
        QPointF counterBendPoint(QPointF outer, QPointF inner, QPointF other) const {
            return outer + counterBendWeights.x() * (inner - outer) + counterBendWeights.y() * (other - outer);
        }
    std::vector<QLineF> cutSegments() const {
        if (counterEntry.pathIndex <= 0)
            return {QLineF(topBaseLine.point, bottomBaseLine.point)};
        const QLineF lower(counterExit.point, bottomBaseLine.point);
        if (counterBendWeights.isNull())
            return {QLineF(topBaseLine.point, counterEntry.point), lower};
        const auto bend = counterBendPoint(topBaseLine.point, counterEntry.point, counterExit.point);
        return {QLineF(topBaseLine.point, bend), QLineF(bend, counterEntry.point), lower};
    }

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
struct JoinReadingOrder {
    bool operator()(const Stretching& a, const Stretching& b) const {
        if (a.readingOrder != b.readingOrder)
            return a.readingOrder < b.readingOrder;
        if (a.readingOrder >= 0)
            return false;
        return Stretching::cmpbyx{}(a, b);
    }
};
using Joins = std::set<Stretching, JoinReadingOrder>;

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
        // Derived from source geometry after loading. Legacy page-cache layout stays readable.
        int secondaryBodyPath = -1;
        bool bodyCounter = false;
		std::vector<GlyphInfo> baseGlyphs;
		// Join ink in the same local coordinates as baseGlyphs, indexed in reading order.
		std::vector<QPainterPath> kashidaPaths;
		QStringList templateTrace;
		QStringList matchedTemplates;
		QJsonArray letterDiagnostics;
		std::vector<std::pair<QLineF, QLineF>> kashidaCuts;
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
    Apath bodyShape(const WordResultInfo::SubWordInfo& sub) const;

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

using AnchorPartitionCheck = std::function<bool(const WordResultInfo::SubWordInfo&)>;

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



struct OutlineSearchCache;

struct PageAnalysisResult {

	static QMap<ushort, UnicodeProp> charProps;

	static QMap<int, QMap<int, QMap<int, ShapeExceptionRecord>>> nbShapeExceptions;
    static ShapeExceptionRecord wordShapeException(int page, int line, int word, int wordsInLine);
    // First path is the primary body. Result indexes this list, or -1 if inconclusive.
    static int secondaryBodyContour(ShapeException exception, const QString& text, int markQuota, double baseline,
        double scale, const std::vector<BodyContourInput>& members,
        const std::function<QRegion(int)>& inkRegion);
    void resolveCompoundBody(int pageIndex, int lineIndex, int wordIndex);

	static QVector<QStringList> quranText;

	static QVector<QVector<QStringList>> QuranTextByWord;

	static void initQuranText();

	static const arabic_state_table_entry arabic_state_table[][4];

	static const unsigned int joining_mapping[6];

	static QVector<TextInfo> arabic_joining(QString text);
	static int expectedLowerPathCount(ushort codepoint, arabic_joining_t joining);

	struct NotMatched {
		int lineIndex;
		Apath item;
	};

	int loadPage(int pageNumber, AFont* font, bool debug, QGraphicsScene* extScene = nullptr);
	APage page;

	WordResultInfo detectSubWords(int pageIndex, int lineIndex, int wordIndex, AFont* font, bool recompute = false);
	// Conservative ownership repair, shared by fresh segmentation and legacy cache readers.
	QJsonArray resolveWordBoundaries(int pageNumber, AFont* font);
	QJsonArray subwordBoundaryRepairs;
	QJsonArray waqfTypeRepairs;
	QMap<int, QVector<QPainterPath>> headingPaths;

	static const QJsonObject& letterForms();
	static QString prepareLetterForms(QJsonObject& config);
	static void activateLetterForms(const QJsonObject& config);
	// Letters with body slots in the connected outline; keep the source text intact.
	static QString bodyLetters(const QString& text);
	bool segmentSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
	                    bool refresh = false, const QJsonObject* draft = nullptr,
	                    const AnchorPartitionCheck& acceptAnchors = {});


private:
	void restoreWaqfTypes(int pageIndex, int lineIndex, int wordIndex, AFont* font);
	void repairSubwordBoundaries(int pageIndex, int lineIndex, int wordIndex, AFont* font);

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
