#include "afont.h"

#include "TFMatcherCpp/Matcher.h"

long AFont::getArea(QPainterPath path) {
	auto region = QRegion(path.toFillPolygon().toPolygon());
	long area = 0;
	for (auto rec : region) {
		area += rec.width() * rec.height();
	}
	return area;
}

long AFont::getArea(QPolygon poly) {
	auto region = QRegion(poly);
	long area = 0;
	for (auto rec : region) {
		area += rec.width() * rec.height();
	}
	return area;
}

int AFont::checkGlyph(const QPainterPath& inpath, QString glyphName, CompareMethod compareMethod, double errorRatio, double* value)
{
	// add 0.1 to avoid error
	double adjust = 0.1;
	if (compareMethod == CompareMethod::TF) {
		adjust = 0.0;
	}
	QTransform transform{ constants::SCALE_GLYPH + adjust,0,0,-constants::SCALE_GLYPH + adjust,0,0 };
	QPainterPath path = inpath * transform;
	auto box = path.boundingRect();
	path.translate(-box.left(), -box.top());

	double minValue = MAXINT;
	int index = -1;
	QMultiMap<QString, AGlyph>::iterator minValueIter = glyphs.end();

	QMultiMap<QString, AGlyph>::iterator i;

	if (glyphName.isEmpty()) {
		i = glyphs.begin();
	}
	else {
		i = glyphs.find(glyphName);
	}


	while (i != glyphs.end() && (glyphName.isEmpty() || i.key() == glyphName)) {
		auto& glyph = i.value();

		double value;
		if (compareMethod == CompareMethod::AREA) {
			value = compareQPainterPath(glyph.path, path);
		}
		else {
			value = compareQPainterPathTF(glyph.path, path);
		}
#ifdef MYDEBUG
		if (glyphName.isEmpty()) {
			qDebug() << "Result diff with " << (int)compareMethod << " for " << glyph.name << value;
		}
		
#endif

		if (value < minValue) {
			minValue = value;			
			minValueIter = i;
		}

		
		++i;
	}

	if (minValueIter != glyphs.end()) {
#ifdef MYDEBUG
		if (glyphName.isEmpty()) {
			qDebug() << "Best Result with  " << (int)compareMethod << " for " << minValueIter.value().name << minValue << "\n";
		}
#endif
	}

	if (value != nullptr) {
		*value = minValue;
	}

	if (minValue < errorRatio) {
		index = 0;
	}

	return index;
}

/*
double AFont::compareQPainterPath(const QPainterPath& path1, const QPainterPath& path2)
{
	auto diff1 = path1.subtracted(path2);
	auto diff2 = path2.subtracted(path1);

	//auto region1 = QRegion(diff1.toFillPolygon().toPolygon()).rects();
	//auto region2 = QRegion(diff2.toFillPolygon().toPolygon()).rects();

	double diffArea = getArea(diff1) + getArea(diff2);
	double oriArea = getArea(path1) + getArea(path2);

	return diffArea / oriArea;

}*/

double AFont::compareQPainterPath(const QPainterPath& path1, const QPainterPath& path2)
{
	auto intersection = path1.intersected(path2);
	auto unionf = path1.united(path2);

	double intArea = getArea(intersection);
	double uniArea = getArea(unionf);

	return 1 - (intArea / uniArea);

}

double AFont::scaleAndCompareQPainterPath(QPainterPath path1, QPainterPath path2, bool transform1 = false, bool transform2 = false)
{


	QTransform transform{ constants::SCALE_GLYPH + .1,0,0,-constants::SCALE_GLYPH + 0.1,0,0 };

	if (transform1) {
		path1 = path1 * transform;
		auto box1 = path1.boundingRect();
		path1.translate(-box1.left(), -box1.top());
	}

	if (transform2) {
		path2 = path2 * transform;
		auto box2 = path2.boundingRect();
		path2.translate(-box2.left(), -box2.top());
	}

	auto diff1 = path1.subtracted(path2);
	auto diff2 = path2.subtracted(path1);

	auto region1 = QRegion(diff1.toFillPolygon().toPolygon()).rects();
	auto region2 = QRegion(diff2.toFillPolygon().toPolygon()).rects();

	double diffArea = getArea(diff1) + getArea(diff2);
	double oriArea = getArea(path1) + getArea(path2);

	return diffArea / oriArea;


}

static double perimeter(const QPolygonF& polygon) {
	double peri = 0.0;
	for (int i = 1; i < polygon.length(); i++) {
		peri += QLineF(polygon[i - 1], polygon[i]).length();
	}

	return peri;
}
//TODO : USE centroid distance signature with FD (best result) : See A Comparative Study of Curvature Scale Space and Fourier Descriptors
// see https://www.math.uci.edu/icamp/summer/research_11/park/shape_descriptors_survey_part3.pdf
// see docs\CurveMatching\Centroid\Burger-Burge2013_Book_PrinciplesOfDigitalImageProces.pdf for fourier implementation details in JAVA
double AFont::compareQPainterPathTF(const QPainterPath& path1, const QPainterPath& path2) {

	//TODO: test if equi-distant sampling do better

	auto firstpolygons = path1.toSubpathPolygons();

	auto secondpolygons = path2.toSubpathPolygons();

	if (firstpolygons.size() != secondpolygons.size()) {
		//return -1;
	}

	double dis = 0;
	double sim = 0;


	for (int i = 0; i < firstpolygons.size() && i < secondpolygons.size(); i++) {
		auto& first = firstpolygons[i];
		auto& second = secondpolygons[i];

		TFMatcherCpp::Polygon poly1;
		TFMatcherCpp::Polygon poly2;

		for (auto& vertex : first) {
			poly1.AddVertex(TFMatcherCpp::Vector2(vertex.x(), vertex.y()));
		}

		for (auto& vertex : second) {
			poly2.AddVertex(TFMatcherCpp::Vector2(vertex.x(), vertex.y()));
		}

		TFMatcherCpp::Matcher matcher(poly1, poly2);

		auto ret = matcher.Distance();

		dis = std::max(std::get<0>(ret), dis);

		double k = 500;
		double comp1 = std::get<1>(ret);
		double comp2 = std::get<2>(ret);

		double locsim = (std::powf(2, -dis / (k + comp1 + comp2)));

		sim = std::max(locsim, sim);

	}

	return dis;
}

int AFont::checkGlyph(const QPainterPath& inpath, QChar qchar, double errorRatio = constants::MATCHING_ERROR_RATIO) {

	if (qchar.unicode() == 0x064E || qchar.unicode() == 0x0650) {
		QTransform transform{ constants::SCALE_GLYPH,0,0,-constants::SCALE_GLYPH,0,0 };
		QPainterPath path = inpath * transform;
		auto box = path.boundingRect();
		path.translate(-box.left(), -box.top());
		box = path.boundingRect();

		QPainterPath fatha;

		fatha.moveTo(box.left(), box.bottom());
		fatha.lineTo(box.left() + 14, box.bottom() - 45);
		fatha.lineTo(box.right(), box.top());
		fatha.lineTo(box.right() - 14, box.top() + 45);
		fatha.closeSubpath();


		auto value = compareQPainterPath(fatha, path);

		if (value < 0.1) {
			return 0;
		}
	}
	else if (qchar.unicode() == 0x064F) {
		return checkGlyph(inpath, "damma", CompareMethod::AREA, constants::MATCHING_ERROR_RATIO);
	}

	return -1;

}







