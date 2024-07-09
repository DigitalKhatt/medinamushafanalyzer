
#include "CommonPCH.h"
#include <private/qbezier_p.h>

ShapeItem::ShapeItem(QGraphicsItem* parent)
	: QGraphicsPathItem(parent)
{
}

ShapeItem::ShapeItem(const QPainterPath& path,
	QGraphicsItem* parent)
	: QGraphicsPathItem(path, parent) {

	std::vector<QPainterPath> subPaths;

	getExtrema(path, subPaths, this->extrema);

	searchStretchings(path, this->stretchings, constants::SCALE_GLYPH);

}

ShapeItem::~ShapeItem()
{
}

#define QT_BEZIER_A(bezier, coord) 3 * (-bezier.coord##1 \
                                        + 3*bezier.coord##2 \
                                        - 3*bezier.coord##3 \
                                        +bezier.coord##4)
#define QT_BEZIER_B(bezier, coord) 6 * (bezier.coord##1 \
                                        - 2*bezier.coord##2 \
                                        + bezier.coord##3)
#define QT_BEZIER_C(bezier, coord) 3 * (- bezier.coord##1 \
                                        + bezier.coord##2)
#define QT_BEZIER_CHECK_T(bezier, t) \
    if (t >= 0 && t <= 1) { \
        QPointF p(b.pointAt(t)); \
        if (p.x() < minx) minx = p.x(); \
        else if (p.x() > maxx) maxx = p.x(); \
        if (p.y() < miny) miny = p.y(); \
        else if (p.y() > maxy) maxy = p.y(); \
    }
//derivative of the equation
static inline qreal slopeAt(qreal t, qreal a, qreal b, qreal c, qreal d)
{
	return 3 * t * t * (d - 3 * c + 3 * b - a) + 6 * t * (c - 2 * b + a) + 3 * (b - a);
}
static QVector<PathExtrema> qt_painterpath_bezier_extrema(int pathIndex, qreal curveIndex, const QBezier& b)
{

	QVector<PathExtrema> extrema;
	// Update for the X extrema
	{
		qreal ax = QT_BEZIER_A(b, x);
		qreal bx = QT_BEZIER_B(b, x);
		qreal cx = QT_BEZIER_C(b, x);
		// specialcase quadratic curves to avoid div by zero
		if (qFuzzyIsNull(ax)) {
			// linear curves are covered by initialization.
			if (!qFuzzyIsNull(bx)) {
				qreal t = -cx / bx;
				if (t >= 0 && t <= 1) {
					extrema.append({ pathIndex, t + curveIndex,b.pointAt(t),true,false });
				}
			}
		}
		else {
			const qreal tx = bx * bx - 4 * ax * cx;
			if (tx >= 0) {
				qreal temp = qSqrt(tx);
				qreal rcp = 1 / (2 * ax);
				qreal t1 = (-bx + temp) * rcp;
				if (t1 >= 0 && t1 <= 1) {
					extrema.append({ pathIndex, t1 + curveIndex,b.pointAt(t1),true,false });
				}
				qreal t2 = (-bx - temp) * rcp;
				if (t2 >= 0 && t2 <= 1) {
					extrema.append({ pathIndex, t2 + curveIndex,b.pointAt(t2),true,false });
				}
			}
		}
	}
	// Update for the Y extrema
	{
		qreal ay = QT_BEZIER_A(b, y);
		qreal by = QT_BEZIER_B(b, y);
		qreal cy = QT_BEZIER_C(b, y);
		// specialcase quadratic curves to avoid div by zero
		if (qFuzzyIsNull(ay)) {
			// linear curves are covered by initialization.
			if (!qFuzzyIsNull(by)) {
				qreal t = -cy / by;
				if (t >= 0 && t <= 1) {
					extrema.append({ pathIndex, t + curveIndex,b.pointAt(t),false,true });
				}
			}
		}
		else {
			const qreal ty = by * by - 4 * ay * cy;
			if (ty > 0) {
				qreal temp = qSqrt(ty);
				qreal rcp = 1 / (2 * ay);
				qreal t1 = (-by + temp) * rcp;
				if (t1 >= 0 && t1 <= 1) {
					extrema.append({ pathIndex, t1 + curveIndex,b.pointAt(t1),false,true });
				}
				qreal t2 = (-by - temp) * rcp;
				if (t2 >= 0 && t2 <= 1) {
					extrema.append({ pathIndex, t2 + curveIndex,b.pointAt(t2),false,true });
				}
			}
		}
	}
	return extrema;
}
QVector<PathExtrema> ShapeItem::getExtrema(const QPainterPath& path, std::vector<QPainterPath>& subPaths, QVector<PathExtrema>& extrema) {

	qreal curveIndex = -1;
	int pathIndex = -1;
	qreal nextXSlope = 0;
	qreal nextYSlope = 0;
	QPainterPath* subPath;

	for (int i = 0; i < path.elementCount(); ++i) {

		qreal prevXSlope = nextXSlope;
		qreal prevYSlope = nextYSlope;

		qreal currXSlope = 0;
		qreal currYSlope = 0;

		QPointF currPoint;

		const QPainterPath::Element& e = path.elementAt(i);
		switch (e.type) {
		case QPainterPath::MoveToElement:
			pathIndex++;
			curveIndex = -1;
			subPaths.push_back({});
			subPath = &subPaths.back();
			subPath->moveTo(path.elementAt(i));
			continue;
		case QPainterPath::LineToElement:
		{
			curveIndex++;
			currPoint = path.elementAt(i - 1);
			QPointF nextPoint = path.elementAt(i);

			currXSlope = nextPoint.x() - currPoint.x();
			currYSlope = nextPoint.y() - currPoint.y();

			nextXSlope = currXSlope;
			nextYSlope = currYSlope;

			subPath->lineTo(nextPoint);


			break;
		}
		case QPainterPath::CurveToElement:
		{
			curveIndex++;
			QBezier b = QBezier::fromPoints(path.elementAt(i - 1),
				e,
				path.elementAt(i + 1),
				path.elementAt(i + 2));
			auto locextrama = qt_painterpath_bezier_extrema(pathIndex, curveIndex, b);

			subPath->cubicTo(path.elementAt(i), path.elementAt(i + 1), path.elementAt(i + 2));

			extrema.append(locextrama);

			currXSlope = slopeAt(0, b.x1, b.x2, b.x3, b.x4);
			currYSlope = slopeAt(0, b.y1, b.y2, b.y3, b.y4);

			nextXSlope = slopeAt(1, b.x1, b.x2, b.x3, b.x4);
			nextYSlope = slopeAt(1, b.y1, b.y2, b.y3, b.y4);

			currPoint = path.elementAt(i - 1);


			i += 2;
		}
		break;
		default:
			break;
		}

		if (e.type != QPainterPath::MoveToElement) {
			if (currXSlope > 0 && prevXSlope < 0 || (currXSlope < 0 && prevXSlope > 0)) {
				extrema.append({ pathIndex, curveIndex,currPoint,true,false,true });
			}
			else if (currYSlope > 0 && prevYSlope < 0 || (currYSlope < 0 && prevYSlope > 0)) {
				extrema.append({ pathIndex, curveIndex,currPoint,false,true,true });
			}
			else {
				auto currLine = QLineF(0, 0, currXSlope, currYSlope);
				auto prevLine = QLineF(0, 0, prevXSlope, prevYSlope);
				auto angle = prevLine.angleTo(currLine);
				if (angle > 3 && angle < 357) {
					extrema.append({ pathIndex, curveIndex,currPoint,false,false,true });
				}

			}
		}
	}

	return extrema;
}
void ShapeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {




	painter->setPen(this->pen());
	painter->setBrush(this->brush());
	QGraphicsPathItem::paint(painter, option, widget);


	auto path = this->path();
	auto pen = QPen(Qt::cyan);
	auto brush = QBrush(Qt::white);
	pen.setCosmetic(true);
	pen.setWidthF(1);
	int radius = 3;

	QColor blue(4, 100, 166, 50);
	QColor color2(166, 100, 166);


	for (int i = 0; i < path.elementCount(); ++i) {
		const QPainterPath::Element& elm = path.elementAt(i);
		switch (elm.type) {
		case QPainterPath::MoveToElement: {
			pen.setColor(color2);
			painter->setPen(pen);
			painter->setBrush(color2);
			QRectF  rect = QRectF(elm.x - radius, elm.y - radius, 2 * radius, 2 * radius);
			painter->drawEllipse(rect);
			break;
		}
		case QPainterPath::LineToElement: {
			pen.setColor(blue);
			painter->setPen(pen);
			painter->setBrush(blue);
			QRectF  rect = QRectF(elm.x - radius, elm.y - radius, 2 * radius, 2 * radius);
			painter->drawEllipse(rect);

			if (i - 1 >= 0 && path.elementAt(i - 1).type == QPainterPath::MoveToElement) {


				pen.setColor(color2);

				const QPainterPath::Element& prePoint = path.elementAt(i - 1);
				pen.setWidthF(3);
				painter->setPen(pen);
				QLineF line{ prePoint.x, prePoint.y, elm.x, elm.y };

				painter->drawLine(line);

				pen.setWidthF(1);

			}

			break;
		}
		case QPainterPath::CurveToElement:
		{


			if (i - 1 >= 0 && path.elementAt(i - 1).type == QPainterPath::MoveToElement) {


				pen.setColor(color2);
				pen.setWidthF(3);
				painter->setPen(pen);
				painter->setBrush(color2);
				const QPainterPath::Element& prePoint = path.elementAt(i - 1);

				QLineF line{ prePoint.x, prePoint.y, elm.x, elm.y };

				painter->drawLine(line);

				pen.setWidthF(1);

			}


			const QPainterPath::Element& control1 = elm;
			const QPainterPath::Element& control2 = path.elementAt(i + 1);
			const QPainterPath::Element& last = path.elementAt(i + 2);

			pen.setColor(Qt::cyan);
			painter->setPen(pen);
			painter->setBrush(brush);
			QRectF  rect = QRectF(control1.x - radius, control1.y - radius, 2 * radius, 2 * radius);
			//painter->drawEllipse(rect);

			pen.setColor(Qt::magenta);
			painter->setPen(pen);
			rect = QRectF(control2.x - radius, control2.y - radius, 2 * radius, 2 * radius);

			//painter->drawEllipse(rect);

			pen.setColor(blue);
			painter->setPen(pen);
			painter->setBrush(blue);
			rect = QRectF(last.x - radius, last.y - radius, 2 * radius, 2 * radius);
			painter->drawEllipse(rect);


			i += 2;
			break;
		}
		default:
			qFatal("MyQPdf::generatePath(), unhandled type: %d", elm.type);
		}
	}

	for (auto& stretching : stretchings) {


		QVector<QPointF> vertex{ stretching.topLeft.point,stretching.topBaseLine.point,stretching.topRight.point,stretching.bottomRight.point,
			stretching.bottomBaseLine.point,stretching.bottomLeft.point };

		QPolygonF polygon{ vertex };


		pen.setColor(Qt::green);
		painter->setPen(pen);


		painter->drawPolyline(polygon);

		QLineF  line = QLineF(stretching.bottomBaseLine.point, stretching.topBaseLine.point);
		pen.setColor(Qt::darkCyan);
		painter->setPen(pen);

		painter->drawLine(line);


		/*
		auto& subPath = subPaths[stretching.bottomBaseLine.pathIndex];

		auto bbox = subPath.boundingRect();

		//QRectF rec{ stretching.topBaseLine.point.x(),bbox.top(),1,stretching.topBaseLine.point.y() - bbox.top() - 0.1 * constants::SCALE_GLYPH };

		auto atop = stretching.bottomBaseLine.point.y() + 0.1 * constants::SCALE_GLYPH;
		QRectF rec{ stretching.bottomBaseLine.point.x(),atop,1,bbox.bottom() - atop };


		painter->drawRect(rec);*/
	}

	for (auto& xextr : extrema) {

		pen.setColor(Qt::yellow);
		painter->setPen(pen);
		if (xextr.isY) {
			painter->setBrush(Qt::red);
		}
		else if (xextr.isX) {

			painter->setBrush(Qt::blue);
		}
		else {

			painter->setBrush(Qt::green);
		}

		QRectF  rect = QRectF(xextr.point.x() - radius, xextr.point.y() - radius, 2 * radius, 2 * radius);
		//painter->drawEllipse(rect);

	}




}

void ShapeItem::searchStretchings(const QPainterPath& path, Stretchings& stretchings, double scaled) {


	double scale = std::abs(scaled);

	double maxYDiffWithBaseLine = 3 * scale;
	double maxXBaseDiff = 1 * scale;
	double minXBaseDiff = 0.0 * scale;
	double maxYBaseDiff = 1.5 * scale;
	double minYBaseDiff = 1 * scale;
	double minLength = 2 * scale;
	double alefThreshold = 6 * scale;

	std::vector<QPainterPath> subPaths;

	QVector<PathExtrema> extr;

	ShapeItem::getExtrema(path, subPaths, extr);

	int currentPathIndex = 0;


	std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) { return a.time > b.time; }) > timeSorted;
	std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) { return a.point.x() > b.point.x(); }) > xSorted;
	std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) { return a.point.y() > b.point.y(); }) > ySorted;

	QPainterPath currentPath;


	for (auto it = extr.begin(); it != extr.end(); it++) {
		if (it->pathIndex == currentPathIndex) {
			timeSorted.insert(*it);
			xSorted.insert(*it);
			ySorted.insert(*it);
			if (std::next(it) != extr.end()) {
				continue;
			}
		}

		for (auto xiter = xSorted.size() != 0 ? std::next(xSorted.begin()) : xSorted.end(); xiter != xSorted.end(); xiter++) {
			auto& curr = *xiter;

			int lookbackward = 2;

			auto prevBasePoint = std::prev(xiter, 1);

			while (lookbackward != 0 && prevBasePoint != xSorted.begin()) {

				auto yDiff = std::abs(curr.point.y() - prevBasePoint->point.y());
				auto xDiff = std::abs(curr.point.x() - prevBasePoint->point.x());


				if (xDiff > maxXBaseDiff || xDiff < minXBaseDiff || yDiff > maxYBaseDiff || yDiff < minYBaseDiff) {
					lookbackward--;
					prevBasePoint = std::prev(prevBasePoint, 1);
					continue;
				}
				else {
					break;
				}
			}

			if (prevBasePoint == xSorted.begin() || lookbackward == 0) {
				continue;
			}

			auto& prec = *prevBasePoint;


			Stretching res;

			if (prec.point.y() > curr.point.y()) {
				res.bottomBaseLine = prec;
				res.topBaseLine = curr;

			}
			else {
				res.bottomBaseLine = curr;
				res.topBaseLine = prec;
			}

			auto item = timeSorted.find(res.bottomBaseLine);
			auto prev = std::prev(item);
			auto next = std::next(item);

			if (next == timeSorted.end() || prev == timeSorted.end()) {
				continue;
			}

			if (prev->point.x() < next->point.x()) {
				res.bottomLeft = *prev;
				res.bottomRight = *next;
			}
			else {
				res.bottomRight = *prev;
				res.bottomLeft = *next;
			}

			if (res.bottomLeft.point.x() > res.bottomBaseLine.point.x()
				|| res.bottomRight.point.x() < res.bottomBaseLine.point.x()
				|| res.bottomLeft.point.y() > res.bottomBaseLine.point.y()
				|| res.bottomRight.point.y() > res.bottomBaseLine.point.y()
				) {
				continue;
			}

			item = timeSorted.find(res.topBaseLine);
			prev = std::prev(item);
			next = std::next(item);

			if (next == timeSorted.end() || prev == timeSorted.end()) {
				continue;
			}

			if (prev->point.x() < next->point.x()) {
				res.topLeft = *prev;
				res.topRight = *next;
			}
			else {
				res.topRight = *prev;
				res.topLeft = *next;
			}

			if (res.topLeft.point.x() > res.topBaseLine.point.x()
				|| res.topRight.point.x() < res.topBaseLine.point.x()
				|| res.topLeft.point.y() > res.topBaseLine.point.y()
				|| res.topRight.point.y() > res.topBaseLine.point.y()
				) {
				continue;
			}

			// Should be same direction
			bool dir = res.topBaseLine.time > res.topLeft.time;
			int dirChange = 0;
			if (dir && res.topRight.time < res.topBaseLine.time || !dir && res.topRight.time > res.topBaseLine.time) {
				dirChange++;
			}
			if (dir && res.bottomRight.time < res.topRight.time || !dir && res.bottomRight.time > res.topRight.time) {
				dirChange++;
			}
			if (dir && res.bottomBaseLine.time < res.bottomRight.time || !dir && res.bottomBaseLine.time > res.bottomRight.time) {
				dirChange++;
			}
			if (dir && res.bottomLeft.time < res.bottomBaseLine.time || !dir && res.bottomLeft.time > res.bottomBaseLine.time) {
				dirChange++;
			}
			if (dir && res.topLeft.time < res.bottomLeft.time || !dir && res.topLeft.time > res.bottomLeft.time) {
				dirChange++;
			}

			if (dirChange != 1) {
				continue;
			}

			auto topLength = res.topRight.point.x() - res.topLeft.point.x();
			auto bottonLength = res.bottomRight.point.x() - res.bottomLeft.point.x();

			res.length = std::min(topLength, bottonLength);

			if (res.length < minLength) {
				continue;
			}

			//Do not include if last or first in subwords
			// do not exclude final alef
			if (std::abs(res.bottomBaseLine.point.y() - res.topLeft.point.y()) < alefThreshold)
			{

				auto item = xSorted.find(res.bottomLeft);
				if (std::next(item) == xSorted.end()) {
					continue;
				}
				item = xSorted.find(res.bottomRight);
				if (std::prev(item) == xSorted.begin()) {
					continue;
				}

				auto item1 = timeSorted.find(res.bottomLeft);
				auto item2 = timeSorted.find(res.topLeft);
				if (std::next(item1) == item2 || std::next(item2) == item1) {
					continue;
				}
				else if (std::next(item1) == std::prev(item2) || std::next(item2) == std::prev(item1)) {
					continue;
				}
				else {

					auto item3 = std::next(item1);
					auto item4 = std::prev(item2);
					if (item3 != timeSorted.end() && std::next(item3) == item4) {
						continue;
					}
					else {
						auto item3 = std::prev(item1);
						auto item4 = std::next(item2);
						if (item4 != timeSorted.end() && std::next(item4) == item3) {
							continue;
						}
					}
				}
			}

			//Test if intersects with shape above (such as kaf)

			auto& subPath = subPaths[currentPathIndex];

			auto bbox = subPath.boundingRect();

			auto height = res.topBaseLine.point.y() - bbox.top() - 0.1 * scale;
			if (height > 0) {
				QRectF rec{ res.topBaseLine.point.x(),bbox.top(),1,height };
				if (subPath.intersects(rec)) {
					continue;
				}
			}
			auto atop = res.bottomBaseLine.point.y() + 0.1 * scale;
			height = bbox.bottom() - atop;
			if (height > 0) {
				QRectF rec{ res.bottomBaseLine.point.x(),atop,1,height };
				if (subPath.intersects(rec)) {
					continue;
				}
			}


			stretchings.insert(res);




		}

		timeSorted.clear();
		xSorted.clear();
		ySorted.clear();
		currentPath.clear();

		currentPathIndex++;
	}

}
