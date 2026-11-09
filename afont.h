#pragma once
#include "aglyph.h"
#include <QHash>
#include <optional>

enum class CompareMethod {
	AREA = 1,
	TF
};

struct AFont {
	double version = 1;
	QMultiMap<QString, AGlyph> glyphs;

	int checkGlyph(const QPainterPath& path, QString glyphName, CompareMethod compareMethod = CompareMethod::AREA, double errorRatio = constants::MATCHING_ERROR_RATIO, double* value = nullptr);

	int checkGlyph(const QPainterPath& inpath, QChar qchar, double errorRatio);	
	// Uniform scale only; preserve shape/aspect ratio and use the native area tolerance.
	int checkGlyphScaled(const QPainterPath& path, const QString& glyphName,
	                     double errorRatio = constants::MATCHING_ERROR_RATIO, double* value = nullptr) const;

	// For short-lived font instances used to analyze a single page.
	void cacheAreaComparisons() { areaComparisons.emplace(); }

	static double compareQPainterPath(const QPainterPath& path1, const QPainterPath& path2);

	static double scaleAndCompareQPainterPath(QPainterPath path1, QPainterPath path2, bool transform1, bool transform2);

	static long getArea(QPainterPath path);

	static long getArea(QPolygon path);

	static double compareQPainterPathTF(const QPainterPath& path1, const QPainterPath& path2);


	/* overload the operators */
	friend QDataStream& operator<< (QDataStream& out, const AFont& rhs)
	{
		out << rhs.version << rhs.glyphs;
		return out;
	}

	friend QDataStream& operator>> (QDataStream& in, AFont& rhs)
	{
		double version;
		in >> version;
		in >> rhs.glyphs;

		return in;
	}

private:
	double compareGlyphAreas(const QPainterPath& reference, const QPainterPath& outline) const;
	mutable std::optional<QHash<QByteArray, double>> areaComparisons;

};
