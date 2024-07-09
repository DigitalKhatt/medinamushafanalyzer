#include "pageanalysisresult.h"
#include "CustomOutputDev.h"
#include "PDFDoc.h"
#include "GlobalParams.h"
#include "afont.h"
#include "quran.h"
#include <format>
#include "qfile.h"
#include <private/qbezier_p.h>
#include <cstdlib>

using timesortedtype = std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) { return a.time > b.time; }) > ;
using xsortedtype = std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) {
	if (a.point.x() == b.point.x()) {
		return a.point.y() > b.point.y();
	}
	else {
		return a.point.x() > b.point.x();
	}
}) > ;
using ysortedtype = std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) {
	if (a.point.y() == b.point.y()) {
		return a.point.x() > b.point.x();
	}
	else {
		return a.point.y() > b.point.y();
	}
}) > ;

struct SearchContext {
	QPainterPath transformedPath;
	xsortedtype xSorted;
	ysortedtype ySorted;
	timesortedtype timeSorted;
	Stretching res;
	xsortedtype::iterator nextIter;
	QRectF bbox;
	WordResultInfo::SubWordInfo& subword;
	QString baseText;
	std::vector<QPainterPath> subPaths;
	Orientation orientation;

	timesortedtype::iterator prev(timesortedtype::iterator time) {
		auto prev = std::prev(time);
		if (prev == timeSorted.end()) {
			prev = --timeSorted.end();
		}
		return prev;
	}

	timesortedtype::iterator next(timesortedtype::iterator time) {
		auto next = std::next(time);
		if (next == timeSorted.end()) {
			next = timeSorted.begin();
		}
		return next;
	}

	void setOrientation() {

		auto minY = ySorted.begin();
		auto C = timeSorted.find(*minY);


		auto B = std::prev(C);
		if (B == timeSorted.end()) {
			B = --timeSorted.end();
		}

		auto initB = B;

		double det;


		do {
			auto A = B;
			B = C;

			C = std::next(B);
			if (C == timeSorted.end()) {
				C = timeSorted.begin();
			}

			det = (B->point.x() - A->point.x()) * (C->point.y() - A->point.y()) - (C->point.x() - A->point.x()) * (B->point.y() - A->point.y());


		} while (det == 0 && B != initB);

		orientation = det < 0 ? Orientation::ClockWise : Orientation::CounterClockwise;

	}
};

struct JoinTime {
	double minTime = -1;
	double maxTime = 100000;
	bool startAtLeft = false;

	bool inline isLeft(double value) {
		return (startAtLeft && (value <= minTime || value >= maxTime)) || (!startAtLeft && value >= minTime && value <= maxTime);
	}

	bool isRight(double value) {
		return (!startAtLeft && (value <= minTime || value >= maxTime)) || (startAtLeft && value >= minTime && value <= maxTime);
	}

	bool inline isStrictLeft(double value) {
		return (startAtLeft && (value < minTime || value > maxTime)) || (!startAtLeft && value > minTime && value < maxTime);
	}

	bool isStrictRight(double value) {
		return (!startAtLeft && (value < minTime || value > maxTime)) || (startAtLeft && value > minTime && value < maxTime);
	}
};

JoinTime static getJoinTime(Stretching join) {
	JoinTime joinTime;

	if (join.topBaseLine.time > join.bottomBaseLine.time) {
		joinTime.minTime = join.bottomBaseLine.time;
		joinTime.maxTime = join.topBaseLine.time;
		joinTime.startAtLeft = !join.clockWise;
	}
	else {
		joinTime.maxTime = join.bottomBaseLine.time;
		joinTime.minTime = join.topBaseLine.time;
		joinTime.startAtLeft = join.clockWise;
	}

	return joinTime;
}


JoinTime static getJoinTime(Stretching join, Orientation pathOrient) {
	JoinTime joinTime;

	if (join.topBaseLine.time > join.bottomBaseLine.time) {
		joinTime.minTime = join.bottomBaseLine.time;
		joinTime.maxTime = join.topBaseLine.time;
		joinTime.startAtLeft = pathOrient != Orientation::ClockWise;
	}
	else {
		joinTime.maxTime = join.bottomBaseLine.time;
		joinTime.minTime = join.topBaseLine.time;
		joinTime.startAtLeft = pathOrient == Orientation::ClockWise;
	}

	return joinTime;
}

JoinTime static getJoinTime(SearchContext& context) {
	JoinTime joinTime;

	if (context.subword.joins.size() > 0) {
		auto& lastJoin = *context.subword.joins.rbegin();
		joinTime = getJoinTime(lastJoin);
	}

	return joinTime;
}
/*based on https://www.particleincell.com/2013/cubic-line-intersection/
and http://mysite.verizon.net/res148h4j/javascript/script_exact_cubic.html#the%20source%20code */
static std::vector<double> cubicRoots(double a, double b, double c, double d)
{

	std::vector<double> roots;
	auto sgn = [](double x)
	{
		if (x < 0.0) return -1;
		return 1;
	};


	double A = b / a;
	double B = c / a;
	double C = d / a;


	double Q = (3.0 * B - std::pow(A, 2.0)) / 9.0;
	double R = (9.0 * A * B - 27.0 * C - 2.0 * std::pow(A, 3.0)) / 54.0;
	double D = std::pow(Q, 3.0) + std::pow(R, 2.0);    // polynomial discriminant

	double t[3];

	if (D >= 0)                                 // complex or duplicate roots
	{
		auto S = sgn(R + std::sqrt(D)) * std::pow(std::abs(R + std::sqrt(D)), (1.0 / 3));
		auto T = sgn(R - std::sqrt(D)) * std::pow(std::abs(R - std::sqrt(D)), (1.0 / 3));

		t[0] = -A / 3.0 + (S + T);                    // real root
		t[1] = -A / 3.0 - (S + T) / 2.0;                  // real part of complex root
		t[2] = -A / 3.0 - (S + T) / 2.0;                  // real part of complex root
		auto Im = std::abs(std::sqrt(3.0) * (S - T) / 2.0);    // complex part of root pair   

		/*discard complex roots*/
		if (Im != 0)
		{
			t[1] = -1;
			t[2] = -1;
		}

	}
	else                                          // distinct real roots
	{
		double th = std::acos(R / std::sqrt(-std::pow(Q, 3)));

		t[0] = 2.0 * std::sqrt(-Q) * std::cos(th / 3.0) - A / 3.0;
		t[1] = 2.0 * std::sqrt(-Q) * std::cos((th + 2.0 * M_PI) / 3.0) - A / 3.0;
		t[2] = 2.0 * std::sqrt(-Q) * std::cos((th + 4.0 * M_PI) / 3.0) - A / 3.0;

	}

	/*discard out of spec roots*/
	for (auto i = 0; i < 3; i++) {
		if (t[i] < 0 || t[i]>1.0 || isnan(t[i])) continue;
		roots.push_back(t[i]);
	}

	return roots;
}

static void FindInterections(const QBezier& b, const QLineF& line, int pathIndex, int curveIndex, std::vector<PathExtrema>& intersections, bool segment = false) {

	auto C0 = b.pt1();
	auto C1 = b.pt2();
	auto C2 = b.pt3();
	auto C3 = b.pt4();

	auto A = line.p1();
	auto B = line.p2();

	double Ax = 3 * (C1.x() - C2.x()) + C3.x() - C0.x();
	double Ay = 3 * (C1.y() - C2.y()) + C3.y() - C0.y();

	double Bx = 3 * (C0.x() - 2 * C1.x() + C2.x());
	double By = 3 * (C0.y() - 2 * C1.y() + C2.y());

	double Cx = 3 * (C1.x() - C0.x());
	double Cy = 3 * (C1.y() - C0.y());

	double Dx = C0.x();
	double Dy = C0.y();

	double vx = B.y() - A.y();
	double vy = A.x() - B.x();

	double d = A.x() * vx + A.y() * vy;

	/*
	PolynomialRoots::Cubic csolve(vx * Ax + vy * Ay,
		vx * Bx + vy * By,
		vx * Cx + vy * Cy,
		vx * Dx + vy * Dy - d);

	double result1[3];

	csolve.getRealRoots(result1);*/

	auto result = cubicRoots(vx * Ax + vy * Ay,
		vx * Bx + vy * By,
		vx * Cx + vy * Cy,
		vx * Dx + vy * Dy - d);

	for (auto t : result) {
		if (0 > t || t > 1) continue;

		auto point = QPointF{ ((Ax * t + Bx) * t + Cx) * t + Dx, ((Ay * t + By) * t + Cy) * t + Dy };

		if (segment) {
			auto minX = std::min(line.p1().x(), line.p2().x());
			auto maxX = std::max(line.p1().x(), line.p2().x());
			auto minY = std::min(line.p1().y(), line.p2().y());
			auto maxY = std::max(line.p1().y(), line.p2().y());
			if (point.x() > maxX
				|| point.x() < minX
				|| point.y() > maxY
				|| point.y() < minY
				) continue;
		}

		PathExtrema intersect;

		intersect.pathIndex = pathIndex;
		intersect.time = curveIndex + t;
		intersect.point = point;
		intersections.push_back(intersect);

	}
}
static std::vector<PathExtrema> FindInterections(const QPainterPath& path, const QLineF& line, bool segment = false) {

	std::vector<PathExtrema> res;
	qreal curveIndex = -1;
	int pathIndex = -1;

	for (int i = 0; i < path.elementCount(); ++i) {
		const QPainterPath::Element& e = path.elementAt(i);
		switch (e.type) {
		case QPainterPath::MoveToElement:
			curveIndex = -1;
			pathIndex++;
			break;
		case QPainterPath::LineToElement: {
			curveIndex++;
			QLineF currentLine{ path.elementAt(i - 1), e };
			QPointF intersetion;
			if (currentLine.intersects(line, &intersetion) == QLineF::BoundedIntersection) {
				PathExtrema intersect;
				intersect.pathIndex = pathIndex;
				QLineF newLine{ path.elementAt(i - 1), intersetion };
				intersect.time = curveIndex + (newLine.length() / currentLine.length());
				intersect.point = intersetion;
				res.push_back(intersect);
			}
			break;
		}
		case QPainterPath::CurveToElement: {
			curveIndex++;
			QBezier b = QBezier::fromPoints(path.elementAt(i - 1),
				e,
				path.elementAt(i + 1),
				path.elementAt(i + 2));
			i += 2;
			FindInterections(b, line, pathIndex, curveIndex, res, segment);
			break;
		}
		case QPainterPath::CurveToDataElement:
			Q_ASSERT(!"toSubpaths(), bad element type");
			break;
		}
	}

	std::sort(res.begin(), res.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() == b.point.y() ? a.point.x() < b.point.x() : a.point.y() < b.point.y(); });
	auto last = std::unique(res.begin(), res.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.x() == b.point.x() && a.point.y() == b.point.y(); });
	res.erase(last, res.end());
	return res;

}



static double scale = std::abs(1);

static double maxYDiffWithBaseLine = 3 * scale;
static double maxXBaseDiff = 1 * scale;
static double minXBaseDiff = 0.0 * scale;
static double maxYBaseDiff = 1.5 * scale;
static double minYBaseDiff = 0.8 * scale;
static double minLength = 2 * scale;
static double alefThreshold = 6 * scale;

static std::function<bool(xsortedtype::iterator)> defaultCheck = [](xsortedtype::iterator xiter) {return true; };

struct JoinCheck {
	bool bottomCheck = true;
	bool topCheck = true;
	bool sameDirectionCheck = true;
	std::function<bool(xsortedtype::iterator)> customCheck = defaultCheck;
};

bool static getJoin(SearchContext& context, const JoinCheck& joinCheck,
	int plookbackward = 4, int prevOff = 1, double minXBaseDiff = ::minXBaseDiff, double maxXBaseDiff = ::maxXBaseDiff,
	double minYBaseDiff = ::minYBaseDiff, double maxYBaseDiff = ::maxYBaseDiff) {

	auto xiter = context.nextIter;
	auto& xSorted = context.xSorted;
	auto& res = context.res;
	auto& timeSorted = context.timeSorted;
	auto& transformedPath = context.transformedPath;
	double minDistance = 0.01;

	for (; xiter != xSorted.end(); xiter++) {
		auto& curr = *xiter;

		auto prevBasePoint = std::prev(xiter, prevOff);

		auto lookbackward = plookbackward;

		while (lookbackward != 0 && prevBasePoint != xSorted.end()) {

			auto& prec = *prevBasePoint;

			prevBasePoint = std::prev(prevBasePoint, 1);

			auto yDiff = std::abs(curr.point.y() - prec.point.y());
			auto xDiff = std::abs(curr.point.x() - prec.point.x());


			if (xDiff > maxXBaseDiff || xDiff < minXBaseDiff || yDiff > maxYBaseDiff || yDiff < minYBaseDiff) {
				lookbackward--;
				continue;
			}

			if (prec.point.y() > curr.point.y()) {
				res.bottomBaseLine = prec;
				res.topBaseLine = curr;

			}
			else {
				res.bottomBaseLine = curr;
				res.topBaseLine = prec;
			}

			if (context.subword.joins.size() > 0) {
				auto& lastJoin = *context.subword.joins.rbegin();
				if (lastJoin.bottomBaseLine.time == res.bottomBaseLine.time
					|| lastJoin.bottomBaseLine.time == res.topBaseLine.time
					|| lastJoin.topBaseLine.time == res.bottomBaseLine.time
					|| lastJoin.topBaseLine.time == res.topBaseLine.time) {
					continue;
				}
			}


			auto item = timeSorted.find(res.bottomBaseLine);
			auto prev = item;
			auto next = item;

			int nbTentative = 3;

			while (nbTentative != 0) {
				nbTentative--;
				prev = std::prev(prev);

				if (prev == timeSorted.end()) {
					prev = --timeSorted.end();
				}

				if (std::abs(res.bottomBaseLine.point.x() - prev->point.x()) > minDistance || std::abs(res.bottomBaseLine.point.y() - prev->point.y()) > minDistance) {
					break;
				}
			}
			nbTentative = 3;

			while (nbTentative != 0) {
				nbTentative--;

				next = std::next(next);

				if (next == timeSorted.end()) {
					next = timeSorted.begin();
				}

				if (std::abs(res.bottomBaseLine.point.x() - next->point.x()) > minDistance || std::abs(res.bottomBaseLine.point.y() - next->point.y()) > minDistance) {
					break;
				}

			}

			bool bottomClockWiseOrder;
			if (prev->point.x() < next->point.x()) {
				res.bottomLeft = *prev;
				res.bottomRight = *next;
				bottomClockWiseOrder = true;
			}
			else {
				res.bottomRight = *prev;
				res.bottomLeft = *next;
				bottomClockWiseOrder = false;
			}

			if (joinCheck.bottomCheck) {
				if (res.bottomLeft.point.x() > res.bottomBaseLine.point.x()
					|| res.bottomRight.point.x() < res.bottomBaseLine.point.x()
					|| res.bottomLeft.point.y() > res.bottomBaseLine.point.y()
					|| res.bottomRight.point.y() > res.bottomBaseLine.point.y()
					) {
					//lookbackward--;
					continue;
				}
			}

			item = timeSorted.find(res.topBaseLine);
			prev = item;
			next = item;

			nbTentative = 3;

			while (nbTentative != 0) {
				nbTentative--;

				next = std::next(next);

				if (next == timeSorted.end()) {
					next = timeSorted.begin();
				}

				if (std::abs(res.topBaseLine.point.x() - next->point.x()) > minDistance || std::abs(res.topBaseLine.point.y() - next->point.y()) > minDistance) {
					break;
				}

			}

			nbTentative = 3;

			while (nbTentative != 0) {
				nbTentative--;

				prev = std::prev(prev);


				if (prev == timeSorted.end()) {
					prev = --timeSorted.end();
				}

				if (std::abs(res.topBaseLine.point.x() - prev->point.x()) > minDistance || std::abs(res.topBaseLine.point.y() - prev->point.y()) > minDistance) {
					break;
				}

			}

			bool topClockWiseOrder;

			if (prev->point.x() < next->point.x()) {
				res.topLeft = *prev;
				res.topRight = *next;
				topClockWiseOrder = false;
			}
			else {
				res.topRight = *prev;
				res.topLeft = *next;
				topClockWiseOrder = true;
			}

			if (joinCheck.topCheck) {
				if (res.topLeft.point.x() > res.topBaseLine.point.x()
					|| res.topRight.point.x() < res.topBaseLine.point.x()
					|| res.topLeft.point.y() > res.topBaseLine.point.y()
					|| res.topRight.point.y() > res.topBaseLine.point.y()
					) {
					//lookbackward--;
					continue;
				}
			}

			if (joinCheck.sameDirectionCheck) {
				if (topClockWiseOrder != bottomClockWiseOrder) {
					//lookbackward--;
					continue;
				}
			}


			res.clockWise = topClockWiseOrder;

			//check if joining segment belongs to the interior of the path area
			QLineF segment{ res.topBaseLine.point,res.bottomBaseLine.point };

			if (!transformedPath.contains(segment.center())) {
				//lookbackward--;
				continue;
			}

			if (!joinCheck.customCheck(xiter)) {
				//lookbackward--;
				continue;
			}

			/*
			QPainterPath temp;
			temp.addPolygon(QPolygonF{ {res.bottomLeft.point + QPointF(0, 0.03) ,res.bottomBaseLine.point + QPointF(-0.03,0),QPointF{res.bottomLeft.point.x(),res.bottomBaseLine.point.y()}} });

			if (!transformedPath.intersects(temp)) {
				continue;
			}

			context.subword.debugPath.addPath(temp);*/

			auto rightKash = std::min(res.topRight.point.x(), res.bottomRight.point.x());
			auto leftKash = std::max(res.topLeft.point.x(), res.bottomLeft.point.x());

			res.length = rightKash - leftKash;


			context.nextIter = std::next(xiter);
			return true;
		}
	}

	return false;

}

bool static searchWawFina(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.bottomCheck = false;



	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		auto& res = context.res;

		//bottom Check
		if (res.bottomLeft.point.x() > res.bottomBaseLine.point.x()
			|| res.bottomRight.point.x() < res.bottomBaseLine.point.x()
			|| res.bottomLeft.point.y() < res.bottomBaseLine.point.y() // diff
			|| res.bottomRight.point.y() > res.bottomBaseLine.point.y()
			) {
			return false;
		}

		auto center = context.res.topLeft.point;

		QPainterPath temp;

		temp.addRect(QRectF{ QPointF{ center.x() - 0.2,context.bbox.top() } ,QPointF{ center.x() + 0.2,context.bbox.bottom()} });

		temp.setFillRule(Qt::WindingFill);
		auto intersected = context.transformedPath.intersected(temp);

		int nbpath = 0;

		for (int elemIndex = 0; elemIndex < intersected.elementCount(); elemIndex++) {
			auto elem = intersected.elementAt(elemIndex);
			if (elem.type == QPainterPath::MoveToElement) {
				nbpath++;
			}
		}

		if (nbpath != 2) {
			return false;
		}

		return true;
	};

	return getJoin(context, joinCheck);

}

bool static searchSeen(SearchContext& context) {

	auto startIter = context.nextIter;

	auto prevJoinTime = getJoinTime(context);


	JoinCheck joinCheck;

	joinCheck.customCheck = [&context, &startIter, &prevJoinTime](xsortedtype::iterator currentIter) {

		auto& res = context.res;

		std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) {
			return a.point.y() < b.point.y();
		}) > yss;

		auto currentjoinTime = getJoinTime(res);

		for (auto iter = context.timeSorted.begin(); iter != context.timeSorted.end(); iter++) {
			auto& value = *iter;

			if (/*value.isY && !value.isSharp &&*/ prevJoinTime.isStrictLeft(value.time) && currentjoinTime.isStrictRight(value.time)) {
				yss.insert(value);
			}
		}

		if (yss.size() < 14) {
			return false;
		}
		auto firstPoint = *yss.begin();
		auto secondPoint = *std::next(yss.begin(), 1);
		auto thirdPoint = *std::next(yss.begin(), 2);

		std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) {
			return a.point.x() < b.point.x();
		}) > xss;

		xss.insert(firstPoint);
		xss.insert(secondPoint);
		xss.insert(thirdPoint);

		firstPoint = *xss.begin();
		secondPoint = *std::next(xss.begin(), 1);
		thirdPoint = *std::next(xss.begin(), 2);

		/*
		if (firstPoint.point.x() < thirdPoint.point.x() || firstPoint.point.y() > thirdPoint.point.y()) {
			return false;
		}

		if (!(secondPoint.point.x() < firstPoint.point.x() && secondPoint.point.x() > thirdPoint.point.x())) {
			return false;
		}*/


		QPointF topLeft = { thirdPoint.point.x(),firstPoint.point.y() };
		QPointF bottomRight = { firstPoint.point.x(),thirdPoint.point.y() };

		QPainterPath temp;

		QPolygonF polygon{ {res.topBaseLine.point,firstPoint.point ,secondPoint.point ,thirdPoint.point,res.bottomBaseLine.point} };

		//temp.addRect({ topLeft, bottomRight });
		temp.addPolygon(polygon);

		//temp.setFillRule(Qt::WindingFill);
		auto intersected = context.transformedPath.intersected(temp);

		int nbpath = 0;

		for (int elemIndex = 0; elemIndex < intersected.elementCount(); elemIndex++) {
			auto elem = intersected.elementAt(elemIndex);
			if (elem.type == QPainterPath::MoveToElement) {
				nbpath++;
			}
		}

		if (nbpath < 2) {
			return false;
		}

		return true;
	};

	return getJoin(context, joinCheck);

}

bool static searchNextHehMedi(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.bottomCheck = false;
	joinCheck.topCheck = false;

	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		auto xDiff = context.res.bottomBaseLine.point.x() - context.res.topBaseLine.point.x();

		if (!(xDiff > 1.5 && xDiff < 3.5)) {
			return false;
		}

		if (//res.bottomLeft.point.x() > res.bottomBaseLine.point.x() ||
			context.res.bottomRight.point.x() < context.res.bottomBaseLine.point.x()
			//|| res.bottomLeft.point.y() > res.bottomBaseLine.point.y()
			|| context.res.bottomRight.point.y() > context.res.bottomBaseLine.point.y()
			) {
			return false;
		}

		if (//res.topLeft.point.x() > res.topBaseLine.point.x() ||
			context.res.topRight.point.x() < context.res.topBaseLine.point.x()
			//|| res.topLeft.point.y() > res.topBaseLine.point.y()
			|| context.res.topRight.point.y() > context.res.topBaseLine.point.y()
			) {
			return false;
		}

		auto bbox = context.transformedPath.boundingRect();

		QLineF segment{ context.res.topBaseLine.point,context.res.bottomBaseLine.point };

		auto center = segment.center();

		center = context.res.topBaseLine.point;

		QPainterPath temp;

		temp.addRect(QRectF{ QPointF{ center.x() - 0.2,bbox.top() } ,QPointF{ center.x() + 0.2,bbox.bottom()} });

		temp.setFillRule(Qt::WindingFill);

		// bug in intersected : does not take into account the fillrule in intersected when intersecting a rectangle
		// see QPathClipper function QPainterPath QPathClipper::clip(Operation operation) line 1604 if (subjectIsRect) 
		// -> minus one segment = 3 segements (= number subpaths)
		// auto intersected = temp.subtracted(transformedPath);			
		auto intersected = context.transformedPath.intersected(temp);

		int nbpath = 0;

		for (int elemIndex = 0; elemIndex < intersected.elementCount(); elemIndex++) {
			auto elem = intersected.elementAt(elemIndex);
			if (elem.type == QPainterPath::MoveToElement) {
				nbpath++;
			}
		}

		if (nbpath != 3) {
			return false;
		}

		return true;
	};

	return getJoin(context, joinCheck, 4, 2, 1.5, 3.5, 0.5, maxYBaseDiff);


}
bool static searchNextSadOrTah(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.bottomCheck = true;
	joinCheck.topCheck = false;

	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		auto xDiff = context.res.bottomBaseLine.point.x() - context.res.topBaseLine.point.x();
		auto yDiff = context.res.bottomBaseLine.point.y() - context.res.topBaseLine.point.y();



		if (context.res.topLeft.point.x() < context.res.topBaseLine.point.x() || // inversed
			context.res.topRight.point.x() < context.res.topBaseLine.point.x()
			|| context.res.topLeft.point.y() > context.res.topBaseLine.point.y()
			|| context.res.topRight.point.y() > context.res.topBaseLine.point.y()
			) {
			return false;
		}

		auto bbox = context.transformedPath.boundingRect();

		QLineF segment{ context.res.topBaseLine.point,context.res.bottomBaseLine.point };

		auto center = segment.center();

		QLineF line{ {center.x() ,bbox.top()}, {center.x() ,bbox.bottom()} };

		auto intersections = FindInterections(context.transformedPath, line);

		if (intersections.size() != 4) {
			return false;
		}

		auto x = context.res.topBaseLine.point.x() + 1.5;

		line = { {x ,bbox.top()}, {x ,bbox.bottom()} };

		intersections = FindInterections(context.transformedPath, line);

		if (intersections.size() < 2) {
			return false;
		}

		std::sort(intersections.begin(), intersections.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() > b.point.y(); });

		context.res.bottomBaseLine = intersections[0];
		context.res.topBaseLine = intersections[1];


		return true;
	};

	return getJoin(context, joinCheck, 14, 1, 3, 6.5, 1, 2);


}

bool static searchBehInitFollowedByHah(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.bottomCheck = false;
	joinCheck.topCheck = false;
	joinCheck.sameDirectionCheck = false;

	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		auto& res = context.res;

		auto xDiff = context.res.bottomBaseLine.point.x() - context.res.topBaseLine.point.x();
		auto yDiff = context.res.bottomBaseLine.point.y() - context.res.topBaseLine.point.y();

		if (res.bottomLeft.point.x() < res.bottomBaseLine.point.x() //diff
			|| res.bottomRight.point.x() < res.bottomBaseLine.point.x()
			|| res.bottomLeft.point.y() < res.bottomBaseLine.point.y() // diff
			|| res.bottomRight.point.y() > res.bottomBaseLine.point.y()
			) {
			return false;
		}

		if (context.res.topLeft.point.x() < context.res.topBaseLine.point.x() || // inversed
			context.res.topRight.point.x() < context.res.topBaseLine.point.x()
			|| context.res.topLeft.point.y() > context.res.topBaseLine.point.y()
			|| context.res.topRight.point.y() < context.res.topBaseLine.point.y() // inverted
			) {
			return false;
		}

		auto bbox = context.transformedPath.boundingRect();

		QLineF segment{ context.res.topBaseLine.point,context.res.bottomBaseLine.point };

		auto center = segment.center();

		QLineF line{ {center.x() ,bbox.top()}, {center.x() ,bbox.bottom()} };

		auto intersections = FindInterections(context.transformedPath, line);

		if (intersections.size() != 4) {
			return false;
		}

		// reverse order
		auto temp = context.res.bottomBaseLine;
		context.res.bottomBaseLine = context.res.topBaseLine;
		context.res.topBaseLine = temp;

		temp = context.res.bottomLeft;
		context.res.bottomLeft = context.res.topRight;
		context.res.topRight = temp;

		temp = context.res.bottomRight;
		context.res.bottomRight = context.res.topLeft;
		context.res.topLeft = temp;




		return true;
	};

	// beh init hah ligature
	auto ret = getJoin(context, joinCheck, 6, 1, 1.5, 3, 0.5, 1.5);
	if (!ret) {
		JoinCheck noCheck;
		ret = getJoin(context, noCheck);
	}
	else {
		context.nextIter = context.xSorted.find(context.res.bottomBaseLine);
	}

	return ret;


}

bool static searchNextReh(SearchContext& context) {

	auto& xSorted = context.xSorted;
	auto& timeSorted = context.timeSorted;

	/*
	auto curr = xSorted.rbegin();

	for (curr++; curr != xSorted.rend(); curr++) {
		auto prec = std::prev(curr, 1);
		auto yDiff = std::abs(curr->point.y() - prec->point.y());
		auto xDiff = std::abs(curr->point.x() - prec->point.x());
		std::cout << "curr=(" << curr->point.x() << "," << curr->point.y()
			<< "),prec=(" << prec->point.x() << "," << prec->point.y()
			<< "),xDiff=" << xDiff << ",yDiff=" << yDiff
			<< std::endl;
	}*/

	auto firstPoint = xSorted.end();
	firstPoint--;

	auto firstPointTime = timeSorted.find(*firstPoint);
	auto prevFirstPointTime = firstPointTime;
	bool foundPrev;
	bool foundNext;
	do {
		prevFirstPointTime = std::prev(prevFirstPointTime);
		if (prevFirstPointTime == timeSorted.end()) {
			prevFirstPointTime = --timeSorted.end();
		}
		auto prevYDiff = firstPointTime->point.y() - prevFirstPointTime->point.y();
		foundPrev = prevYDiff > 1.5;
	} while (!foundPrev && prevFirstPointTime != firstPointTime);

	auto nextFirstPointTime = firstPointTime;
	double nextYDiff;
	do {
		nextFirstPointTime = std::next(nextFirstPointTime);
		if (nextFirstPointTime == timeSorted.end()) {
			nextFirstPointTime = timeSorted.begin();
		}
		auto nextYDiff = firstPointTime->point.y() - nextFirstPointTime->point.y();
		foundNext = nextYDiff > 1.5;
	} while (!foundNext && nextFirstPointTime != firstPointTime);

	if (foundPrev && foundNext) {

		auto& res = context.res;


		if (nextFirstPointTime->point.x() > prevFirstPointTime->point.x()) {
			res.bottomBaseLine = *nextFirstPointTime;
			res.topBaseLine = *prevFirstPointTime;
		}
		else {
			res.bottomBaseLine = *prevFirstPointTime;
			res.topBaseLine = *nextFirstPointTime;
		}

		auto x = res.bottomBaseLine.point.x();
		auto y = res.bottomBaseLine.point.y();

		/*
		QLineF line = { {x + 0.1 ,y - 2}, {x + 0.1,y - 0.1} };

		auto intersections = FindInterections(context.transformedPath, line, true);

		if (intersections.size() != 1) {
			std::cout << "Reh error" << std::endl;
			return false;
		}

		res.topBaseLine = intersections[0];*/

		res.bottomLeft = res.bottomBaseLine;
		res.bottomRight = res.bottomBaseLine;

		res.topLeft = res.topBaseLine;
		res.topRight = res.topBaseLine;

		res.length = res.bottomBaseLine.point.x() - firstPoint->point.x();
		res.clockWise = false;

		context.nextIter = firstPoint;
		return true;

	}




	return false;


}

bool static searchQafFina(SearchContext& context) {

	enum class State {
		Halt = -1,
		Init = 1,
		Descending = 2,
		Final = 100
	};

	auto& xSorted = context.xSorted;
	auto& timeSorted = context.timeSorted;

	auto firstPoint = xSorted.end();
	firstPoint--;
	auto initPointTime = timeSorted.end();
	auto currentPointTime = timeSorted.find(*firstPoint);
	auto step = &SearchContext::prev;
	State state = State::Init;


	while (state != State::Halt && state != State::Final && currentPointTime != initPointTime) {
		switch (state) {
		case State::Init: {
			auto prev = context.prev(currentPointTime);
			if (prev->point.y() < currentPointTime->point.y()) {
				step = &SearchContext::next;
			}
			state = State::Descending;
			break;
		}
		case State::Descending: {
			auto next = (context.*step)(currentPointTime);
			if (next->point.y() < currentPointTime->point.y() && (initPointTime == timeSorted.end() || next->point.y() < initPointTime->point.y())) {
				state = State::Final;
			}
			if (initPointTime == timeSorted.end()) {
				initPointTime = currentPointTime;
			}
			currentPointTime = next;
			break;
		}
		}
	}

	if (state == State::Final) {

		auto x = currentPointTime->point.x();
		auto y = currentPointTime->point.y();

		QLineF line = { {x + 0.2 ,y - 2}, {x + 0.2,y + 2} };

		auto intersections = FindInterections(context.transformedPath, line);

		if (intersections.size() < 2) {
			return false;
		}



		auto& res = context.res;

		res.bottomBaseLine = intersections.end()[-1];
		res.topBaseLine = intersections.end()[-2];
		res.bottomLeft = res.bottomBaseLine;
		res.bottomRight = res.bottomBaseLine;
		res.topLeft = res.topBaseLine;
		res.topRight = res.topBaseLine;

		res.length = 0;

		context.nextIter = firstPoint;
		return true;

	}

	return false;
}
bool static searchYehFina(SearchContext& context) {

	enum class State {
		Halt = -1,
		Init = 1,
		Descending = 2,
		Asending = 3,
		Left = 4,
		Right = 5,
		Final = 100
	};

	auto& xSorted = context.xSorted;
	auto& timeSorted = context.timeSorted;

	auto firstPoint = xSorted.end();
	firstPoint--;
	auto initPointTime = timeSorted.find(*firstPoint);
	auto bottomPointTime = initPointTime;
	auto step = &SearchContext::prev;
	State state1 = State::Init;
	int iter = -2;

	while (state1 != State::Halt && state1 != State::Final && ++iter < (int)timeSorted.size()) {
		switch (state1) {
		case State::Init: {
			auto prev = context.prev(bottomPointTime);
			if (prev->point.y() < bottomPointTime->point.y()) {
				step = &SearchContext::next;
			}
			state1 = State::Descending;
			break;
		}
		case State::Descending: {
			auto next = (context.*step)(bottomPointTime);
			if (next->point.y() < bottomPointTime->point.y()) {
				state1 = State::Right;
			}
			bottomPointTime = next;
			break;
		}
		case State::Right: {
			auto next = (context.*step)(bottomPointTime);
			if (next->point.x() < bottomPointTime->point.x()) {
				state1 = State::Left;
			}
			bottomPointTime = next;
			break;
		}
		case State::Left: {
			auto next = (context.*step)(bottomPointTime);
			if (next->point.x() > bottomPointTime->point.x()) {
				state1 = State::Final;
				break;
			}
			bottomPointTime = next;
			break;
		}
		}
	}

	if (state1 != State::Final) return false;


	step = step == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
	iter = -2;
	auto topPointTime = initPointTime;
	State state2 = State::Asending;

	while (state2 != State::Halt && state2 != State::Final && ++iter < (int)timeSorted.size()) {
		switch (state2) {
		case State::Asending: {
			auto next = (context.*step)(topPointTime);
			if (next->point.y() > topPointTime->point.y()) {
				state2 = State::Descending;
			}
			topPointTime = next;
			break;
		}
		case State::Descending: {
			auto next = (context.*step)(topPointTime);
			if (next->point.y() < topPointTime->point.y()) {
				state2 = State::Right;
			}
			topPointTime = next;
			break;
		}
		case State::Right: {
			auto next = (context.*step)(topPointTime);
			if (next->point.x() < topPointTime->point.x()) {
				state2 = State::Left;
			}
			topPointTime = next;
			break;
		}
		case State::Left: {
			auto next = (context.*step)(topPointTime);
			if (next->point.x() > topPointTime->point.x()) {
				state2 = State::Final;
				break;
			}
			topPointTime = next;
			break;
		}
		}
	}

	if (state2 != State::Final) return false;

	auto& res = context.res;

	res.bottomBaseLine = *bottomPointTime;
	res.topBaseLine = *topPointTime;
	res.bottomLeft = res.bottomBaseLine;
	res.bottomRight = res.bottomBaseLine;
	res.topLeft = res.topBaseLine;
	res.topRight = res.topBaseLine;

	res.length = 0;

	context.nextIter = firstPoint;
	return true;
}
bool static searchAinInit(SearchContext& context) {

	enum class State {
		Halt = -1,
		Init = 1,
		Descending = 2,
		Asending = 3,
		Left = 4,
		Right = 5,
		Final = 100
	};

	auto& xSorted = context.xSorted;
	auto& timeSorted = context.timeSorted;
	auto& res = context.res;

	auto firstPoint = xSorted.begin();
	auto initPointTime = timeSorted.find(*firstPoint);
	auto bottomPointTime = initPointTime;
	//auto step = &SearchContext::prev;
	auto step = context.orientation == Orientation::ClockWise ? &SearchContext::prev : &SearchContext::next;
	State state1 = State::Descending;
	int iter = -2;

	while (state1 != State::Halt && state1 != State::Final && ++iter < (int)timeSorted.size()) {
		switch (state1) {		
		case State::Descending: {
			auto next = (context.*step)(bottomPointTime);
			if (next->point.y() < bottomPointTime->point.y()) {
				state1 = State::Final;
				res.bottomLeft = *next;
				break;
			}
			res.bottomRight = *bottomPointTime;
			bottomPointTime = next;
			break;
		}
		}
	}

	if (state1 != State::Final) return false;


	step = step == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
	iter = -2;
	auto topPointTime = initPointTime;
	State state2 = State::Asending;
	int ascending = 1;
	while (state2 != State::Halt && state2 != State::Final && ++iter < (int)timeSorted.size()) {
		switch (state2) {
		case State::Asending: {
			auto next = (context.*step)(topPointTime);
			if (next->point.y() > topPointTime->point.y()) {
				state2 = ascending == 1 ? State::Left : State::Descending;
			}
			res.topRight = *topPointTime;
			topPointTime = next;
			break;
		}
		case State::Left: {
			auto next = (context.*step)(topPointTime);
			if (next->point.x() > topPointTime->point.x()) {
				state2 = State::Right;
			}
			res.topRight = *topPointTime;
			topPointTime = next;
			break;
		}
		case State::Right: {
			auto next = (context.*step)(topPointTime);
			if (next->point.x() < topPointTime->point.x()) {
				state2 = State::Asending;
				ascending++;
			}
			res.topRight = *topPointTime;
			topPointTime = next;
			break;
		}
		case State::Descending: {
			auto next = (context.*step)(topPointTime);
			if (next->point.y() < topPointTime->point.y()) {
				res.topLeft = *next;
				state2 = State::Final;
				break;
			}
			res.topRight = *topPointTime;
			topPointTime = next;
			break;
		}
		}
	}

	if (state2 != State::Final) return false;

	res.bottomBaseLine = *bottomPointTime;
	res.topBaseLine = *topPointTime;

	auto rightKash = std::min(res.topRight.point.x(), res.bottomRight.point.x());
	auto leftKash = std::max(res.topLeft.point.x(), res.bottomLeft.point.x());

	res.length = rightKash - leftKash;

	context.nextIter = firstPoint;
	return true;
}
bool static searchLamInit(SearchContext& context) {

	enum class State {
		Halt = -1,
		Init = 1,
		Descending = 2,
		Asending = 3,
		Left = 4,
		Right = 5,
		Final = 100
	};

	auto& xSorted = context.xSorted;
	auto& timeSorted = context.timeSorted;
	auto& res = context.res;

	auto firstPoint = xSorted.begin();
	auto initPointTime = timeSorted.find(*firstPoint);
	auto bottomPointTime = initPointTime;
	auto step = context.orientation == Orientation::ClockWise ? &SearchContext::prev : &SearchContext::next;
	State state1 = State::Descending;
	int iter = -2;

	while (state1 != State::Halt && state1 != State::Final && ++iter < (int)timeSorted.size()) {
		switch (state1) {
		case State::Descending: {
			auto next = (context.*step)(bottomPointTime);
			if (next->point.y() < bottomPointTime->point.y() || next->point.x() > bottomPointTime->point.x()) {
				state1 = State::Final;
				res.bottomLeft = *next;
				break;
			}
			res.bottomRight = *bottomPointTime;
			bottomPointTime = next;
			break;
		}
		}
	}

	if (state1 != State::Final) return false;


	step = step == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
	iter = -2;
	auto topPointTime = initPointTime;
	State state2 = State::Asending;
	while (state2 != State::Halt && state2 != State::Final && ++iter < (int)timeSorted.size()) {
		switch (state2) {
		case State::Asending: {
			auto next = (context.*step)(topPointTime);
			if (next->point.y() > topPointTime->point.y()) {
				state2 = State::Descending;
			}
			res.topRight = *topPointTime;
			topPointTime = next;
			break;
		}
		case State::Descending: {
			auto next = (context.*step)(topPointTime);
			if (next->point.y() < topPointTime->point.y()) {
				res.topLeft = *next;
				state2 = State::Final;
				break;
			}
			res.topRight = *topPointTime;
			topPointTime = next;
			break;
		}
		}
	}

	if (state2 != State::Final) return false;

	res.bottomBaseLine = *bottomPointTime;
	res.topBaseLine = *topPointTime;

	auto rightKash = std::min(res.topRight.point.x(), res.bottomRight.point.x());
	auto leftKash = std::max(res.topLeft.point.x(), res.bottomLeft.point.x());

	res.length = rightKash - leftKash;

	context.nextIter = firstPoint;
	return true;
}
//TODO correct kashida length
bool static searchNextRehOld(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.bottomCheck = false;
	joinCheck.topCheck = false;

	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		auto& res = context.res;

		//bottom Check
		if (res.bottomLeft.point.x() > res.bottomBaseLine.point.x()
			|| res.bottomRight.point.x() < res.bottomBaseLine.point.x()
			|| res.bottomLeft.point.y() < res.bottomBaseLine.point.y() // diff
			//|| res.bottomRight.point.y() < res.bottomBaseLine.point.y() // diff
			) {
			return false;
		}

		if (res.topLeft.point.x() > res.topBaseLine.point.x()
			|| res.topRight.point.x() > res.topBaseLine.point.x() // diff
			|| res.topLeft.point.y() < res.topBaseLine.point.y() // diff
			|| res.topRight.point.y() > res.topBaseLine.point.y()
			) {
			return false;
		}

		if (res.bottomBaseLine.point.x() - res.bottomLeft.point.x() < 3) return false;
		if (res.bottomLeft.point.y() - res.bottomBaseLine.point.y() < 3) return false;
		if (res.topBaseLine.point.x() - res.topLeft.point.x() < 3) return false;
		if (res.topLeft.point.y() - res.topBaseLine.point.y() < 3) return false;

		std::vector<PathExtrema> intersections;

		if (res.bottomRight.point.y() < res.bottomBaseLine.point.y()) {
			auto x = res.bottomBaseLine.point.x() + 0.5;
			auto y = res.bottomBaseLine.point.y();

			QLineF line = { {x ,y - 2}, {x ,y + 2} };

			intersections = FindInterections(context.transformedPath, line);


		}
		else {

			auto x = res.bottomRight.point.x();
			auto y = res.bottomRight.point.y();

			QLineF line = { {x ,y - 2}, {x ,y + 0.5} };

			intersections = FindInterections(context.transformedPath, line);


		}

		if (intersections.size() != 2) {
			return false;
		}

		std::sort(intersections.begin(), intersections.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() > b.point.y(); });

		context.res.bottomBaseLine = intersections[0];
		context.res.topBaseLine = intersections[1];



		return true;
	};



	return getJoin(context, joinCheck, 4, 1, 0, 0.8, 0, 0.8);


}

bool static searchKaf(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		//Test if intersects with shape below



		auto& bbox = context.bbox;

		auto& res = context.res;

		auto& subPath = context.transformedPath;

		auto atop = res.bottomBaseLine.point.y() + 0.1 * scale;
		auto height = bbox.bottom() - atop;
		if (height > 0) {
			QRectF rec{ res.bottomBaseLine.point.x(),atop,1,height };
			if (subPath.intersects(rec)) {
				return false;
			}
		}


		return true;
	};

	auto ret = getJoin(context, joinCheck);

	/*
	if (ret) {

		auto& res = context.res;
		auto topLength = res.topBaseLine.point.x() - res.topLeft.point.x();
		auto bottonLength = res.bottomBaseLine.point.x() - res.bottomLeft.point.x();

		res.length = std::min(topLength, bottonLength);
	}*/

	return ret;


}


bool static searchHah(SearchContext& context, bool behhahinit) {

	auto startIter = context.nextIter;

	auto prevJoinTime = getJoinTime(context);

	JoinCheck joinCheck;

	joinCheck.customCheck = [&context, &startIter, &prevJoinTime, behhahinit](xsortedtype::iterator currentIter) {

		auto& res = context.res;

		auto currentjoinTime = getJoinTime(res);

		std::set < PathExtrema, decltype([](PathExtrema a, PathExtrema b) {
			return a.time < b.time;
		}) > yss;

		for (auto iter = context.timeSorted.begin(); iter != context.timeSorted.end(); iter++) {
			auto& value = *iter;

			if (prevJoinTime.isStrictLeft(value.time) && currentjoinTime.isStrictRight(value.time)) {
				yss.insert(value);
			}
		}

		if (!behhahinit && yss.size() < 8) {
			return false;
		}
		else if (yss.size() < 3) {
			return false;
		}

		return true;
	};

	//check hah before ascendant	

	double x;

	if (context.subword.joins.size() > 0) {
		auto& lastJoin = *context.subword.joins.rbegin();
		x = lastJoin.bottomBaseLine.point.x();
	}
	else {
		x = context.bbox.right();
	}

	QLineF line{ {x - 6 ,context.bbox.top()}, {x - 6,context.bbox.bottom()} };

	auto intersections = FindInterections(context.transformedPath, line);

	if (intersections.size() == 4) {
		std::sort(intersections.begin(), intersections.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() < b.point.y(); });
		for (auto& subPath : context.subPaths) {
			if (subPath.contains(intersections[1].point)) {
				auto x = subPath.boundingRect().left() - 0.6;

				line = { {x  ,context.bbox.top()}, {x,context.bbox.bottom()} };

				auto intersections = FindInterections(context.transformedPath, line);
				if (intersections.size() == 2) {
					std::sort(intersections.begin(), intersections.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() > b.point.y(); });
					context.res.bottomBaseLine = intersections[0];
					context.res.topBaseLine = intersections[1];
					context.res.topLeft = context.res.topBaseLine;
					context.res.topRight = context.res.topBaseLine;
					context.res.bottomLeft = context.res.bottomBaseLine;
					context.res.bottomRight = context.res.bottomBaseLine;
					context.res.length = 0;
					return true;
				}
			}

		}
	}


	return getJoin(context, joinCheck);

}
bool static searchYehFinaOld(SearchContext& context) {

	JoinCheck joinCheck;

	if (!getJoin(context, joinCheck)) {
		return false;
	}

	auto res = context.res;
	auto nextIter = context.nextIter;

	auto ret2 = getJoin(context, joinCheck);

	if (ret2) {
		context.res = res;
		context.nextIter = nextIter;
		return true;
	}

	auto nextFunc = res.clockWise ? std::prev <timesortedtype::iterator> : std::next< timesortedtype::iterator>;

	auto item = context.timeSorted.find(res.topBaseLine);
	auto prev = item;
	auto curr = nextFunc(item, 1);
	auto nbXExtremum = 2;
	while (curr != context.timeSorted.end() && nbXExtremum != 0) {
		if (curr->isX) {
			--nbXExtremum;
		}
		prev = curr;
		curr = nextFunc(curr, 1);
	}
	if (nbXExtremum != 0) return false;
	PathExtrema topBaseLine = *prev;

	nextFunc = !res.clockWise ? std::prev <timesortedtype::iterator> : std::next< timesortedtype::iterator>;
	item = context.timeSorted.find(res.bottomBaseLine);
	prev = item;
	curr = nextFunc(item, 1);
	nbXExtremum = 2;
	while (curr != context.timeSorted.end() && nbXExtremum != 0) {
		if (curr->isX) {
			--nbXExtremum;
		}
		prev = curr;
		curr = nextFunc(curr, 1);
	}
	if (nbXExtremum != 0) return false;
	PathExtrema bottomBaseLine = *prev;

	context.res.bottomBaseLine = bottomBaseLine;
	context.res.topBaseLine = topBaseLine;
	context.res.length = 0;

	return true;


}
bool static searchNoonFina(SearchContext& context) {

	JoinCheck joinCheck;

	auto res = context.res;
	auto nextIter = context.nextIter;

	auto prevRes = context.res;
	auto prevNextIter = context.nextIter;

	int nbFound = 0;

	while (getJoin(context, joinCheck)) {
		prevRes = res;
		prevNextIter = nextIter;
		res = context.res;
		nextIter = context.nextIter;
		nbFound++;
	}

	if (nbFound == 0) {
		return false;
	}

	if (nbFound >= 2) {
		context.res = prevRes;
		context.nextIter = prevNextIter;
		return true;
	}

	auto nextFunc = res.clockWise ? std::prev <timesortedtype::iterator> : std::next< timesortedtype::iterator>;

	auto item = context.timeSorted.find(res.topBaseLine);
	auto prev = item;
	auto curr = nextFunc(item, 1);
	while (curr != context.timeSorted.end() && (curr->point.y() <= prev->point.y())) {
		prev = curr;
		curr = nextFunc(curr, 1);
	}
	if (curr == context.timeSorted.end()) return false;
	PathExtrema extremum = *prev;

	auto tentative = 14;
	curr = nextFunc(curr, 1);
	while (curr != context.timeSorted.end() && !(!curr->isSharp && curr->isY) && tentative != 0) {
		curr = nextFunc(curr, 1);
		tentative--;
	}

	double x;

	if (curr != context.timeSorted.end() && tentative != 0) {
		x = curr->point.x();
	}
	else {
		x = extremum.point.x() + 1.2;
	}

	auto bbox = context.transformedPath.boundingRect();

	QLineF line{ {x ,bbox.top()}, {x ,bbox.bottom()} };

	auto intersections = FindInterections(context.transformedPath, line);

	if (intersections.size() < 2) {
		return false;
	}

	std::sort(intersections.begin(), intersections.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() < b.point.y(); });

	context.res.bottomBaseLine = intersections[1];
	context.res.topBaseLine = intersections[0];

	context.res.length = 0;

	return true;

}

bool static searchLamAlefLiga(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.bottomCheck = false;


	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		auto& res = context.res;

		auto xDiff = context.res.bottomBaseLine.point.x() - context.res.topBaseLine.point.x();
		auto yDiff = context.res.bottomBaseLine.point.y() - context.res.topBaseLine.point.y();

		if (res.bottomLeft.point.x() > res.bottomBaseLine.point.x()
			|| res.bottomRight.point.x() > res.bottomBaseLine.point.x() // diff
			|| res.bottomLeft.point.y() > res.bottomBaseLine.point.y()
			|| res.bottomRight.point.y() < res.bottomBaseLine.point.y()// diff
			) {
			return false;
		}

		auto bbox = context.transformedPath.boundingRect();

		auto center = context.res.topBaseLine.point;

		QLineF line{ {center.x() ,bbox.top()}, {center.x() ,bbox.bottom()} };

		auto intersections = FindInterections(context.transformedPath, line);

		if (intersections.size() < 4) {
			return false;
		}



		return true;
	};

	return getJoin(context, joinCheck, 4, 1, 0.2, 0.5, 0.3, 0.8);


}

static void copySegment(const QPainterPath& source, QPainterPath& dest, int startIndex, int endIndex) {
	for (int i = startIndex; i <= endIndex; ++i) {
		const QPainterPath::Element& e = source.elementAt(i);
		switch (e.type) {
		case QPainterPath::MoveToElement:
			dest.moveTo(e);
			break;
		case QPainterPath::LineToElement:
		{
			dest.lineTo(e);
			break;
		}
		case QPainterPath::CurveToElement: {
			dest.cubicTo(e, source.elementAt(i + 1), source.elementAt(i + 2));
			i += 2;
			break;
		}
		default:
			break;
		}

	}
}
/*FROM qpathclipper.cpp */
static QVector<QPainterPath> toSubpaths(const QPainterPath& path)
{
	QVector<QPainterPath> subpaths;
	if (path.isEmpty())
		return subpaths;
	QPainterPath current;
	for (int i = 0; i < path.elementCount(); ++i) {
		const QPainterPath::Element& e = path.elementAt(i);
		switch (e.type) {
		case QPainterPath::MoveToElement:
			if (current.elementCount() > 1)
				subpaths += current;
			current = QPainterPath();
			current.moveTo(e);
			break;
		case QPainterPath::LineToElement:
			current.lineTo(e);
			break;
		case QPainterPath::CurveToElement: {
			current.cubicTo(e, path.elementAt(i + 1), path.elementAt(i + 2));
			i += 2;
			break;
		}
		case QPainterPath::CurveToDataElement:
			Q_ASSERT(!"toSubpaths(), bad element type");
			break;
		}
	}
	if (current.elementCount() > 1)
		subpaths << current;
	return subpaths;
}
bool PageAnalysisResult::cutSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font, bool refresh) {

	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];
	auto& subWord = word.wordResultInfo.subWords[subWordIndex];

	if (subWord.baseGlyphs.size() > 0 && !refresh) {
		return subWord.correct;
	}

	subWord.baseGlyphs.clear();

	auto& shape = word.paths[subWord.paths[0]];

	QPainterPath& path = shape.path;

	if (subWord.joins.size() == 0) {
		subWord.baseGlyphs.push_back({ subWord.text ,path });
		return true;
	}

	auto subPaths = toSubpaths(path);

	subPaths.remove(0);

	auto currentJoin = subWord.joins.begin();

	for (int joinIndex = 0; joinIndex <= subWord.joins.size(); joinIndex++) {

		JoinTime leftJointTime;
		JoinTime rightJointTime;
		bool leftJoinExists = false;
		bool rightJoinExists = false;
		leftJointTime.startAtLeft = true;

		Stretching prevJoin;


		if (currentJoin != subWord.joins.end()) {
			leftJointTime = getJoinTime(*currentJoin, subWord.basePathOrientation);
			leftJoinExists = true;
		}

		if (joinIndex != 0) {
			prevJoin = *std::prev(currentJoin);
			rightJointTime = getJoinTime(prevJoin, subWord.basePathOrientation);
			rightJoinExists = true;
		}

		QPainterPath constructingPath;

		bool isOutside = (leftJoinExists && leftJointTime.startAtLeft) || (rightJoinExists && !rightJointTime.startAtLeft);


		JoinTime minJoinTime;
		JoinTime nextMinJoinTime;


		std::vector<double> nextPoints;

		if (!leftJoinExists) {
			minJoinTime = rightJointTime;
			nextPoints.push_back(minJoinTime.minTime);
			nextPoints.push_back(minJoinTime.maxTime);
		}
		else if (!rightJoinExists) {
			minJoinTime = leftJointTime;
			nextPoints.push_back(minJoinTime.minTime);
			nextPoints.push_back(minJoinTime.maxTime);
		}
		else {
			if (leftJointTime.minTime < rightJointTime.minTime) {
				minJoinTime = leftJointTime;
				nextMinJoinTime = rightJointTime;

			}
			else {
				minJoinTime = rightJointTime;
				nextMinJoinTime = leftJointTime;
			}
			if (isOutside) {
				nextPoints.push_back(minJoinTime.minTime);
				nextPoints.push_back(nextMinJoinTime.minTime);
				nextPoints.push_back(nextMinJoinTime.maxTime);
				nextPoints.push_back(minJoinTime.maxTime);
			}
			else {
				nextPoints.push_back(minJoinTime.minTime);
				nextPoints.push_back(minJoinTime.maxTime);
				nextPoints.push_back(nextMinJoinTime.minTime);
				nextPoints.push_back(nextMinJoinTime.maxTime);
			}
		}
		bool correct = true;
		for (int i = 1; i < nextPoints.size(); i++) {
			//assert(nextPoints[i - 1] < nextPoints[i]);
			if (!(nextPoints[i - 1] < nextPoints[i])) {
				correct = false;
				break;
			}
		}

		qreal curveIndex = -1;
		int pathIndex = -1;

		int currentJoinTimeIndex = 0;

		if (!correct) {
			std::cout << "Error in joins, text=" << subWord.text.toStdString() << std::endl;
			goto Continue;
		}


		nextPoints.push_back(path.elementCount());




		for (int i = 0; i < path.elementCount() && pathIndex < 1; ++i) {

			QBezier b;

			int currentIndex = i;

			const QPainterPath::Element& e = path.elementAt(i);
			switch (e.type) {
			case QPainterPath::MoveToElement:
				pathIndex++;
				curveIndex = -1;
				continue;
			case QPainterPath::LineToElement:
			{
				curveIndex++;
				break;
			}
			case QPainterPath::CurveToElement:
			{
				curveIndex++;

				b = QBezier::fromPoints(path.elementAt(i - 1),
					e,
					path.elementAt(i + 1),
					path.elementAt(i + 2));

				i += 2;
			}
			break;
			default:
				break;
			}

			auto currentJoinTime = nextPoints[currentJoinTimeIndex];
			auto prevJoinTime = currentJoinTimeIndex > 0 ? nextPoints[currentJoinTimeIndex - 1] : -1;

			if (isOutside) {
				if ((curveIndex + 1) <= currentJoinTime) continue;// || (currentJoinTimeIndex > 0 && curveIndex >= prevJoinTime)
				else {
					qreal tstart = currentJoinTime - curveIndex;
					qreal tend = 1;
					assert(tstart >= 0 && tstart < 1);
					isOutside = false;
					if (currentJoinTimeIndex + 1 < nextPoints.size()) {
						auto nextTime = nextPoints[currentJoinTimeIndex + 1];
						if (curveIndex + 1 > nextTime) {
							tend = nextTime - curveIndex;
							isOutside = true;
							currentJoinTimeIndex++;
						}
					}
					if (e.type == QPainterPath::CurveToElement) {
						auto b1 = b.getSubRange(tstart, tend);

						if (constructingPath.isEmpty()) {
							constructingPath.moveTo(b1.pt1());
						}
						else {
							constructingPath.lineTo(b1.pt1());
						}

						constructingPath.cubicTo(b1.pt2(), b1.pt3(), b1.pt4());
					}
					else {
						QLineF line{ path.elementAt(currentIndex - 1),e };
						if (constructingPath.isEmpty()) {
							constructingPath.moveTo(line.pointAt(tstart));
						}
						else {
							constructingPath.lineTo(line.pointAt(tstart));
						}
						constructingPath.lineTo(line.pointAt(tend));
					}

					currentJoinTimeIndex++;
				}
			}
			else {
				if (constructingPath.isEmpty()) {
					constructingPath.moveTo(path.elementAt(currentIndex - 1));
				}
				if ((curveIndex + 1) <= currentJoinTime) {

					if (e.type == QPainterPath::CurveToElement) {

						constructingPath.cubicTo(b.pt2(), b.pt3(), b.pt4());
					}
					else {
						constructingPath.lineTo(e);
					}
				}
				else {
					qreal tend = currentJoinTime - curveIndex;
					qreal tstart = 0.0;
					assert(tend >= 0 && tend < 1);
					isOutside = true;
					if (currentJoinTimeIndex + 1 < nextPoints.size()) {
						auto nextTime = nextPoints[currentJoinTimeIndex + 1];
						if (curveIndex + 1 > nextTime) {
							tstart = nextTime - curveIndex;
							isOutside = false;
							currentJoinTimeIndex++;
						}
					}
					if (e.type == QPainterPath::CurveToElement) {
						auto b1 = b.getSubRange(0, tend);

						constructingPath.cubicTo(b1.pt2(), b1.pt3(), b1.pt4());

						if (tstart != 0.0) {
							auto b2 = b.getSubRange(tstart, 1.0);

							constructingPath.lineTo(b2.pt1());

							constructingPath.cubicTo(b2.pt2(), b2.pt3(), b2.pt4());
						}
					}
					else {
						QLineF line{ path.elementAt(currentIndex - 1),e };
						constructingPath.lineTo(line.pointAt(tend));

						if (tstart != 0.0) {

							constructingPath.lineTo(line.pointAt(tstart));

							constructingPath.lineTo(e);
						}
					}

					currentJoinTimeIndex++;
				}


			}

		}

		for (auto& subpath : subPaths) {
			if (constructingPath.contains(subpath)) {
				constructingPath.addPath(subpath);
			}
		}

		subWord.baseGlyphs.push_back({ subWord.text ,constructingPath });
	Continue:
		if (currentJoin != subWord.joins.end())
			currentJoin++;
	}



	//subWord.stretchings


}





bool PageAnalysisResult::segmentSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font, bool refresh) {
	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];
	auto& subWord = word.wordResultInfo.subWords[subWordIndex];

	if (subWord.baseGlyphs.size() > 0 && !refresh) {
		return subWord.correct;
	}

	subWord.baseGlyphs.clear();

	QString baseText;
	QString markText;

	for (auto& qchar : subWord.text) {
		if (qchar.isMark()) {
			markText += qchar;
		}
		else {
			baseText += qchar;
		}
	}

	auto& shape = word.paths[subWord.paths[0]];

	if (baseText.size() < 2) {
		subWord.correct = true;
		GlyphInfo glyphInfo{ baseText ,shape.path };
		subWord.baseGlyphs.push_back(glyphInfo);
		return true;
	}

	subWord.joins.clear();
	subWord.extrema.clear();



	SearchContext context{ .subword = subWord,.baseText = baseText };

	auto& subPaths = context.subPaths;

	auto pos = shape.pos;
	context.transformedPath = shape.path * shape.transform;
	context.transformedPath.translate(pos.x(), pos.y());
	context.transformedPath.setFillRule(Qt::WindingFill);

	context.bbox = context.transformedPath.boundingRect();

	ShapeItem::getExtrema(context.transformedPath, subPaths, subWord.extrema);

	int basePathIndex = 0;

	//remove close points
	auto oldExtrema = subWord.extrema;
	subWord.extrema.clear();
	for (auto old = oldExtrema.begin(); old != oldExtrema.end(); old++) {
		bool add = true;
		for (auto it = subWord.extrema.begin(); it != subWord.extrema.end(); it++) {
			if (it->pathIndex == old->pathIndex && std::abs(it->point.x() - old->point.x()) < 0.01 && std::abs(it->point.y() - old->point.y()) < 0.01) {
				add = false;
				break;
			}
		}
		if (add) {
			subWord.extrema.append(*old);
			if (old->pathIndex == basePathIndex) {
				context.timeSorted.insert(*old);
				context.xSorted.insert(*old);
				context.ySorted.insert(*old);
			}
		}
	}

	context.setOrientation();
	subWord.basePathOrientation = context.orientation;

	context.nextIter = context.xSorted.size() != 0 ? std::next(context.xSorted.begin()) : context.xSorted.end();

	bool cont = true;

	bool init = true;
	auto currentChar = baseText[0];

	int lastIndex = baseText.length() - 1;
	bool match = true;

	JoinCheck joinCheck;

	auto isBehShape = [](QChar character) {
		return character == QChar(u'ب') || character == QChar(u'ت')
			|| character == QChar(u'ث') || character == QChar(u'ن') || character == QChar(u'ي');
	};


	for (int charIndex = 0; charIndex < lastIndex && match; charIndex++) {
		bool firstJoin = charIndex == 0;
		bool lastJoin = charIndex == lastIndex - 1;
		auto currentChar = baseText[charIndex];
		auto nextIndex = charIndex + 1;
		auto nextChar = baseText[nextIndex];

		match = false;

		auto tatweel = baseText[charIndex] == QChar(u'ـ');

		context.res = {};
		if (baseText[nextIndex] == QChar(u'ر') || baseText[nextIndex] == QChar(u'ز')) {
			match = searchNextReh(context);
		}
		else if (lastJoin && baseText[nextIndex] == QChar(u'ق')) {
			match = searchQafFina(context);
		}
		else if (lastJoin && (baseText[nextIndex] == QChar(u'ي') || baseText[nextIndex] == QChar(u'ئ') || baseText[nextIndex] == QChar(u'ى'))) {
			match = searchYehFina(context);
		}
		else if (lastJoin && baseText[nextIndex] == QChar(u'ن')) {
			match = searchNoonFina(context);
		}
		else if (firstJoin && (baseText[charIndex] == QChar(u'ع') || baseText[charIndex] == QChar(u'غ'))) {
			match = searchAinInit(context);
		}
		else if (firstJoin && (baseText[charIndex] == QChar(u'ل'))) {
			match = searchLamInit(context);
		}
		else if (baseText.size() == 2 && baseText[charIndex] == QChar(u'ل') && (baseText[nextIndex] == QChar(u'ا')
			|| baseText[nextIndex] == QChar(u'آ') || baseText[nextIndex] == QChar(u'أ') || baseText[nextIndex] == QChar(u'إ'))) {
			match = searchLamAlefLiga(context);
		}
		else if (firstJoin && isBehShape(baseText[charIndex])
			&& (baseText[nextIndex] == QChar(u'ج') || baseText[nextIndex] == QChar(u'ح') || baseText[nextIndex] == QChar(u'خ'))) {
			match = searchBehInitFollowedByHah(context);
		}
		else if (baseText[nextIndex] == QChar(u'و') || baseText[nextIndex] == QChar(u'ؤ')) {
			match = searchWawFina(context);
		}
		else if (baseText[nextIndex] == QChar(u'ص') || baseText[nextIndex] == QChar(u'ض')) {
			match = searchNextSadOrTah(context);
		}
		else if (baseText[nextIndex] == QChar(u'ط') || baseText[nextIndex] == QChar(u'ظ')) {
			match = searchNextSadOrTah(context);
		}
		if (!match) {
			if (baseText[charIndex] == QChar(u'ط') || baseText[charIndex] == QChar(u'ظ')) {
				match = getJoin(context, joinCheck, 4, 1, minXBaseDiff, 2); // getJoin(context, joinCheck);
			}
			else if (baseText[charIndex] == QChar(u'س') || baseText[charIndex] == QChar(u'ش')) {
				match = searchSeen(context);
			}
			else if (nextIndex < lastIndex && baseText[nextIndex] == QChar(u'ه')) { // Heh medi
				match = searchNextHehMedi(context);
				if (!match) {
					match = getJoin(context, joinCheck);
				}
			}
			else if (baseText[charIndex] == QChar(u'ج') || baseText[charIndex] == QChar(u'ح') || baseText[charIndex] == QChar(u'خ')) {
				bool behhahinit = charIndex == 1 && isBehShape(baseText[0]);
				match = searchHah(context, behhahinit);
			}
			else if (baseText[charIndex] == QChar(u'ك')) {
				match = searchKaf(context);
			}
			else {
				match = getJoin(context, joinCheck); // getJoin(context, joinCheck);
			}
		}


		if (match) {
			subWord.joins.insert(context.res);
		}
	}

	cutSubword(pageIndex, lineIndex, wordIndex, subWordIndex, font, true);

	subWord.correct = subWord.joins.size() == lastIndex;

	return subWord.correct;
}
