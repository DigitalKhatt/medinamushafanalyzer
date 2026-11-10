#include "pageanalysisresult.h"
#include "letterforminspection.h"
#include "CustomOutputDev.h"
#include "PDFDoc.h"
#include "GlobalParams.h"
#include "afont.h"
#include "quran.h"
#include <format>
#include "qfile.h"
#include <private/qbezier_p.h>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>

namespace {
thread_local LetterFormInspection* formInspection = nullptr;
thread_local bool inspectingForm = false;
thread_local bool inspectionCoordinates = true;

struct InspectFormScope {
    bool previous = inspectingForm;
    bool previousCoordinates = inspectionCoordinates;
    explicit InspectFormScope(const QString& id) {
        inspectingForm = formInspection && id == formInspection->formId;
        if (inspectingForm) formInspection->attempted = true;
    }
    explicit InspectFormScope(const QJsonObject& form) : InspectFormScope(form.value("id").toString()) {}
    InspectFormScope(const QJsonObject& form, const QPainterPath& ink) : InspectFormScope(form) {
        if (inspectingForm) {
            inspectionCoordinates = ink == formInspection->ink;
            if (!inspectionCoordinates)
                ++formInspection->reasons["Preview: Alternative recovery outline; its landmarks/proposals are omitted. Assembled cuts use the original page coordinates."];
        }
    }
    ~InspectFormScope() { inspectingForm = previous; inspectionCoordinates = previousCoordinates; }
};

static void inspectReason(const char* stage, const QString& reason) {
    if (inspectingForm) ++formInspection->reasons[QString::fromLatin1(stage) + ": " + reason];
}
static void inspectReason(const char* stage, const char* reason) {
    if (inspectingForm) inspectReason(stage, QString::fromUtf8(reason));
}

static void inspectLandmark(const char* name, const PathExtrema& point) {
    if (!inspectingForm || !inspectionCoordinates) return;
    const auto label = QString::fromUtf8(name) + QString(" · %1").arg(point.time, 0, 'f', 2);
    for (const auto& item : formInspection->landmarks)
        if (item.label == label && item.point == point.point) return;
    formInspection->landmarks.append({label, point.point});
}

static void inspectCandidate(const QString& label, const Stretching& left,
                             const std::optional<Stretching>& right = {}) {
    if (!inspectingForm || !inspectionCoordinates) return;
    QVector<QLineF> lines;
    if (right) for (const auto& line : right->cutSegments()) lines.append(line);
    for (const auto& line : left.cutSegments()) lines.append(line);
    for (const auto& item : formInspection->candidates)
        if (item.cuts == lines) return;
    formInspection->candidates.append({label, lines});
}
} // namespace

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

struct StrokeNormalSample {
    std::optional<std::pair<PathExtrema, PathExtrema>> endpoints;
    double width = 0;
};

// Intrinsic body evidence. It contains no proposed boundaries and does not
// depend on the width or landmarks of either adjoining letter.
struct LamForkBody {
    PathExtrema tip;
    QPointF forwardFlank, backwardFlank;
    bool rightForward;
    bool fork;
    bool positiveTurn;

    bool isConcave(Orientation orientation) const {
        return positiveTurn == (orientation == Orientation::CounterClockwise);
    }

    bool folded() const {
        const auto steep = forwardFlank.x() < backwardFlank.x() ? forwardFlank : backwardFlank;
        const auto foot = forwardFlank.x() < backwardFlank.x() ? backwardFlank : forwardFlank;
        // A folded fork has a nearly upright flank and a foot turning farther
        // right than that flank. These slopes belong to the uncut lam, so a
        // thick adjoining letter or an absent connector cannot hide the fork.
        return foot.x() > 0 && std::abs(steep.x()) <= .25 * steep.y() &&
            foot.x() * steep.y() > std::abs(steep.x()) * foot.y();
    }
};

struct LamBodyEvidence {
    std::vector<PathExtrema> crests;
    std::vector<LamForkBody> forks;
};

struct LamMedialBodyEvidence;
struct SharedShapeEvidence;

// Shared by retries on a single, fixed outline in page coordinates.
struct OutlineSearchCache {
    std::map<std::pair<double, double>, StrokeNormalSample> normalsByCenter;
    std::map<std::pair<QString, bool>, std::optional<Stretching>> terminalCuts;
    std::map<std::pair<QString, double>, std::vector<std::pair<Stretching, Stretching>>> contourBodies;
    // Interior points on the two lips found in an adjoining connector.
    std::map<int, QLineF> misplacedHeads;
    std::map<std::pair<QByteArray, int>, std::shared_ptr<const LamMedialBodyEvidence>> medialLamBodies;
    std::map<std::pair<QByteArray, int>, std::shared_ptr<const SharedShapeEvidence>> sharedBodies;
};

struct SearchContext {
	QJsonObject form;
	QJsonObject joinDefaults;
	double pairedStrokeWidth = 0;
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
    OutlineSearchCache* outlineCache = nullptr;

	timesortedtype::iterator prev(timesortedtype::iterator time) const {
		if(time == timeSorted.begin()){			
			return --timeSorted.end();
		}else{			
			return std::prev(time);
		}
		/*auto prev = std::prev(time);
		if (prev == timeSorted.end()) {
			prev = --timeSorted.end();
		}
		return prev;*/
	}

	timesortedtype::iterator next(timesortedtype::iterator time) const {
		auto next = std::next(time);
		if (next == timeSorted.end()) {
			next = timeSorted.begin();
		}
		return next;
	}

	void setOrientation() {

		auto minY = ySorted.begin();
		auto C = timeSorted.find(*minY);


		auto B = prev(C);
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

// Recognition receives only the original contour and its landmark inventory.
// Neighboring joins, transcript slots, cut policies and assembly cursors are
// deliberately absent from this interface.
struct InkOutline {
    const QPainterPath& ink;
    const xsortedtype& x;
    const ysortedtype& y;
    const timesortedtype& time;
    const std::vector<QPainterPath>& contours;
    QRectF bounds;
    Orientation orientation;

    SearchContext geometry(WordResultInfo::SubWordInfo& empty) const {
        return {.transformedPath = ink, .xSorted = x, .ySorted = y, .timeSorted = time,
                .bbox = bounds, .subword = empty, .subPaths = contours, .orientation = orientation};
    }
};

static InkOutline inkOutline(const SearchContext& context) {
    return {context.transformedPath, context.xSorted, context.ySorted, context.timeSorted,
            context.subPaths, context.bbox, context.orientation};
}

struct JoinTime {
	double minTime = -1;
	double maxTime = 100000;
	bool startAtLeft = false;

	bool inline isLeft(double value) const {
		return (startAtLeft && (value <= minTime || value >= maxTime)) || (!startAtLeft && value >= minTime && value <= maxTime);
	}

	bool isRight(double value) const {
		return (!startAtLeft && (value <= minTime || value >= maxTime)) || (startAtLeft && value >= minTime && value <= maxTime);
	}

	bool inline isStrictLeft(double value) const {
		return (startAtLeft && (value < minTime || value > maxTime)) || (!startAtLeft && value > minTime && value < maxTime);
	}

	bool isStrictRight(double value) const {
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

static bool sharesLowerForkTip(const Stretching& right, const Stretching& left) {
    const auto& tip = right.bottomBaseLine;
    return right.counterEntry.pathIndex <= 0 && left.counterEntry.pathIndex <= 0 &&
        tip.pathIndex == 0 && left.bottomBaseLine.pathIndex == 0 &&
        tip.time == left.bottomBaseLine.time && tip.point == left.bottomBaseLine.point &&
        right.topBaseLine.point.x() > tip.point.x() && left.topBaseLine.point.x() < tip.point.x() &&
        right.topBaseLine.point.y() < tip.point.y() && left.topBaseLine.point.y() < tip.point.y();
}

static bool cutPrecedes(const Stretching& right, const Stretching& left, Orientation orientation) {
    const auto side = getJoinTime(right, orientation);
    return side.isStrictLeft(left.topBaseLine.time) &&
        (side.isStrictLeft(left.bottomBaseLine.time) || sharesLowerForkTip(right, left));
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
	// A cubic Bezier can reduce to a quadratic or line in the probe's
	// direction. Cardano's formula divides by a and loses those intersections.
	const double scale = std::max({std::abs(a), std::abs(b), std::abs(c), std::abs(d)});
	if (scale == 0) return roots; // Coincident curves have no isolated root.
	const double tolerance = 64 * std::numeric_limits<double>::epsilon() * scale;
	if (std::abs(a) <= tolerance) {
		const auto add = [&](double t) {
			if (!std::isfinite(t) || t < -1e-12 || t > 1 + 1e-12) return;
			t = std::clamp(t, 0.0, 1.0);
			if (std::none_of(roots.begin(), roots.end(), [&](double other) { return std::abs(t - other) < 1e-12; }))
				roots.push_back(t);
		};
		if (std::abs(b) <= tolerance) {
			if (std::abs(c) > tolerance) add(-d / c);
			return roots;
		}
		const double qa = b / scale, qb = c / scale, qc = d / scale;
		double discriminant = std::fma(-4 * qa, qc, qb * qb);
		const double error = 64 * std::numeric_limits<double>::epsilon() *
			(std::abs(qb * qb) + std::abs(4 * qa * qc));
		if (discriminant < -error) return roots;
		discriminant = std::max(0.0, discriminant);
		const double q = -0.5 * (qb + std::copysign(std::sqrt(discriminant), qb));
		if (q == 0) add(-qb / (2 * qa));
		else { add(q / qa); add(qc / q); }
		std::sort(roots.begin(), roots.end());
		return roots;
	}
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

struct ProbeBounds {
    double left, right, top, bottom;
};

static void FindInterections(const QBezier& b, const QLineF& line, int pathIndex, int curveIndex,
                             std::vector<PathExtrema>& intersections, const ProbeBounds* probeBounds) {

	auto C0 = b.pt1();
	auto C1 = b.pt2();
	auto C2 = b.pt3();
	auto C3 = b.pt4();

    // Every point on a Bézier is inside the bounds of its four control points.
    // Include roundoff in the polynomial evaluation as well as the probe's
    // existing endpoint tolerance. An unbounded line still visits every curve.
    if (probeBounds) {
        const auto outside = [](double a, double b, double c, double d, double low, double high) {
            const auto [lo, hi] = std::minmax({a, b, c, d});
            const double roundoff = 64 * std::numeric_limits<double>::epsilon() * std::max(std::abs(lo), std::abs(hi));
            return hi + roundoff < low || lo - roundoff > high;
        };
        if (outside(C0.x(), C1.x(), C2.x(), C3.x(), probeBounds->left, probeBounds->right) ||
            outside(C0.y(), C1.y(), C2.y(), C3.y(), probeBounds->top, probeBounds->bottom)) return;
    }

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

		if (probeBounds && (point.x() < probeBounds->left || point.x() > probeBounds->right ||
                            point.y() < probeBounds->top || point.y() > probeBounds->bottom)) continue;

		PathExtrema intersect;

		intersect.pathIndex = pathIndex;
		intersect.time = curveIndex + t;
		intersect.point = point;
		intersections.push_back(intersect);

	}
}
static std::vector<PathExtrema> FindInterections(const QPainterPath& path, const QLineF& line, bool segment = false) {

	std::vector<PathExtrema> res;
    constexpr double tolerance = 1e-9;
    const ProbeBounds bounds{std::min(line.x1(), line.x2()) - tolerance, std::max(line.x1(), line.x2()) + tolerance,
                             std::min(line.y1(), line.y2()) - tolerance, std::max(line.y1(), line.y2()) + tolerance};
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
			FindInterections(b, line, pathIndex, curveIndex, res, segment ? &bounds : nullptr);
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



static QJsonObject readFormConfig(const QString& filename) {
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const auto config = QJsonDocument::fromJson(file.readAll(), &error).object();
    if (error.error != QJsonParseError::NoError || config.value("version").toInt() != 2 ||
        !config.value("initial").isArray() || !config.value("medial").isArray() || !config.value("final").isArray()) return {};
    return config;
}

static QJsonObject medialLamShortExit(const QJsonObject& config);

static QJsonObject withOverrides(QJsonObject defaults, const QJsonObject& overrides) {
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        defaults.insert(it.key(), it.value());
    }
    return defaults;
}

static QString invalidKashidaSettings(const QJsonValue& value, const QJsonObject& defaults,
                                      const QString& location) {
    if (value.isUndefined()) return {};
    if (!value.isObject()) return location + " must be an object";
    const auto settings = value.toObject();
    for (auto setting = settings.begin(); setting != settings.end(); ++setting) {
        const auto key = setting.key();
        const auto expected = defaults.value(key);
        if (expected.isUndefined()) return location + ": unknown setting " + key;
        if (setting.value().type() != expected.type()) return location + ": wrong type for " + key;
        if (!expected.isDouble()) continue;

        const double number = setting.value().toDouble();
        const bool sampleCount = key == "seedSteps" || key == "maxTrackSteps";
        const bool fraction = key == "steepAnchorRatio" || key == "neighborSearchRatio" || key == "sharedCutTolerance";
        if (!std::isfinite(number) || number < 0 ||
            (sampleCount && (number != std::floor(number) || number > 1000)) ||
            (fraction && number > 1) || (key == "samplesPerAnchorWidth" && number <= 0)) {
            return location + ": out-of-range value for " + key;
        }
    }
    const auto effective = withOverrides(defaults, settings);
    for (const auto& range : {std::pair{"minSeedWidthRatio", "maxSeedWidthRatio"},
                              std::pair{"minTrackWidthRatio", "maxTrackWidthRatio"}}) {
        if (effective.value(range.first).toDouble() > effective.value(range.second).toDouble())
            return location + ": minimum exceeds maximum for " + QString::fromLatin1(range.first);
    }
    return {};
}

static QString invalidWalkSettings(const QJsonObject& route, const QString& location) {
    static const QStringList probes{"down", "up", "left", "right", "below", "normal", "corner", "walk-stroke-up", "down-stroke", "walk-stroke-normal"};
    if (route.contains("bottomProbe") && !probes.contains(route.value("bottomProbe").toString()))
        return location + ": unsupported contour-walk bottomProbe";
    if (route.contains("penWidth")) {
        const auto width = route.value("penWidth");
        const auto probe = route.value("bottomProbe").toString();
        if (!width.isDouble() || !std::isfinite(width.toDouble()) || width.toDouble() <= 0 ||
            route.contains("strokeSource") ||
            (probe != "walk-stroke-normal" && probe != "walk-stroke-up" && probe != "down-stroke"))
            return location + ": penWidth requires a stroke-walk probe and a positive width in page units";
    }
    for (const auto* field : {"visibleFrom", "recoveryVisibleFrom"}) {
        if (route.contains(field) && (route.value("bottom").toArray().isEmpty() || route.contains("bottomProbe") ||
            !QStringList{"top", "bottom"}.contains(route.value(field).toString())))
            return location + ": a visible chord needs two contour walks and a top or bottom anchor";
    }
    if (route.contains("topUseStepEnd") && !route.value("topUseStepEnd").isBool())
        return location + ": topUseStepEnd must be true or false";
    if (route.contains("topAtAnchor") && (!route.value("topAtAnchor").isBool() ||
        !route.value("top").toArray().isEmpty() || route.value("topUseStepEnd").toBool()))
        return location + ": topAtAnchor needs a boolean, without a top walk or step end";
    if (route.contains("probeBothSides") && (!route.value("probeBothSides").isBool() ||
        !route.value("topAtAnchor").toBool() || route.value("bottomProbe") != "normal"))
        return location + ": probeBothSides requires a normal directly at the anchor";
    if (route.contains("oppositeCorner")) {
        if (route.value("bottomProbe") != "corner" || !route.value("oppositeCorner").isObject())
            return location + ": oppositeCorner requires a corner probe and numeric offset bounds";
        const auto bounds = route.value("oppositeCorner").toObject();
        for (auto it = bounds.begin(); it != bounds.end(); ++it) {
            if (QStringList{"sharp", "xExtremum", "yExtremum"}.contains(it.key())) {
                if (!it.value().isBool()) return location + ": opposing landmark flags must be booleans";
                continue;
            }
            if (it.key() == "flanks") {
                if (!it.value().isObject()) return location + ": opposing corner flanks must contain numeric bounds";
                const auto flanks = it.value().toObject();
                for (auto flank = flanks.begin(); flank != flanks.end(); ++flank)
                    if (!QStringList{"minX", "maxX", "minY", "maxY"}.contains(flank.key()) ||
                        !flank.value().isDouble() || !std::isfinite(flank.value().toDouble()))
                        return location + ": opposing corner flanks need finite x/y bounds";
                for (const auto* suffix : {"X", "Y"})
                    if (flanks.contains(QString("min") + suffix) && flanks.contains(QString("max") + suffix) &&
                        flanks.value(QString("min") + suffix).toDouble() > flanks.value(QString("max") + suffix).toDouble())
                        return location + ": opposing corner flank minimum exceeds maximum";
                continue;
            }
            if (!QStringList{"minX", "maxX", "minY", "maxY", "minDistance", "maxDistance"}.contains(it.key()) ||
                !it.value().isDouble() || !std::isfinite(it.value().toDouble()) ||
                (it.key().endsWith("Distance") && it.value().toDouble() < 0))
                return location + ": oppositeCorner needs finite x/y/distance bounds";
        }
        for (const auto* suffix : {"X", "Y", "Distance"})
            if (bounds.contains(QString("min") + suffix) && bounds.contains(QString("max") + suffix) &&
                bounds.value(QString("min") + suffix).toDouble() > bounds.value(QString("max") + suffix).toDouble())
                return location + ": oppositeCorner minimum exceeds maximum";
    }
    for (const auto& options : {std::pair{"anchorSide", QStringList{"top", "bottom"}},
                                std::pair{"strokeSource", QStringList{"local", "paired"}}})
        if (route.contains(options.first) && !options.second.contains(route.value(options.first).toString()))
            return location + ": unsupported " + options.first;
    if (route.contains("walkNormalOffset") &&
        (!route.value("walkNormalOffset").isDouble() ||
         !std::isfinite(route.value("walkNormalOffset").toDouble()) || route.value("walkNormalOffset").toDouble() < 0))
        return location + ": walkNormalOffset must be a nonnegative distance in stroke widths";
    if (route.contains("forkSide") &&
        (!(route.value("forkSide") == "left" || route.value("forkSide") == "right") ||
         route.value("bottomProbe") != "down" ||
         route.value("walkNormalOffset").toDouble() <= 0))
        return location + ": forkSide requires left or right, a downward probe and a positive walkNormalOffset";
    for (const auto* side : {"top", "bottom"}) {
        for (const auto& value : route.value(side).toArray()) {
            const auto step = value.toObject();
            if (step.contains("immediate") && !step.value("immediate").isBool())
                return location + ": immediate must be a boolean on a contour step";
            for (const auto* endpoint : {"start", "end"}) {
                if (!step.contains(endpoint)) continue;
                if (!step.value(endpoint).isObject()) return location + ": step landmark must be an object";
                const auto features = step.value(endpoint).toObject();
                for (auto feature = features.begin(); feature != features.end(); ++feature)
                    if (!QStringList{"sharp", "xExtremum", "yExtremum", "xMinimum"}.contains(feature.key()) || !feature.value().isBool())
                        return location + ": landmark features must be sharp, xExtremum, yExtremum or xMinimum booleans";
            }
        }
    }
    return {};
}

static QJsonObject namedLetterForm(const QJsonObject& config, const QString& id) {
    for (const auto* phase : {"initial", "medial", "final"})
        for (const auto& entry : config.value(phase).toArray())
            if (entry.toObject().value("id") == id) return entry.toObject();
    return {};
}

static bool toothPairDescription(const QJsonObject& body) {
    return body.value("shape") == "tooth-pair";
}

static bool bentToothDescription(const QJsonObject& body) {
    const auto shape = body.value("shape");
    return shape == "tooth-bend" || shape == "tooth-shoulder" || shape == "tooth-hook";
}

static bool toothDescription(const QJsonObject& body) {
    return toothPairDescription(body) || bentToothDescription(body);
}

// Medial teeth use declared recognition and placement throughout assembly.
static bool hasToothBody(const QJsonObject& form) {
    return toothDescription(form.value("body").toObject());
}

static QJsonObject toothRecognitionSettings(const QJsonObject& form) {
    return hasToothBody(form) ? form.value("body").toObject() : QJsonObject{};
}

static QJsonObject toothPlacementSettings(const QJsonObject& form) {
    return hasToothBody(form) ? form.value("cuts").toObject() : QJsonObject{};
}

// References share descriptions, not execution order. Resolve each field on
// demand so editing a parent in the preview immediately updates its users.
static QJsonObject formDescription(const QJsonObject& config, const QJsonObject& form,
        const QString& field, QString* error = nullptr, QSet<QString> parents = {}) {
    const auto id = form.value("id").toString();
    const auto value = form.value(field);
    if (value.isUndefined()) return {};
    if (!value.isObject() || parents.contains(id)) {
        if (error) *error = id + ": " + field + " must be an object without cyclic inheritance";
        return {};
    }
    auto result = value.toObject();
    const auto inherited = result.take("inherit");
    if (inherited.isUndefined()) return result;
    const auto parent = namedLetterForm(config, inherited.toString());
    if (!inherited.isString() || parent.isEmpty() || !parent.contains(field)) {
        if (error) *error = id + ": " + field + ".inherit must reference an existing description";
        return {};
    }
    parents.insert(id);
    return withOverrides(formDescription(config, parent, field, error, parents), result);
}

static QJsonObject resolvedBodyForm(const QJsonObject& config, QJsonObject form) {
    if (form.contains("body")) {
        form.insert("body", formDescription(config, form, "body"));
        form.insert("cuts", formDescription(config, form, "cuts"));
    }
    return form;
}

QJsonObject resolvedLetterForm(const QJsonObject& config, const QString& id) {
    return resolvedBodyForm(config, namedLetterForm(config, id));
}

// Shared shape declarations keep recognition measurements separate from cut
// projection. The field readers also serve forms not migrated yet; they do not
// reconstruct a legacy method or route through the old method dispatcher.
static bool lamDescription(const QJsonObject& body) {
    static const QStringList shapes{"contour-stem", "descending-stem", "forked-stem", "turning-shaft",
        "terminal-bowl", "terminal-fork", "terminal-return", "attached-return", "detached-return"};
    return shapes.contains(body.value("shape").toString());
}

static bool sharedShapeDescription(const QJsonObject& body) {
    static const QStringList shapes{"counter-neck", "counter-shoulder", "counter-bridge", "counter-loop", "counter-rim",
        "counter-outline", "contour-route", "suspended-loop", "crested-loop", "bowl-turn"};
    return shapes.contains(body.value("shape").toString());
}

static QJsonObject mergeShapeSettings(QJsonObject body, const QJsonObject& cuts) {
    for (auto it = cuts.begin(); it != cuts.end(); ++it)
        body.insert(it.key(), body.value(it.key()).isObject() && it.value().isObject() ?
            QJsonValue(mergeShapeSettings(body.value(it.key()).toObject(), it.value().toObject())) : it.value());
    return body;
}

static QJsonObject shapeSettings(const QJsonObject& form, const QString& key) {
    return mergeShapeSettings(form.value("body").toObject().value(key).toObject(),
                         form.value("cuts").toObject().value(key).toObject());
}

static QJsonObject loopSettings(const QJsonObject& form) {
    const auto shape = form.value("body").toObject().value("shape");
    if (shape == "counter-loop") return shapeSettings(form, "loop");
    if (shape == "counter-rim") return {{"rimAnchors", shapeSettings(form, "rim")}};
    return form.value("loopBoundaries").toObject();
}

static QJsonObject loopHeadSettings(const QJsonObject& form) {
    return form.value("body").toObject().value("shape") == "crested-loop" ? shapeSettings(form, "head") :
        form.value("headEdges").toObject();
}

static bool hasCounterOutline(const QJsonObject& form) {
    return form.value("body").toObject().value("shape") == "counter-outline" || form.contains("bodyOutline");
}

static QJsonObject counterOutlineSettings(const QJsonObject& form) {
    if (form.value("body").toObject().value("shape") != "counter-outline") return form.value("bodyOutline").toObject();
    auto result = shapeSettings(form, "outline");
    result.insert("counters", form.value("body").toObject().value("counters"));
    return result;
}

// Common assembly constraints read the same resolved fields for both schemas.
// This view carries no method: declared shapes have their own recognizer and
// projection dispatcher below. Template JSON remains body/cuts only.
static QJsonObject sharedShapeProjectionForm(QJsonObject form) {
    const auto body = form.value("body").toObject();
    if (!sharedShapeDescription(body)) return form;
    const auto shape = body.value("shape");
    if (shape == "contour-route") form = withOverrides(form, shapeSettings(form, "route"));
    if (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge") {
        form.insert("counter", shapeSettings(form, "counter"));
        for (const auto* field : {"shoulder", "bridge"})
            if (body.contains(field) || form.value("cuts").toObject().contains(field))
                form.insert(field, shapeSettings(form, field));
    }
    if (shape == "counter-loop" || shape == "counter-rim") form.insert("loopBoundaries", loopSettings(form));
    if (shape == "counter-outline") form.insert("bodyOutline", counterOutlineSettings(form));
    if (shape == "suspended-loop") form.insert("head", shapeSettings(form, "head"));
    if (shape == "crested-loop") form.insert("headEdges", shapeSettings(form, "head"));
    if (shape == "bowl-turn") {
        form = withOverrides(form, form.value("cuts").toObject().value("probe").toObject());
        form.insert("start", body.value("origin"));
    }
    if (body.contains("features")) form.insert("bodyFeatures", body.value("features"));
    if (body.contains("counters")) form.insert("bodyCounters", body.value("counters"));
    return form;
}

static QString validateSharedShape(const QJsonObject& form, const QString& phase) {
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto shape = body.value("shape").toString(), id = form.value("id").toString();
    if (cuts.isEmpty()) return id + ": declared shapes require separate cuts";
    const auto boundaries = cuts.value("boundaries").toObject();
    if (boundaries.size() != 2 || !boundaries.contains("left") || !boundaries.contains("right") ||
        (phase == "initial" && boundaries.value("right") != "free") ||
        (phase == "final" && boundaries.value("left") != "free"))
        return id + ": cuts.boundaries must describe both sides for this position";
    const QString section = shape == "suspended-loop" || shape == "crested-loop" ? "head" : shape == "bowl-turn" ? "probe" :
        shape == "contour-route" ? "route" : (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge") ? "counter" :
        shape == "counter-loop" ? "loop" : shape == "counter-rim" ? "rim" : "outline";
    QStringList bodyKeys{"shape", "counters", "features", section}, cutKeys{"boundaries", section};
    if (shape == "bowl-turn") {
        bodyKeys.removeAll("probe"); bodyKeys << "origin";
        if (body.value("origin") != "left") return id + ": bowl-turn starts at the free left end";
    }
    if (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge") { bodyKeys << "shoulder"; cutKeys << "shoulder" << "bridge"; }
    for (auto it = body.begin(); it != body.end(); ++it)
        if (!bodyKeys.contains(it.key())) return id + ": unknown body setting " + it.key();
    for (auto it = cuts.begin(); it != cuts.end(); ++it)
        if (!cutKeys.contains(it.key())) return id + ": unknown cut-policy setting " + it.key();
    for (const auto* old : {"method", "counter", "loopBoundaries", "bodyOutline", "top", "bottom", "bottomProbe",
                          "bodyFeatures", "bodyCounters", "shoulder", "bridge", "head", "headEdges", "probeX", "probeRadius", "minCrossings"})
        if (form.contains(old)) return id + ": declared geometry belongs in body / cuts, not " + old;
    const QStringList projectionKeys{"normalNeck", "normalCut", "offset", "baselineOffset", "sideOffset", "entryBelowCounter",
        "required", "strokeOffsetX", "bottomProbe", "oppositeCorner", "normalOffset", "normalOffsetTolerance",
        "penWidth", "strokeSource", "walkNormalOffset", "anchorSide", "visibleFrom", "maxProjectionInset"};
    const auto checkBody = [&](auto&& self, const QJsonObject& value) -> bool {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (projectionKeys.contains(it.key())) return false;
            if (it.value().isObject() && !self(self, it.value().toObject())) return false;
        }
        return true;
    };
    if (!checkBody(checkBody, body)) return id + ": projection settings cannot change recognized body evidence";
    for (auto it = boundaries.begin(); it != boundaries.end(); ++it) {
        const auto mode = it.value().toString();
        const QStringList modes = shape == "suspended-loop" || shape == "crested-loop" ? QStringList{"free", "head-neck"} :
            shape == "bowl-turn" ? QStringList{"free", "turn-probe"} :
            (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge") ? QStringList{"free", "counter-neck", "counter-shoulder", "counter-bridge"} :
            shape == "contour-route" ? QStringList{"free", "contour-route"} :
            shape == "counter-outline" ? QStringList{"free", "outline-entry", "outline-exit"} :
                                        QStringList{"free", "loop-entry", "loop-exit"};
        if (!modes.contains(mode) || (phase == "medial" && mode == "free"))
            return id + ": unsupported boundary policy for " + shape;
    }
    if ((shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge" || shape == "contour-route") && phase == "medial")
        return id + ": this boundary shape needs an initial or final position";
    if (shape == "counter-rim" && phase != "medial") return id + ": counter-rim requires two boundaries";
    if ((shape == "suspended-loop" && phase != "initial") || (shape == "crested-loop" && phase != "medial") ||
        (shape == "bowl-turn" && phase != "final")) return id + ": shape does not support this letter position";
    return {};
}

// Reuse the established range/type checks during migration. This translation
// exists only in validation; execution never falls back to a legacy method.
static QJsonObject sharedShapeValidationForm(QJsonObject form, const QString& phase) {
    const auto shape = form.value("body").toObject().value("shape");
    form = sharedShapeProjectionForm(form);
    form.remove("body"); form.remove("cuts");
    if (shape == "contour-route") form.insert("method", "walk");
    else if (shape == "suspended-loop") form.insert("method", "initial-loop-head");
    else if (shape == "bowl-turn") form.insert("method", "qaf");
    else if (shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge") form.insert("method", phase == "initial" ? "initial-counter" : "final-counter");
    else if (shape == "counter-loop" && phase == "initial") form.insert("method", "initial-loop-body");
    return form;
}

// A terminal attachment is declared by the form. No template names or letter
// identities in the planner decide when a body changes its cut topology.
static QJsonObject descendingStemDescription(const QJsonObject& config, const QJsonObject& form) {
    return form.contains("body") ? formDescription(config, form, "body").value("stem").toObject()
                                    : form.value("descendingStem").toObject();
}

static QJsonObject medialLamShortExit(const QJsonObject& config) {
    if (config.value("_disableLamShortExit").toBool()) return {};
    for (const auto& value : config.value("medial").toArray()) {
        const auto form = value.toObject();
        if (formDescription(config, form, "body").value("shape") == "forked-stem")
            return formDescription(config, form, "cuts").value("shortExit").toObject();
    }
    return {};
}

static QJsonObject bodyConstraintForm(const QJsonObject& config, QJsonObject form) {
    if (!form.contains("body")) return form;
    const auto body = formDescription(config, form, "body");
    const auto cuts = formDescription(config, form, "cuts");
    if (sharedShapeDescription(body)) {
        form.insert("body", body); form.insert("cuts", cuts);
        form = sharedShapeProjectionForm(form);
    }
    if (toothDescription(body)) form.insert("tightJoin", cuts.value("tightJoin") == "shared-boundary");
    if (body.value("features").isObject()) form.insert("bodyFeatures", body.value("features"));
    if (body.value("counters").isDouble()) form.insert("bodyCounters", body.value("counters"));
    const auto stem = body.value("stem").toObject();
    if (stem.contains("headLip")) form.insert("_bodyPenWidth", stem.value("penWidth"));
    return form;
}

static QJsonObject bodyCutPolicy(const QJsonObject& description, const QString& letters, int slot) {
    auto policy = description;
    const auto terminal = policy.take("beforeFinal").toObject();
    if (slot + 2 == letters.size() && terminal.value("letters").toString().contains(letters.back()))
        policy = withOverrides(policy, terminal);
    return policy;
}

QString PageAnalysisResult::prepareLetterForms(QJsonObject& config) {
    if (config.value("version").toInt() != 2 || !config.value("initial").isArray() ||
        !config.value("medial").isArray() || !config.value("final").isArray()) {
        return "Expected a version 2 config with initial, medial and final lists";
    }
    for (const auto* obsolete : {"preferred", "fallback", "medialBodies"})
        if (config.contains(obsolete)) return QString("Obsolete template section: %1").arg(obsolete);
    if (config.contains("provisionalFallbacks")) {
        if (!config.value("provisionalFallbacks").isObject()) return "provisionalFallbacks must be an object";
        const auto fallbacks = config.value("provisionalFallbacks").toObject();
        for (auto setting = fallbacks.begin(); setting != fallbacks.end(); ++setting)
            if (setting.key() != "finalYehRemainder" || !setting.value().isBool())
                return "provisionalFallbacks supports only finalYehRemainder: true or false";
    }
    if (config.contains("recoveryChecks")) {
        const auto checks = config.value("recoveryChecks").toObject();
        const auto bowl = checks.value("finalBowl").toObject();
        const auto valid = [](const QJsonValue& x) {
            return x.isDouble() && std::isfinite(x.toDouble()) && x.toDouble() > 0;
        };
        if (!valid(checks.value("connectorMargin")) || !valid(checks.value("connectorBend")) ||
            bowl.value("letters").toString().isEmpty() || !valid(bowl.value("minWidth")) ||
            !valid(bowl.value("minHeight")) || !valid(bowl.value("tipTolerance")))
            return "Invalid recovery ownership measurements";
        if (checks.contains("longConnectorWidths") && !valid(checks.value("longConnectorWidths")))
            return "longConnectorWidths needs a positive dimension in stroke widths";
        if (checks.contains("bodyExtents") && !checks.value("bodyExtents").isArray())
            return "bodyExtents must be an array";
        for (const auto& v : checks.value("bodyExtents").toArray()) {
            const auto bounds = v.toObject();
            const auto position = bounds.value("position").toString();
            if ((bounds.contains("protectHead") &&
                 (!bounds.value("protectHead").isBool() || !bounds.contains("lips") ||
                  (position != "initial" && position != "medial"))) ||
                (bounds.contains("minHeadRiseWidths") &&
                 (position != "final" || !valid(bounds.value("minHeadRiseWidths")))) ||
                (bounds.contains("onUnresolved") && !bounds.value("onUnresolved").isBool()) ||
                (bounds.contains("lips") && (!valid(bounds.value("lips").toObject().value("minRun")) ||
                                             !valid(bounds.value("lips").toObject().value("minGap")))) ||
                (bounds.contains("maxWidthToHeight") && !valid(bounds.value("maxWidthToHeight"))) ||
                (bounds.contains("template") && bounds.value("template").toString().isEmpty()) ||
                bounds.value("letters").toString().isEmpty() ||
                !QStringList{"initial", "medial", "final"}.contains(position) || !valid(bounds.value("minWidth")) ||
                !valid(bounds.value("minHeight")))
                return "Invalid finished letter-body dimensions";
        }
    }
    if (config.contains("retryUnresolvedForms") && !config.value("retryUnresolvedForms").isBool())
        return "retryUnresolvedForms must be true or false";
    if (config.contains("unresolvedShapeFrames")) {
        const auto frames = config.value("unresolvedShapeFrames").toArray();
        if (!config.value("unresolvedShapeFrames").isArray() || frames.size() > 18)
            return "unresolvedShapeFrames needs at most 18 [width, slant, optional angle] measurements";
        for (const auto& value : frames) {
            const auto pair = value.toArray();
            if ((pair.size() != 2 && pair.size() != 3) || !pair[0].isDouble() || !pair[1].isDouble() ||
                (pair.size() == 3 && (!pair[2].isDouble() || !(std::abs(pair[2].toDouble()) <= 18))) ||
                !(pair[0].toDouble() >= .55 && pair[0].toDouble() <= 1.7 && std::abs(pair[1].toDouble()) <= .45))
                return "Invalid shape frame width (0.55–1.7), shear (−0.45–0.45) or angle (−18°–18°)";
        }
    }
    for (const auto* field : {"unresolvedScaleCenters", "unresolvedTiltAngles"}) {
        if (!config.contains(field))
            continue;
        const auto values = config.value(field).toArray();
        if (!config.value(field).isArray() || values.size() > 6)
            return QString(field) + " needs an array of at most six measurements";
        for (const auto& item : values) {
            const double value = item.toDouble();
            if (!item.isDouble() || !std::isfinite(value) ||
                (QString(field) == "unresolvedScaleCenters" ? (value < .65 || value > 1.4) : std::abs(value) > 15))
                return QString(field) + " is outside the supported scale or tilt range";
        }
    }
    if (config.contains("unresolvedInitialShaft")) {
        const auto shaft = config.value("unresolvedInitialShaft").toObject();
        if (shaft.value("letters").toString().isEmpty()) return "The initial shaft needs a letter selector";
        for (const auto* field : {"minHeight", "maxWidthToHeight", "edgeTolerance"}) {
            const auto value = shaft.value(field);
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() <= 0)
                return QString("Invalid initial shaft dimension: ") + field;
        }
    }
    if (config.contains("reconsiderScale")) {
        const auto retry = config.value("reconsiderScale").toObject();
        if (!(retry.value("fraction").toDouble() > 0 && retry.value("fraction").toDouble() < .5 &&
              retry.value("boundaryTolerance").toDouble() > 0))
            return "reconsiderScale needs a fraction between zero and one half and a positive boundary tolerance";
    }
    if (config.contains("reconsiderFinalAttachments") &&
        (!config.value("reconsiderFinalAttachments").isString() || config.value("reconsiderFinalAttachments").toString().isEmpty()))
        return "reconsiderFinalAttachments needs a nonempty final letter selector";
    if (config.contains("reconsiderForms")) {
        const auto groups = config.value("reconsiderForms").toObject();
        if (groups.isEmpty())
            return "reconsiderForms needs positional letter selectors";
        for (auto it = groups.begin(); it != groups.end(); ++it)
            if (!QStringList{"initial", "medial", "final"}.contains(it.key()) || !it.value().isString() ||
                it.value().toString().isEmpty())
                return "reconsiderForms supports nonempty initial, medial and final selectors";
    }
    static const auto bundled = readFormConfig(":/templates/letter-forms.json");
    const auto defaults = bundled.value("kashidaDefaults").toObject();
    auto problem = invalidKashidaSettings(config.value("kashidaDefaults"), defaults, "kashidaDefaults");
    if (!problem.isEmpty()) return problem;
    const auto effective = withOverrides(defaults, config.value("kashidaDefaults").toObject());
    QSet<QString> ids;
    for (const auto* phase : {"initial", "medial", "final"}) {
        for (const auto& entry : config.value(phase).toArray()) {
            if (!entry.isObject()) return QString(phase) + ": every template must be an object";
            auto rule = entry.toObject();
            QString sharedError;
            const auto sharedBody = formDescription(config, rule, "body", &sharedError);
            if (!sharedError.isEmpty()) return sharedError;
            if (sharedShapeDescription(sharedBody)) {
                const auto resolved = resolvedBodyForm(config, rule);
                sharedError = validateSharedShape(resolved, phase);
                if (!sharedError.isEmpty()) return sharedError;
                rule = sharedShapeValidationForm(resolved, phase);
            }
            const auto id = rule.value("id").toString().trimmed();
            if (id.isEmpty() || ids.contains(id)) return "Each template needs a unique nonempty id";
            ids.insert(id);
            if (!rule.value("current").isString() || rule.value("current").toString().isEmpty())
                return id + ": current must name the letters represented by this form";
            for (const auto* obsolete : {"next", "first", "last", "bodyBoundary", "deferAfterMedial"})
                if (rule.contains(obsolete)) return id + ": position is determined by its section; remove " + obsolete;
            for (const auto* key : {"tryNext", "retryOnPartial", "pairedOnly", "tightJoin", "sharedEntry", "sharedBoundary", "onPartial", "onIncomplete","onUnresolved","reclaimEntry","returningStroke","retryWithVariants","onHeadConflict"})
                if (rule.contains(key) && !rule.value(key).isBool()) return id + ": " + key + " must be true or false";
            if (rule.contains("sharedEntry") && QString(phase) != "medial")
                return id + ": sharedEntry belongs to a medial ligature form";
            if (rule.contains("sharedBoundary") && QString(phase) != "final")
                return id + ": sharedBoundary belongs to a final ligature form";
            if (rule.contains("sharedBoundaryWith") && (QString(phase) != "final" ||
                !rule.value("sharedBoundary").toBool() || !rule.value("sharedBoundaryWith").isString() ||
                rule.value("sharedBoundaryWith").toString().isEmpty()))
                return id + ": sharedBoundaryWith needs a final shared boundary and its ligature letters";
            const auto footAngle = rule.value("pairedCounterCorners").toObject().value("maxFootInwardAngle");
            if (!footAngle.isUndefined() && (!footAngle.isDouble() ||
                !std::isfinite(footAngle.toDouble()) || footAngle.toDouble() < 0 || footAngle.toDouble() >= 90))
                return id + ": maxFootInwardAngle must be an angle in degrees from 0 up to, but excluding, 90";
            const auto positive = [](const QJsonValue& value) {
                return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0;
            };
            const auto rimValue = loopSettings(rule).value("rimAnchors");
            if (!rimValue.isUndefined()) {
                const auto rim = rimValue.toObject();
                const auto point = [](const QJsonValue& value) {
                    const auto p = value.toArray();
                    return p.size() == 2 && p[0].isDouble() && p[1].isDouble() && std::isfinite(p[0].toDouble()) &&
                           std::isfinite(p[1].toDouble());
                };
                if (QString(phase) != "medial" || !rimValue.isObject() || !positive(rim.value("radius")) ||
                    !positive(rim.value("rimMargin")) || (QString(phase) != "initial" && !point(rim.value("entry"))) ||
                    (QString(phase) != "final" && !point(rim.value("exit"))) ||
                    (rim.contains("shaftRise") && !positive(rim.value("shaftRise"))))
                    return id + ": invalid counter-relative rim anchors, radius, margin or shaft rise";
            }
            if (hasCounterOutline(rule)) {
                const auto outline = rule.value("bodyOutline").toObject();
                const auto interval = [](const QJsonValue& value, bool positive = false) {
                    const auto bounds = value.toArray();
                    return bounds.size() == 2 && bounds[0].isDouble() && bounds[1].isDouble() &&
                           std::isfinite(bounds[0].toDouble()) && std::isfinite(bounds[1].toDouble()) &&
                           bounds[0].toDouble() <= bounds[1].toDouble() && (!positive || bounds[0].toDouble() > 0);
                };
                if (!rule.value("onUnresolved").toBool() || !interval(outline.value("aspect"), true) ||
                    (outline.value("counters") != 1 && outline.value("counters") != 2))
                    return id + ": bodyOutline needs one or two counters and a positive aspect range";
                if (rule.contains("requiresExitTemplate") && (QString(phase) != "medial" ||
                    rule.value("requiresExitTemplate").toString().isEmpty()))
                    return id + ": the counter outline needs a named final attachment";
                if (rule.contains("rimOffset") && (!positive(rule.value("rimOffset")) || !rule.contains("requiresExitTemplate")))
                    return id + ": a rim offset needs a positive counter-height distance and a final attachment";
                for (const auto* side : {"top", "bottom", "left", "right"})
                    if (!interval(outline.value("rim").toObject().value(side)))
                        return id + ": bodyOutline.rim needs four ordered ranges in counter heights";
                for (const auto* side : {"entry", "exit"}) {
                    const bool required =
                        QString(side) == "entry" ? QString(phase) != "initial" : QString(phase) != "final";
                    const auto attachment = outline.value(side).toObject();
                    if (outline.contains(side) != required ||
                        (required && (!interval(attachment.value("x")) || !interval(attachment.value("y")) ||
                                      !interval(attachment.value("width"), true))))
                        return id + ": invalid bodyOutline attachment for this position";
                }
                continue;
            }
            if (rule.contains("onFinalRetry") && (!rule.value("onFinalRetry").isBool() || rule.contains("onIncomplete")))
                return id + ": onFinalRetry must be a separate boolean retry phase";
            QString descriptionError;
            if (rule.contains("lamBody") || rule.contains("lamCuts"))
                return id + ": use body / cuts for declared forms";
            const auto declaredCuts = formDescription(config, rule, "cuts", &descriptionError);
            const auto declaredBody = formDescription(config, rule, "body", &descriptionError);
            if (!descriptionError.isEmpty()) return descriptionError;
            if (rule.contains("body") != rule.contains("cuts") ||
                (rule.contains("body") && (declaredBody.isEmpty() || declaredCuts.isEmpty())))
                return id + ": declared forms require both body and cuts";
            const auto method = rule.value("method").toString();
            if (method == "descending-stem" && (QString(phase) != "initial" ||
                !positive(rule.value("descendingStem").toObject().value("penWidth")) ||
                !positive(rule.value("descendingStem").toObject().value("minRise"))))
                return id + ": an initial descending stem needs a positive pen width and rise";
            const auto headLip = descendingStemDescription(config, rule).value("headLip");
            if (!headLip.isUndefined()) {
                const auto lip = headLip.toObject();
                if ((method != "descending-stem" && declaredBody.value("shape") != "descending-stem") || !rule.contains("ligatureWith") || !positive(lip.value("minRun")) ||
                    !positive(lip.value("maxRun")) || lip.value("maxRun").toDouble() <= lip.value("minRun").toDouble() ||
                    !positive(lip.value("maxDrop")))
                    return id + ": a stacked head needs a paired letter, ordered lip reach and positive drop";
            }
            if (rule.contains("terminalAttachment") &&
                (QString(phase) != "final" || !rule.value("terminalAttachment").isBool() ||
                 !rule.value("onFinalRetry").toBool()))
                return id + ": terminalAttachment belongs to a recovery-only final form";
            if (rule.contains("stemFoot") && (method != "bowl-fork" || !rule.value("stemFoot").isBool() ||
                                              (rule.contains("penWidth") && !positive(rule.value("penWidth")))))
                return id + ": stemFoot requires a bowl fork and a positive pen width";
            if (rule.contains("bridge") &&
                (method != "final-counter" || !positive(rule.value("bridge").toObject().value("maxWidth"))))
                return id + ": a final counter bridge needs a positive maximum width in counter heights";
            if (rule.contains("terminalBowl") &&
                (method != "seen-teeth" || rule.value("side") != "right" || !rule.value("terminalBowl").isBool()))
                return id + ": terminalBowl requires a final three-tooth form";
            if (rule.contains("roundedEntry") &&
                (!rule.value("terminalBowl").toBool() || !rule.value("roundedEntry").isBool()))
                return id + ": roundedEntry requires a terminal bowl";
            const auto neckWalk = rule.value("head").toObject().value("neckWalk");
            if (!neckWalk.isUndefined()) {
                if (method != "initial-loop-head" || !neckWalk.isObject() || neckWalk.toObject().value("top").toArray().isEmpty())
                    return id + ": head.neckWalk requires an initial loop head and a top route";
                problem = invalidWalkSettings(neckWalk.toObject(), id + ".head.neckWalk");
                if (!problem.isEmpty()) return problem;
            }
            for (const auto* owner : {"counter", "loopBoundaries"}) {
              for (const auto* side : {"entryCorner", "exitCorner"}) {
                const auto cornerValue = rule.value(owner).toObject().value(side);
                if (!cornerValue.isUndefined() && !cornerValue.isObject())
                    return id + ": " + side + " must be an object";
                const auto corner = cornerValue.toObject();
                const auto placement = corner.value("placement");
                if (!placement.isUndefined() && (!placement.isString() ||
                    !QStringList{"upper", "top-right", "right-corner-pair"}.contains(placement.toString())))
                    return id + ": " + side + ".placement must be upper, top-right or right-corner-pair";
                if (QString(side) == "exitCorner" && (!placement.isUndefined() && placement != "upper"))
                    return id + ": exitCorner.placement supports upper or the default lower neck";
                const auto offset = corner.value("strokeOffsetX");
                if (!offset.isUndefined() && (placement != "right-corner-pair" || !offset.isDouble() ||
                    !std::isfinite(offset.toDouble())))
                    return id + ": " + side + ".strokeOffsetX needs a finite distance and right-corner-pair";
                const auto distance = corner.value("maxDistance");
                if (!distance.isUndefined() && (!distance.isDouble() ||
                    !std::isfinite(distance.toDouble()) || distance.toDouble() <= 0))
                    return id + ": " + side + ".maxDistance must be positive, in counter heights";
                const auto counterY = corner.value("maxCounterY");
                if (!counterY.isUndefined() && (placement != "upper" || !counterY.isDouble() ||
                    !std::isfinite(counterY.toDouble()) || counterY.toDouble() < 0 || counterY.toDouble() > .5))
                    return id + ": " + side + ".maxCounterY needs upper placement and a value from 0 to 0.5";
                for (const auto* key : {"required", "normalCut"})
                    if (corner.contains(key) && !corner.value(key).isBool())
                        return id + ": " + side + "." + key + " must be true or false";
              }
            }
            if (rule.contains("ownsLeftTip") && (QString(phase) != "final" || (rule.value("start") != "left" &&
                declaredBody.value("shape") != "terminal-return" && declaredBody.value("shape") != "terminal-fork") ||
                !rule.value("ownsLeftTip").isBool()))
                return id + ": ownsLeftTip needs a final form starting from its own left tip";
            if (rule.contains("retainBodyFeatures") && (QString(phase) != "final" ||
                !rule.value("retainBodyFeatures").isBool()))
                return id + ": retainBodyFeatures must be a boolean on a final form";
            if (rule.contains("retainEntryCrest") && (QString(phase) != "final" ||
                !rule.value("retainEntryCrest").isBool() || !rule.value("ownsLeftTip").toBool()))
                return id + ": retainEntryCrest requires a final form that owns its left tip";
            if (rule.contains("bodyFeatures")) {
                if (QString(phase) == "medial" || !rule.value("bodyFeatures").isObject())
                    return id + ": bodyFeatures requires an initial or final body";
                const auto features = rule.value("bodyFeatures").toObject();
                for (auto feature = features.begin(); feature != features.end(); ++feature)
                    if (!QStringList{"minHeight", "minWidthToHeight", "maxWidthToHeight", "maxUpperPeaks"}.contains(feature.key()) ||
                        !feature.value().isDouble() || !std::isfinite(feature.value().toDouble()) ||
                        feature.value().toDouble() <= 0 ||
                        (feature.key() == "maxUpperPeaks" && feature.value().toDouble() != std::floor(feature.value().toDouble())))
                        return id + ": bodyFeatures needs positive dimensions and an integer peak limit";
                if (features.value("minWidthToHeight").toDouble() >
                    features.value("maxWidthToHeight").toDouble(INFINITY))
                    return id + ": minimum body aspect exceeds maximum";
            }
            if (rule.contains("tooth")) return id + ": medial teeth must use body / cuts";
            if (rule.contains("onPartial") && (QString(phase) != "initial" || method != "walk"))
                return id + ": onPartial requires an initial contour walk";
            if (rule.contains("recoveryStage")) {
                const auto stage = rule.value("recoveryStage");
                if (QString(phase) != "medial" || !toothDescription(declaredBody) || !stage.isDouble() ||
                    !std::isfinite(stage.toDouble()) || stage.toDouble() != std::floor(stage.toDouble()) ||
                    stage.toInt() < 1 || stage.toInt() > 3)
                    return id + ": recoveryStage must be 1, 2 or 3 on a declared medial tooth";
            }
            if (toothDescription(declaredBody)) {
                if (QString(phase) != "medial") return id + ": tooth shapes are medial bodies";
                for (const auto* key : {"method", "tightJoin"})
                    if (rule.contains(key)) return id + ": declared tooth geometry and policies belong in body / cuts";
                const bool pair = toothPairDescription(declaredBody);
                const QStringList dimensions = pair ? QStringList{"minPairDistance", "maxPairDistance", "minFlankRise",
                    "flankDrop", "maxTipOverhang", "minWidth", "maxWidth"} :
                    QStringList{"minPairDistance", "maxPairDistance", "minBendTurn"};
                for (auto key = declaredBody.begin(); key != declaredBody.end(); ++key) {
                    if (dimensions.contains(key.key())) {
                        if (!key.value().isDouble() || !std::isfinite(key.value().toDouble()) || key.value().toDouble() < 0)
                            return id + ": tooth body measurements must be finite and nonnegative";
                    } else if (key.key() != "shape" && key.key() != "baselineEntry" &&
                               !(pair && key.key() == "allowFlatBend"))
                        return id + ": unknown tooth body setting " + key.key();
                }
                if (!positive(declaredBody.value("maxPairDistance")) ||
                    declaredBody.value("minPairDistance").toDouble() >= declaredBody.value("maxPairDistance").toDouble() ||
                    (pair && (!positive(declaredBody.value("maxWidth")) ||
                     declaredBody.value("minWidth").toDouble() >= declaredBody.value("maxWidth").toDouble())))
                    return id + ": tooth body measurements need ordered pair-distance and width limits";
                if (!pair && (!declaredBody.contains("minBendTurn") || declaredBody.value("minBendTurn").toDouble() > 1))
                    return id + ": body.minBendTurn must be between 0 and 1";
                if (declaredBody.contains("allowFlatBend") && !declaredBody.value("allowFlatBend").isBool())
                    return id + ": body.allowFlatBend must be boolean";
                const auto entry = declaredBody.value("baselineEntry").toObject();
                if (declaredBody.contains("baselineEntry") && declaredBody.value("baselineEntry") != QJsonValue(false)) {
                    if (!positive(entry.value("minRise"))) return id + ": body.baselineEntry needs a positive minRise";
                    for (auto key = entry.begin(); key != entry.end(); ++key)
                        if (key.key() != "minRise" && (!pair || key.key() != "required" || !key.value().isBool()))
                            return id + ": body.baselineEntry supports minRise, plus required on a tooth-pair";
                }
                if (declaredCuts.value("offsetUnit") != "local-stroke" ||
                    !QStringList{"shared-boundary", "separate"}.contains(declaredCuts.value("tightJoin").toString()))
                    return id + ": tooth cuts need local-stroke offsets and a tightJoin policy";
                const auto boundaries = declaredCuts.value("boundaries").toObject();
                if (boundaries.size() != 2) return id + ": tooth cuts need exactly left and right boundaries";
                for (const auto* side : {"left", "right"})
                    if (boundaries.value(side) != (pair ? "tooth-normal" : "tooth-flank"))
                        return id + ": unsupported tooth boundary policy";
                if (pair) {
                    for (const auto* key : {"leftOffset", "rightOffset", "normalOffsetTolerance"}) {
                        const auto value = declaredCuts.value(key);
                        if ((QString(key) != "normalOffsetTolerance" || !value.isUndefined()) &&
                            (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0))
                            return id + ": tooth offsets must be nonnegative local stroke widths";
                    }
                } else if (!positive(declaredCuts.value("flankOffset")) || !positive(declaredCuts.value("maxSectionWidth")) ||
                    !QStringList{"contour-normal", "axis-projection", "contour-search"}.contains(declaredCuts.value("flankMode").toString()))
                    return id + ": tooth-flank cuts need a mode, positive flankOffset and maxSectionWidth";
                const auto turn = declaredCuts.value("baselineEntry").toObject();
                if (declaredCuts.contains("baselineEntry") && declaredCuts.value("baselineEntry") != QJsonValue(false)) {
                    if (entry.isEmpty() || !positive(turn.value("turnAngle")) || turn.value("turnAngle").toDouble() >= 90 ||
                        (turn.contains("minTurnRun") && (!pair || !positive(turn.value("minTurnRun")))))
                        return id + ": cuts.baselineEntry needs recognized entry geometry and a turn angle between 0 and 90";
                    for (auto key = turn.begin(); key != turn.end(); ++key)
                        if (key.key() != "turnAngle" && key.key() != "minTurnRun")
                            return id + ": cuts.baselineEntry supports turnAngle and minTurnRun";
                }
                QStringList policyKeys{"boundaries", "offsetUnit", "tightJoin", "baselineEntry"};
                policyKeys += pair ? QStringList{"leftOffset", "rightOffset", "normalOffsetTolerance"} :
                                     QStringList{"flankMode", "flankOffset", "maxSectionWidth"};
                for (auto key = declaredCuts.begin(); key != declaredCuts.end(); ++key)
                    if (!policyKeys.contains(key.key())) return id + ": unknown tooth cuts setting " + key.key();
            } else if (rule.contains("body") || rule.contains("cuts")) {
                if (!positive(declaredBody.value("minRise")) || !positive(declaredBody.value("maxWidthToHeight")) ||
                    !positive(declaredBody.value("minFlankSpan")) || declaredCuts.isEmpty())
                    return id + ": body needs positive flank/rise/aspect measurements and separate cuts";
                for (const auto* key : {"method", "top", "bottomProbe", "legacyLamRoutes", "shaft", "tailEntry", "descendingStem",
                                        "terminalArm", "detachedExit", "prefixTail", "bodyFeatures", "bodyCounters"})
                    if (rule.contains(key)) return id + ": lām geometry belongs in body / cuts, not " + key;
                const auto shape = declaredBody.value("shape").toString();
                const QStringList shapes = QString(phase) == "initial" ? QStringList{"contour-stem", "descending-stem"} :
                    QString(phase) == "medial" ? QStringList{"forked-stem", "turning-shaft", "detached-return", "attached-return"} :
                    QStringList{"terminal-bowl", "terminal-fork", "terminal-return"};
                if (!shapes.contains(shape)) return id + ": unknown body.shape for this position";
                const auto counters = declaredBody.value("counters");
                if (!counters.isUndefined() && counters != QJsonValue(false) &&
                    (!counters.isDouble() || !std::isfinite(counters.toDouble()) || counters.toDouble() < 0 ||
                     counters.toDouble() != std::floor(counters.toDouble())))
                    return id + ": body.counters needs a nonnegative integer or false";
                const auto checkRoute = [&](const QJsonValue& body, const QJsonValue& cuts, const QString& name) {
                    if (!body.isObject() || !cuts.isObject() || body.toObject().value("top").toArray().isEmpty())
                        return id + ": " + name + " needs body landmarks and a cut policy";
                    return invalidWalkSettings(withOverrides(body.toObject(), cuts.toObject()), id + "." + name);
                };
                if (shape == "contour-stem") {
                    problem = checkRoute(declaredBody.value("route"), declaredCuts.value("route"), "route");
                    if (!problem.isEmpty()) return problem;
                }
                if (shape == "forked-stem") {
                    const auto routes = declaredBody.value("exitRoutes").toArray(), cuts = declaredCuts.value("exitRoutes").toArray();
                    if (routes.isEmpty() || routes.size() != cuts.size()) return id + ": body/cut exitRoutes must have equal nonzero size";
                    for (int i = 0; i < routes.size(); ++i) {
                        problem = checkRoute(routes[i], cuts[i], "exitRoutes");
                        if (!problem.isEmpty()) return problem;
                    }
                    problem = checkRoute(declaredBody.value("entryRoute"), declaredCuts.value("entryRoute"), "entryRoute");
                    if (!problem.isEmpty()) return problem;
                    const auto placement = declaredCuts.value("placement").toObject();
                    if (!positive(placement.value("entryOverlap")) || !positive(placement.value("turnAngle")) ||
                        placement.value("turnAngle").toDouble() >= 90 || !placement.value("levelForkEntry").isBool())
                        return id + ": invalid cuts.placement";
                }
                const auto stem = declaredBody.value("stem").toObject();
                if ((shape == "descending-stem" || shape == "terminal-fork") &&
                    (!positive(stem.value("minRise")) || !positive(stem.value("penWidth"))))
                    return id + ": body.stem needs positive penWidth and minRise";
                if (shape == "terminal-bowl" && (!positive(stem.value("minRise")) || !positive(stem.value("maxNeckDistance")) ||
                    !positive(declaredCuts.value("section").toObject().value("turnAngle"))))
                    return id + ": invalid terminal bowl body / section";
                if (shape == "turning-shaft" && (!positive(declaredBody.value("shaft").toObject().value("minRise")) ||
                    !positive(declaredBody.value("shaft").toObject().value("maxWidthToHeight"))))
                    return id + ": invalid body.shaft";
                if (shape == "attached-return" && (declaredBody.value("terminalArm").toString().isEmpty() ||
                    !positive(declaredCuts.value("entryTurnAngle")) || declaredCuts.value("entryTurnAngle").toDouble() >= 90))
                    return id + ": invalid returning body / entry angle";
                if (shape == "detached-return" && declaredBody.value("detachedExit").toString().isEmpty())
                    return id + ": detached return needs exit letters";
                if (shape == "terminal-return" || shape == "detached-return") {
                    const auto tail = declaredBody.value("tail").toObject();
                    for (const auto* key : {"minRise", "minRun", "maxRun"})
                        if (!positive(tail.value(key))) return id + ": invalid body.tail";
                }
                if ((shape == "terminal-return" || shape == "detached-return" || shape == "terminal-fork") &&
                    !positive(declaredCuts.value("section").toObject().value("walkWidths")))
                    return id + ": section.walkWidths must be positive";
                const auto shortExit = declaredCuts.value("shortExit");
                if (!shortExit.isUndefined() && shortExit != QJsonValue(false)) {
                    if (!shortExit.isObject()) return "cuts.shortExit must be an object or false";
                    const auto settings = shortExit.toObject();
                    for (const auto* key : {"minHorizontalRun", "maxEntryGap", "minLengthRatio", "anchorTolerance"}) {
                        const auto value = settings.value(key);
                        const double minimum = QString(key) == "minLengthRatio" ? 1 : 0;
                        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() <= minimum)
                            return QString("cuts.shortExit.%1 must be greater than %2").arg(key).arg(minimum);
                    }
                    if (!settings.value("retryOnUnresolved").isBool())
                        return "cuts.shortExit.retryOnUnresolved must be true or false";
                    const auto hollow = settings.value("cutsAroundHollow");
                    if (!hollow.isUndefined() && !hollow.isBool())
                        return "cuts.shortExit.cutsAroundHollow must be true or false";
                    const auto tilt = settings.value("entryMaxTilt");
                    if (!tilt.isUndefined() && (!tilt.isDouble() || !std::isfinite(tilt.toDouble()) || tilt.toDouble() < 0 || tilt.toDouble() >= 90))
                        return "cuts.shortExit.entryMaxTilt must be an angle from 0 up to 90 degrees";
                }
                const auto arm = declaredBody.value("alefArm");
                if (!arm.isUndefined() && (!arm.isString() ||
                    namedLetterForm(config, arm.toString()).value("method") != "folded-arm" ||
                    declaredBody.value("alefLetters").toString().isEmpty()))
                    return id + ": body.alefArm needs a folded-arm body template and alefLetters";
                const auto forkOffset = declaredCuts.value("forkOffset");
                if (!forkOffset.isUndefined() && !positive(forkOffset))
                    return id + ": cuts.forkOffset must be positive in page units";
                if (!QStringList{"tip-normal", "preserve"}.contains(declaredCuts.value("tightJoin").toString()) ||
                    declaredCuts.value("counterJoin") != "preserve-entry")
                    return id + ": cuts needs a tightJoin policy and counterJoin: preserve-entry";
                const auto terminal = declaredCuts.value("beforeFinal");
                if (!terminal.isUndefined() && terminal != QJsonValue(false) &&
                    (!terminal.isObject() || terminal.toObject().value("letters").toString().isEmpty()))
                    return id + ": cuts.beforeFinal needs a terminal letter selector, or false";
                for (const auto& settings : {declaredCuts, terminal.toObject()}) {
                    if (settings.isEmpty()) continue;
                    const auto boundaries = settings.value("boundaries").toObject();
                    if (!QStringList{"free", "preserve", "fork-normal"}.contains(boundaries.value("right").toString()) ||
                        !QStringList{"free", "preserve", "fork-normal", "shared-return"}.contains(boundaries.value("left").toString()))
                        return id + ": cuts.boundaries must declare the right and left cut policies";
                    if (boundaries.value("left") == "fork-normal" && boundaries.value("right") != "fork-normal")
                        return id + ": an outgoing fork-normal policy currently needs the incoming fork-normal policy";
                    if (boundaries.value("left") == "shared-return" && !settings.contains("letters"))
                        return id + ": a shared-return policy belongs in beforeFinal with its letter selector";
                    if (settings.contains("preserveEntryAfter") && !settings.value("preserveEntryAfter").isString())
                        return id + ": cuts.preserveEntryAfter must be a letter selector";
                    if (settings.contains("junction") && (settings.value("junction") != "inward-corners" ||
                        arm.isUndefined() || boundaries.value("left") != "shared-return"))
                        return id + ": inward-corners needs a shared-return boundary and an alefArm body";
                    for (auto key = settings.begin(); key != settings.end(); ++key)
                        if (!QStringList{"boundaries", "forkOffset", "tightJoin", "counterJoin", "beforeFinal",
                                         "letters", "junction", "preserveEntryAfter", "route", "section", "exitRoutes",
                                         "entryRoute", "placement", "entryTurnAngle", "shortExit"}.contains(key.key()))
                            return id + ": unknown cuts setting " + key.key();
                }
            }
            if (rule.contains("shaftTurn") && (method != "alef-stem" || !rule.value("shaftTurn").isBool()))
                return id + ": shaftTurn requires an alef stem and must be true or false";
            if (rule.contains("tailEntry")) {
                const auto tail = rule.value("tailEntry").toObject();
                if (QString(phase) != "final" || (method != "walk" && method != "tail-entry") ||
                    (tail.contains("minRun") && !positive(tail.value("minRun"))) ||
                    (tail.contains("maxRun") &&
                     (!positive(tail.value("maxRun")) ||
                      tail.value("maxRun").toDouble() < tail.value("minRun").toDouble(2))) ||
                    (tail.contains("turnAngle") &&
                     (!positive(tail.value("turnAngle")) || tail.value("turnAngle").toDouble() >= 90)) ||
                    !positive(tail.value("walkWidths")) || !positive(tail.value("minRise")))
                    return id + ": tailEntry needs positive walkWidths and minRise and ordered run limits";
            }
            const auto bodyCounters = rule.value("bodyCounters");
            if (!bodyCounters.isUndefined() && (!bodyCounters.isDouble() ||
                !std::isfinite(bodyCounters.toDouble()) || bodyCounters.toDouble() < 0 ||
                bodyCounters.toDouble() != std::floor(bodyCounters.toDouble())))
                return id + ": bodyCounters must be a nonnegative integer";
            if (rule.contains("ligatureWith") && ((method != "counter-ligature" && headLip.isUndefined()) || QString(phase) != "initial" ||
                !rule.value("ligatureWith").isString() || rule.value("ligatureWith").toString().isEmpty()))
                return id + ": ligatureWith requires an initial ligature form and its paired letters";
            const auto counterLigature = rule.value("counterLigature").toObject();
            if (rule.contains("counterLigature") &&
                (QString(phase) != "medial" || counterLigature.value("letters").toString().isEmpty() ||
                 !positive(counterLigature.value("forkRise")) ||
                 !positive(counterLigature.value("minAscenderRise")) ||
                 !positive(counterLigature.value("maxBridgeRatio"))))
                return id + ": a medial counter ligature needs paired letters, fork rise, ascender rise and bridge "
                            "ratio";
            const auto maxBend = rule.value("maxBridgeBend").isUndefined() ? counterLigature.value("maxBridgeBend")
                                                                           : rule.value("maxBridgeBend");
            if (!maxBend.isUndefined() && (!positive(maxBend) || maxBend.toDouble() > .25))
                return id + ": an ink bridge bend must be within a quarter of its length";
            if (rule.contains("forkRise") && (method != "counter-ligature" || !positive(rule.value("forkRise"))))
                return id + ": forkRise needs a positive counter-height ratio";
            for (const auto* name : {"mouth", "blade", "shaft", "enclosedHead"})
                if (rule.contains(name)) {
                    const auto feature = rule.value(name).toObject();
                    if (QString(phase) != "medial" || feature.isEmpty())
                        return id + ": expected a medial shape description";
                    for (auto it = feature.begin(); it != feature.end(); ++it) {
                        const bool flag = it.key() == "projectExit" ||
                            (QLatin1String(name) == "mouth" && it.key() == "separateSharedEntry");
                        if (flag ? !it.value().isBool() : !positive(it.value()))
                            return id + ": expected a boolean shape flag or a positive measurement";
                    }
                    if (QLatin1String(name) == "mouth" && feature.value("separateSharedEntry").toBool() &&
                        (!positive(feature.value("maxThroat")) || !positive(feature.value("exitMargin"))))
                        return id + ": a separate mouth entry needs positive throat and exit dimensions";
                }
            if (rule.contains("terminalArm") &&
                (QString(phase) != "medial" || rule.value("terminalArm").toString().isEmpty() ||
                 !positive(rule.value("entryTurnAngle")) || rule.value("entryTurnAngle").toDouble() >= 90))
                return id +
                       ": a returning stem needs a final-arm template and an entry turn between 0 and 90 degrees";
            if (rule.contains("overhangingEntry") &&
                (QString(phase) != "medial" ||
                 !positive(rule.value("overhangingEntry").toObject().value("maxGap")) ||
                 !positive(rule.value("overhangingEntry").toObject().value("maxEntryToHead"))))
                return id + ": the detached entry needs a gap limit and entry-to-head width ratio";
            if (method == "folded-arm" && (!positive(rule.value("minRise")) || !positive(rule.value("maxNeck"))))
                return id + ": a folded arm needs a positive rise and neck width";
            if (method == "bowl-fork" && (!positive(rule.value("minRise")) || !positive(rule.value("walkWidths"))))
                return id + ": a bowl fork needs a positive rise and contour distance";
            if (method == "detached-arm" && (!positive(rule.value("detachedArm").toObject().value("minRise")) ||
                                             !positive(rule.value("detachedArm").toObject().value("maxTipGap"))))
                return id + ": a detached arm needs a positive rise and maximum tip gap";
            if (rule.contains("detachedExit") &&
                (QString(phase) != "medial" || rule.value("detachedExit").toString().isEmpty() ||
                 rule.value("prefixTail").toObject().value("method") != "tail-entry"))
                return id + ": expected a returning stem and detached exit letters";
            if (method == "tail-entry" && !rule.contains("tailEntry"))
                return id + ": tail-entry needs its tail measurements";
            if (method == "counter-ligature") {
                if (!rule.contains("ligatureWith") ||
                    (!rule.contains("forkRise") && rule.value("outer").toObject().value("top").toArray().isEmpty()))
                    return id + ": counter-ligature needs paired letters and a fork or an outer contour walk";
                problem = invalidWalkSettings(rule.value("outer").toObject(), id + ".outer");
                if (!problem.isEmpty()) return problem;
                for (const auto* key : {"maxBridgeRatio", "minAscenderRise"})
                    if (!rule.value(key).isDouble() || !std::isfinite(rule.value(key).toDouble()) ||
                        rule.value(key).toDouble() <= 0) return id + ": " + key + " must be a positive counter-height ratio";
                if (rule.contains("lowerBridge") && !QStringList{"right-tip", "bottom-extrema", "stem-side"}.contains(
                                                        rule.value("lowerBridge").toString()))
                    return id + ": lowerBridge must be right-tip, bottom-extrema or stem-side";
                if (rule.value("lowerBridge") == "stem-side" && !rule.contains("forkRise"))
                    return id + ": a stem-side bridge requires a fork rise";
            }
            if (rule.contains("lamPosition") && (method != "lam-alef" ||
                !QStringList{"initial", "medial"}.contains(rule.value("lamPosition").toString())))
                return id + ": lamPosition requires lam-alef and must be initial or medial";
            if (rule.contains("sameDirectionCheck") &&
                (method != "lam-alef" || !rule.value("sameDirectionCheck").isBool()))
                return id + ": sameDirectionCheck requires lam-alef and must be true or false";
            const auto teeth = method == "seen-teeth" ? rule : rule.value("seenTeeth").toObject();
            if (rule.contains("sharedFoldWithFinal") && (QString(phase) != "medial" ||
                (teeth.isEmpty() && rule.value("contourBody").toObject().isEmpty()) ||
                !rule.value("sharedFoldWithFinal").isString() || rule.value("sharedFoldWithFinal").toString().isEmpty()))
                return id + ": sharedFoldWithFinal needs medial teeth or contour routes and the letters sharing the fold";
            const auto lastPitch = teeth.value("maxLastPitch");
            if (!lastPitch.isUndefined() && (!lastPitch.isDouble() || !std::isfinite(lastPitch.toDouble()) ||
                lastPitch.toDouble() < teeth.value("minPitch").toDouble(1)))
                return id + ": maxLastPitch must be at least minPitch";
            const auto widerPitch = teeth.value("retryMaxLastPitch");
            if (!widerPitch.isUndefined() && (!widerPitch.isDouble() || !std::isfinite(widerPitch.toDouble()) ||
                widerPitch.toDouble() <= lastPitch.toDouble(teeth.value("maxPitch").toDouble(5))))
                return id + ": retryMaxLastPitch must exceed the normal last pitch limit";
            const auto tipSpread = teeth.value("maxTipHeightSpreadRatio");
            if (!tipSpread.isUndefined() && (!tipSpread.isDouble() ||
                !std::isfinite(tipSpread.toDouble()) || tipSpread.toDouble() <= 0))
                return id + ": maxTipHeightSpreadRatio must be a positive ratio of the mean tooth pitch";
            const auto entryOffset = teeth.value("rightOffset");
            if (!entryOffset.isUndefined() && (!entryOffset.isDouble() ||
                !std::isfinite(entryOffset.toDouble()) || entryOffset.toDouble() < 0))
                return id + ": rightOffset must be a nonnegative distance in local stroke widths";
            const auto exitDistance = teeth.value("maxExitBendDistance");
            if (!exitDistance.isUndefined() && (!exitDistance.isDouble() ||
                !std::isfinite(exitDistance.toDouble()) || exitDistance.toDouble() <= 0))
                return id + ": maxExitBendDistance must be a positive ratio of the mean tooth pitch";
            if (teeth.contains("exitBendFirst") && !teeth.value("exitBendFirst").isBool())
                return id + ": exitBendFirst must be true or false";
            const auto retryBend = teeth.value("retryBendOffset");
            if (!retryBend.isUndefined() && (QString(phase) == "final" || !retryBend.isDouble() ||
                !std::isfinite(retryBend.toDouble()) || retryBend.toDouble() < 0))
                return id + ": retryBendOffset needs an outgoing edge and a nonnegative stroke-width offset";
            if (teeth.contains("entryCornerOptional") &&
                (!teeth.value("entryCornerOptional").isBool() || !teeth.value("rightOffset").isDouble()))
                return id + ": entryCornerOptional needs a boolean and rightOffset";
            if (teeth.contains("convexTips") && !teeth.value("convexTips").isBool())
                return id + ": convexTips must be true or false";
            const auto exitOffset = teeth.value("exitOffset");
            if (!exitOffset.isUndefined() && (!exitOffset.isDouble() || !std::isfinite(exitOffset.toDouble()) || exitOffset.toDouble() < 0))
                return id + ": exitOffset must be nonnegative in local stroke widths";
            const auto head = rule.value("headEdges").toObject();
            if (head.contains("maxExitDistance"))
                return id + ": headEdges uses its own counter and edges; remove maxExitDistance";
            const auto neckDistance = head.value("maxNeckDistance");
            if (!neckDistance.isUndefined() && (!neckDistance.isDouble() ||
                !std::isfinite(neckDistance.toDouble()) || neckDistance.toDouble() <= 0))
                return id + ": maxNeckDistance must be a positive distance in counter heights";
            if (QString(phase) == "medial") {
                if (rule.contains("penLift")) {
                    const auto lift = rule.value("penLift").toObject();
                    if (!QStringList{"entry", "exit"}.contains(lift.value("side").toString()) ||
                        lift.value("forms").toArray().isEmpty() || !rule.value("onUnresolved").toBool())
                        return id + ": a pen lift needs an exposed side and its allowed forms";
                    continue;
                }
                if (rule.contains("exposedBy") || rule.contains("exposedEntry")) {
                    const auto heads = rule.value("headForms").toArray();
                    if (rule.contains("exposedEntry") == rule.contains("exposedBy") ||
                        (rule.contains("exposedBy") && rule.value("exposedBy").toString().isEmpty()) ||
                        (rule.contains("exposedEntry") && rule.value("exposedEntry") != "shared") ||
                        heads.isEmpty() || std::any_of(heads.begin(), heads.end(), [](const auto& h) {
                            return !h.isString() || h.toString().isEmpty();
                        }) || !rule.value("onUnresolved").toBool() || rule.contains("method"))
                        return id + ": an exposed medial head needs an entry and allowed head forms";
                    continue;
                }
                const bool feature = rule.contains("terminalArm") || rule.contains("overhangingEntry") || rule.contains("parallelReturn") || rule.contains("enclosedHead") || rule.contains("shaft") || rule.contains("detachedExit") || rule.contains("counterLigature") || rule.contains("mouth") || rule.contains("blade") || !loopSettings(rule).isEmpty() ||
                    !rule.value("contourBody").toObject().isEmpty() ||
                    !rule.value("pairedCounterCorners").toObject().isEmpty() ||
                    !rule.value("openHead").toObject().isEmpty() || !rule.value("seenTeeth").toObject().isEmpty() ||
                    !declaredBody.isEmpty() || !rule.value("headEdges").toObject().isEmpty() ||
                    (!rule.value("right").toObject().value("walk").toArray().isEmpty() &&
                     !rule.value("left").toObject().value("walk").toArray().isEmpty());
                if (!feature || rule.contains("method")) return id + ": a medial form must define both body boundaries";
            } else {
                static const QStringList initialMethods{"enclosed-head","walk","descending-stem", "initial-counter", "initial-loop-head", "initial-loop-body", "heh-initial-counters", "seen-teeth", "kaf", "kaf-zigzag", "hah", "counter-ligature"};
                static const QStringList finalMethods{"rising-tail","folded-bowl","folded-arm","bowl-fork","detached-arm", "tail-entry", "walk", "reh", "qaf", "noon", "alef-stem", "stem-entry", "lam-alef", "waw", "sad-tah", "final-counter", "seen-teeth"};
                if (declaredBody.isEmpty() && !(QString(phase) == "initial" ? initialMethods : finalMethods).contains(method))
                    return id + ": an explicit body method for this position is required; generic join rules are unsupported";
            }
            if (rule.contains("bottomProbe") && method != "walk") return id + ": bottomProbe requires method walk";
            if (method == "rising-tail") {
                const auto spec = rule.value("tail").toObject();
                for (auto key : {"minRun", "minRise", "maxChord"})
                    if (!positive(spec.value(key))) return id + ": rising tail distances must be positive";
            }
            if (method == "folded-bowl") {
                const auto fold = rule.value("fold").toObject();
                for (const auto* key : {"minBowlRun","minFoldRun","minReturn","maxChord"})
                    if (!positive(fold.value(key))) return id + ": folded bowl distances must be positive";
            }
            if (method == "walk") {
                problem = invalidWalkSettings(rule, id);
                if (!problem.isEmpty()) return problem;
            }
            const auto entryWalk = loopSettings(rule).value("entryWalk");
            if (!entryWalk.isUndefined()) {
                if (!entryWalk.isObject() || entryWalk.toObject().value("top").toArray().isEmpty())
                    return id + ": loopBoundaries.entryWalk needs a top walk";
                problem = invalidWalkSettings(entryWalk.toObject(), id + ".loopBoundaries.entryWalk");
                if (!problem.isEmpty()) return problem;
            }
            if (rule.contains("upperAttachment")) {
                const auto attachment = rule.value("upperAttachment").toObject();
                const auto right = rule.value("contourBody").toObject().value("right").toObject();
                if (QString(phase) != "medial" || !rule.value("sharedEntry").toBool() ||
                    right.value("bottomProbe") != "left" || right.value("anchorSide") != "bottom" ||
                    attachment.value("letters").toString().isEmpty())
                    return id + ": upperAttachment needs a shared leftward probe and upper letters";
                for (const auto key : {"headFlank", "risingFlank"}) {
                    const auto bounds = attachment.value(key).toObject();
                    if (bounds.isEmpty()) return id + ": upperAttachment needs both flank bounds";
                    for (auto it = bounds.begin(); it != bounds.end(); ++it) {
                        if (it.key() == "accumulate" && it.value().isBool()) continue;
                        if (!QStringList{"minX","maxX","minY","maxY"}.contains(it.key()) ||
                            !it.value().isDouble() || !std::isfinite(it.value().toDouble()))
                            return id + ": upperAttachment needs finite x/y bounds";
                    }
                    if (bounds.value("minX").toDouble(-INFINITY) > bounds.value("maxX").toDouble(INFINITY) ||
                        bounds.value("minY").toDouble(-INFINITY) > bounds.value("maxY").toDouble(INFINITY))
                        return id + ": upperAttachment flank minimum exceeds maximum";
                }
            }
            if (rule.contains("contourBody")) {
                const auto orient = rule.value("contourBody").toObject().value("orientToBody");
                if (!orient.isUndefined() && !orient.isBool()) return id + ": contourBody.orientToBody must be true or false";
                const auto convex = rule.value("contourBody").toObject().value("anchor").toObject().value("convex");
                if (!convex.isUndefined() && !convex.isBool()) return id + ": contour anchor convex must be true or false";
                for (const auto& flank : rule.value("contourBody").toObject().value("anchor").toObject().value("flanks").toArray()) {
                    const auto retained = flank.toObject().value("insideBody");
                    if (!retained.isUndefined() && !retained.isBool())
                        return id + ": contour anchor flank insideBody must be true or false";
                }
                for (const auto* side : {"right", "left"}) {
                    const auto route = rule.value("contourBody").toObject().value(side);
                    if (!route.isObject() || (route.toObject().value("top").toArray().isEmpty() &&
                        !route.toObject().value("topAtAnchor").toBool()))
                        return id + ": contourBody." + side + " needs a top walk or topAtAnchor";
                    problem = invalidWalkSettings(route.toObject(), id + ".contourBody." + side);
                    if (!problem.isEmpty()) return problem;
                }
                const auto retain = rule.value("contourBody").toObject().value("retain");
                if (!retain.isUndefined() && !retain.isArray())
                    return id + ": contourBody.retain must list intrinsic feature descriptions";
                for (const auto& value : retain.toArray()) {
                    if (!value.isObject() || value.toObject().isEmpty())
                        return id + ": a retained feature must have a description";
                    const auto feature = value.toObject();
                    for (const auto* flag : {"sharp", "xExtremum", "yExtremum"})
                        if (feature.contains(flag) && !feature.value(flag).isBool())
                            return id + ": retained landmark flags must be booleans";
                    for (const auto* field : {"offset", "flanks"}) {
                        if (!feature.value(field).isObject()) return id + ": retained landmarks need offset and flank bounds";
                        const auto bounds = feature.value(field).toObject();
                        for (auto item = bounds.begin(); item != bounds.end(); ++item)
                            if (!QStringList{"minX", "maxX", "minY", "maxY"}.contains(item.key()) ||
                                !item.value().isDouble() || !std::isfinite(item.value().toDouble()))
                                return id + ": retained landmark bounds must be finite x/y distances";
                        if (bounds.value("minX").toDouble(-INFINITY) > bounds.value("maxX").toDouble(INFINITY) ||
                            bounds.value("minY").toDouble(-INFINITY) > bounds.value("maxY").toDouble(INFINITY))
                            return id + ": retained landmark minimum exceeds maximum";
                    }
                }
            }
            const auto counterPair = loopSettings(rule).value("counterPair");
            const auto loopHeight = loopSettings(rule).value("maxBodyHeight");
            if (!loopHeight.isUndefined() && (!loopHeight.isDouble() || !std::isfinite(loopHeight.toDouble()) ||
                loopHeight.toDouble() <= 0)) return id + ": loopBoundaries.maxBodyHeight must be positive, in counter heights";
            if (!counterPair.isUndefined() && !QStringList{"horizontal", "vertical"}.contains(counterPair.toString())) {
                return id + ": loopBoundaries.counterPair must be horizontal or vertical";
            }
            problem = invalidKashidaSettings(rule.value("kashida"), effective, id + ".kashida");
            if (!problem.isEmpty()) return problem;
        }
    }
    config.insert("kashidaDefaults", effective);
    return {};
}

static QJsonObject& activeLetterForms() {
    static auto config = [] {
        const auto bundled = readFormConfig(":/templates/letter-forms.json");
        const auto overridePath = qEnvironmentVariable("MEDINA_LETTER_FORMS");
        if (!overridePath.isEmpty()) {
            auto custom = readFormConfig(overridePath);
            const auto problem = PageAnalysisResult::prepareLetterForms(custom);
            if (problem.isEmpty()) return custom;
            qWarning().noquote() << "Invalid letter form configuration" << overridePath
                                 << "-" << problem << "- using bundled forms";
        }
        return bundled;
    }();
    return config;
}

const QJsonObject& PageAnalysisResult::letterForms() {
    return activeLetterForms();
}

void PageAnalysisResult::activateLetterForms(const QJsonObject& config) {
    activeLetterForms() = config;
}

static double formNumber(const SearchContext& context, const char* key, double fallback) {
    return context.form.value(key).toDouble(fallback);
}

static bool within(const QJsonObject& bounds, const char* min, const char* max, double value) {
    return (!bounds.contains(min) || value >= bounds.value(min).toDouble()) &&
           (!bounds.contains(max) || value <= bounds.value(max).toDouble());
}

// Qt page coordinates increase rightward and downward.
static bool matchesOffset(const QJsonObject& bounds, const QPointF& delta) {
    const auto direction = bounds.value("direction");
    const auto matchesDirection = [&](const QString& name) {
        if (name == "left") return delta.x() < 0;
        if (name == "right") return delta.x() > 0;
        if (name == "up") return delta.y() < 0;
        if (name == "down") return delta.y() > 0;
        return false;
    };
    if (direction.isString() && !matchesDirection(direction.toString())) return false;
    if (direction.isArray()) {
        const auto options = direction.toArray();
        if (std::none_of(options.begin(), options.end(), [&](const QJsonValue& name) {
            return matchesDirection(name.toString());
        })) return false;
    }
    return within(bounds, "minX", "maxX", delta.x()) &&
           within(bounds, "minY", "maxY", delta.y()) &&
           within(bounds, "minDistance", "maxDistance", std::hypot(delta.x(), delta.y()));
}

static bool matchesNeighbors(const QJsonArray& relations, const Stretching& join) {
    const QMap<QString, QPointF> points{
        {"top", join.topBaseLine.point}, {"bottom", join.bottomBaseLine.point},
        {"topLeft", join.topLeft.point}, {"topRight", join.topRight.point},
        {"bottomLeft", join.bottomLeft.point}, {"bottomRight", join.bottomRight.point},
    };
    for (const auto& value : relations) {
        const auto relation = value.toObject();
        const auto from = points.constFind(relation.value("from").toString());
        const auto to = points.constFind(relation.value("to").toString());
        if (from == points.end() || to == points.end() || !matchesOffset(relation, *to - *from)) return false;
    }
    return true;
}

static bool matchesJoinFeatures(SearchContext& context) {
    const auto& join = context.res;
    const auto chord = join.bottomBaseLine.point - join.topBaseLine.point;
    if (!matchesOffset(context.form.value("chord").toObject(), chord)) return false;

    const auto ranges = context.form.value("extrema").toObject();
    if (ranges.isEmpty()) return true;
    const auto previous = getJoinTime(context);
    const auto current = getJoinTime(join, context.orientation);
    int total = 0, x = 0, y = 0, sharp = 0;
    for (const auto& point : context.timeSorted) {
        if (!previous.isStrictLeft(point.time) || !current.isStrictRight(point.time)) continue;
        ++total;
        x += point.isX;
        y += point.isY;
        sharp += point.isSharp;
    }
    return within(ranges.value("total").toObject(), "min", "max", total) &&
           within(ranges.value("x").toObject(), "min", "max", x) &&
           within(ranges.value("y").toObject(), "min", "max", y) &&
           within(ranges.value("sharp").toObject(), "min", "max", sharp);
}

static std::function<bool(xsortedtype::iterator)> defaultCheck = [](xsortedtype::iterator xiter) {return true; };

struct JoinCheck {
	bool bottomCheck = true;
	bool topCheck = true;
	bool sameDirectionCheck = true;
	std::function<bool(xsortedtype::iterator)> customCheck = defaultCheck;
};

bool static getJoin(SearchContext& context, const JoinCheck& joinCheck) {
    const auto bounds = withOverrides(context.joinDefaults,
                                      context.form.value("join").toObject());
    const int plookbackward = bounds.value("lookback").toInt(4);
    const int prevOff = bounds.value("previousOffset").toInt(1);
    if (plookbackward < 1 || prevOff < 1) return false;

	auto xiter = context.nextIter;
	auto& xSorted = context.xSorted;
	auto& res = context.res;
	auto& timeSorted = context.timeSorted;
	auto& transformedPath = context.transformedPath;
	double minDistance = 0.01;

	for (; xiter != xSorted.end(); xiter++) {
		auto& curr = *xiter;

        auto prevBasePoint = xiter;
        int offset = prevOff;
        while (offset > 0 && prevBasePoint != xSorted.begin()) {
            --prevBasePoint;
            --offset;
        }
        if (offset > 0) continue;

		auto lookbackward = plookbackward;

		while (lookbackward != 0 && prevBasePoint != xSorted.end()) {

			auto& prec = *prevBasePoint;

			prevBasePoint = prevBasePoint == xSorted.begin() ? xSorted.end() : std::prev(prevBasePoint);

			auto yDiff = std::abs(curr.point.y() - prec.point.y());
			auto xDiff = std::abs(curr.point.x() - prec.point.x());


			if (!within(bounds, "minX", "maxX", xDiff) || !within(bounds, "minY", "maxY", yDiff)) {
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
				prev = context.prev(prev);

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

				prev = context.prev(prev);


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

			const auto neighbors = context.form.value("neighbors").toArray();
            if ((!neighbors.isEmpty() && !matchesNeighbors(neighbors, res)) ||
                !joinCheck.customCheck(xiter) || !matchesJoinFeatures(context)) {
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

		auto center = context.res.topLeft.point;

		QPainterPath temp;

		temp.addRect(QRectF{ QPointF{ center.x() - formNumber(context, "probeHalfWidth", 0.2),context.bbox.top() } ,QPointF{ center.x() + formNumber(context, "probeHalfWidth", 0.2),context.bbox.bottom()} });

		temp.setFillRule(Qt::WindingFill);
		auto intersected = context.transformedPath.intersected(temp);

		int nbpath = 0;

		for (int elemIndex = 0; elemIndex < intersected.elementCount(); elemIndex++) {
			auto elem = intersected.elementAt(elemIndex);
			if (elem.type == QPainterPath::MoveToElement) {
				nbpath++;
			}
		}

		if (nbpath != formNumber(context, "crossings", 2)) {
			return false;
		}

		return true;
	};

	return getJoin(context, joinCheck);

}



bool static searchNextSadOrTah(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.bottomCheck = true;
	joinCheck.topCheck = false;

	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		auto bbox = context.transformedPath.boundingRect();

		QLineF segment{ context.res.topBaseLine.point,context.res.bottomBaseLine.point };

		auto center = segment.center();

		QLineF line{ {center.x() ,bbox.top()}, {center.x() ,bbox.bottom()} };

		auto intersections = FindInterections(context.transformedPath, line);

		if (intersections.size() != formNumber(context, "crossings", 4)) {
			return false;
		}

		auto x = context.res.topBaseLine.point.x() + formNumber(context, "probeX", 1.5);

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

	return getJoin(context, joinCheck);


}



bool static searchNextReh(SearchContext& context) {

	auto& xSorted = context.xSorted;
	auto& timeSorted = context.timeSorted;

	auto firstPoint = xSorted.end();
	firstPoint--;

	auto firstPointTime = timeSorted.find(*firstPoint);
	auto prevFirstPointTime = firstPointTime;
	bool foundPrev;
	bool foundNext;
	do {
		prevFirstPointTime = context.prev(prevFirstPointTime);
		if (prevFirstPointTime == timeSorted.end()) {
			prevFirstPointTime = --timeSorted.end();
		}
		auto prevYDiff = firstPointTime->point.y() - prevFirstPointTime->point.y();
		foundPrev = prevYDiff > formNumber(context, "minRise", 1.5);
	} while (!foundPrev && prevFirstPointTime != firstPointTime);

	auto nextFirstPointTime = firstPointTime;
	do {
		nextFirstPointTime = std::next(nextFirstPointTime);
		if (nextFirstPointTime == timeSorted.end()) {
			nextFirstPointTime = timeSorted.begin();
		}
		auto nextYDiff = firstPointTime->point.y() - nextFirstPointTime->point.y();
		foundNext = nextYDiff > formNumber(context, "minRise", 1.5);
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

// The free end locates a bowl's own descending-to-rising turn. Probes are
// deferred to cut policy, so changing probe width cannot move this landmark.
struct BowlTurnBody { PathExtrema tip, turn; };
static std::optional<BowlTurnBody> recognizeBowlTurn(const InkOutline& outline) {
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    if (geometry.xSorted.empty()) return {};
    const auto first = std::prev(geometry.xSorted.end());
    auto current = geometry.timeSorted.find(*first);
    const auto start = current;
    const auto step = geometry.prev(current)->point.y() < current->point.y() ? &SearchContext::next : &SearchContext::prev;
    do {
        const auto next = (geometry.*step)(current);
        if (next->point.y() < current->point.y() && (current == start || next->point.y() < start->point.y()))
            return BowlTurnBody{*first, *next};
        current = next;
    } while (current != start);
    return {};
}

static bool cutBowlTurn(SearchContext& context, const BowlTurnBody& body, const QJsonObject& policy) {
    const auto p = body.turn.point;
    const double x = p.x() + policy.value("probeX").toDouble(.2), radius = policy.value("probeRadius").toDouble(2);
    const auto hits = FindInterections(context.transformedPath, QLineF(x, p.y()-radius, x, p.y()+radius));
    if (hits.size() < std::max(2., policy.value("minCrossings").toDouble(2))) return false;
    auto& res = context.res;
    res.bottomBaseLine = hits.end()[-1]; res.topBaseLine = hits.end()[-2];
    res.bottomLeft = res.bottomRight = res.bottomBaseLine;
    res.topLeft = res.topRight = res.topBaseLine;
    res.length = 0;
    context.nextIter = context.xSorted.find(body.tip);
    return true;
}

// Each step scans forward to the first edge satisfying its direction and bounds.
static bool interiorChord(const QPainterPath& ink, const PathExtrema& top, const PathExtrema& bottom,
                          double topTolerance = 0);
static bool strokeNormalCut(const SearchContext& context, Stretching& cut, double offsetX = 0);

// Walk the matched contour step by arc length, retaining the curve parameter
// so the resulting point can be used by the vector cutter.
static std::vector<QBezier> baseContourCurves(const QPainterPath& path) {
    std::vector<QBezier> curves;
    for (int i = 1; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        if (element.isMoveTo()) break;
        const QPointF from = path.elementAt(i - 1);
        if (element.isCurveTo()) {
            curves.push_back(QBezier::fromPoints(from, element, path.elementAt(i + 1), path.elementAt(i + 2)));
            i += 2;
        } else if (element.isLineTo()) {
            const QLineF line(from, element);
            curves.push_back(QBezier::fromPoints(from, line.pointAt(1.0 / 3), line.pointAt(2.0 / 3), element));
        }
    }

    return curves;
}

// Project from a known contour landmark using the tangent on the matched
// step's side. The landmark stays fixed, including at sharp corners.
static bool contourNormalCut(const SearchContext& context, const PathExtrema& anchor,
                             bool increasingTime, bool atStepEnd, Stretching& cut, bool aboveOnly = false) {
    const auto curves = baseContourCurves(context.transformedPath);
    if (curves.empty() || anchor.pathIndex != 0) return false;
    const int count = static_cast<int>(curves.size());
    double time = anchor.time;
    if (std::abs(time - std::round(time)) < 1e-8) time = std::round(time);
    int index = static_cast<int>(std::floor(time));
    double t = time - index;
    if (increasingTime == atStepEnd && t == 0) { --index; t = 1; }
    const auto& curve = curves[(index % count + count) % count];
    const double u = 1 - t;
    auto tangent = u * u * (curve.pt2() - curve.pt1()) +
        2 * u * t * (curve.pt3() - curve.pt2()) + t * t * (curve.pt4() - curve.pt3());
    if (std::hypot(tangent.x(), tangent.y()) < 1e-9)
        tangent = curve.pointAt(std::min(1.0, t + 1e-4)) - curve.pointAt(std::max(0.0, t - 1e-4));
    const double length = std::hypot(tangent.x(), tangent.y());
    if (length < 1e-9) return false;
    const QPointF normal(tangent.y() / length, -tangent.x() / length);
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    double nearest = reach;
    bool found = false;
    for (double side : {-1.0, 1.0}) {
        const auto hits = FindInterections(context.transformedPath,
            QLineF(anchor.point, anchor.point + side * reach * normal), true);
        for (const auto& hit : hits) {
            const double distance = QLineF(anchor.point, hit.point).length();
            if ((aboveOnly && hit.point.y() >= anchor.point.y()) || distance <= 1e-5 || distance >= nearest ||
                !interiorChord(context.transformedPath, anchor, hit)) continue;
            cut.topBaseLine = anchor;
            cut.bottomBaseLine = hit;
            cut.referenceWidth = distance;
            nearest = distance;
            found = true;
        }
    }
    return found;
}

static bool advanceOnContour(const QPainterPath& path, const PathExtrema& start,
                             const PathExtrema& end, bool increasingTime,
                             double distance, PathExtrema& point, QPointF& tangent) {
    if (start.pathIndex != 0 || end.pathIndex != 0 || distance <= 0 ||
        (increasingTime ? end.time <= start.time : end.time >= start.time)) return false;

    const auto curves = baseContourCurves(path);

    const double low = std::min(start.time, end.time);
    const double high = std::max(start.time, end.time);
    const int first = static_cast<int>(std::floor(low));
    const int last = static_cast<int>(std::ceil(high)) - 1;
    const int count = static_cast<int>(curves.size());
    if (!count || high - low > count + 1e-8) return false;

    const int direction = increasingTime ? 1 : -1;
    for (int i = increasingTime ? first : last; i >= first && i <= last; i += direction) {
        const double a = std::max(0.0, low - i);
        const double b = std::min(1.0, high - i);
        const int index = (i % count + count) % count;
        const auto& curve = curves[index];
        const double length = curve.getSubRange(a, b).length(0.001);
        if (distance > length) {
            distance -= length;
            continue;
        }
        double lo = a, hi = b;
        for (int iteration = 0; iteration < 24; ++iteration) {
            const double t = (lo + hi) / 2;
            const double traversed = (increasingTime ? curve.getSubRange(a, t) : curve.getSubRange(t, b)).length(0.001);
            if ((traversed < distance) == increasingTime) lo = t;
            else hi = t;
        }
        const double t = (lo + hi) / 2;
        point = {0, index + t, curve.pointAt(t), false, false};
        const double u = 1 - t;
        tangent = u * u * (curve.pt2() - curve.pt1())
                + 2 * u * t * (curve.pt3() - curve.pt2())
                + t * t * (curve.pt4() - curve.pt3());
        return true;
    }
    return false;
}

// Find where the descending outer edge turns toward the right, then cut
// along the inward normal at that point.
static bool alefTurnCut(const SearchContext& context, const PathExtrema& limit,
                        double angle, double stroke, Stretching& cut,
                        const PathExtrema* crest = nullptr, int traversal = 0, bool oppositeFlank = false) {
    const auto start = crest ? crest->time : std::prev(context.xSorted.end())->time;
    const auto curves = baseContourCurves(context.transformedPath);
    if (curves.empty()) return false;
    const int curveCount = static_cast<int>(curves.size());
    const int direction = traversal ? traversal : limit.time > start ? 1 : -1;
    double finish = limit.time;
    // A stem flank may cross the arbitrary start of the closed SVG contour.
    if (direction > 0 && finish < start) finish += curveCount;
    if (direction < 0 && finish > start) finish -= curveCount;
    const double low = std::min(start, finish);
    const double high = std::max(start, finish);
    const int first = static_cast<int>(std::floor(low));
    const int last = static_cast<int>(std::ceil(high)) - 1;
    if (high - low > curveCount) return false;

    const double radians = angle * M_PI / 180;
    const QPointF target(std::cos(radians), std::sin(radians));
    for (int i = direction > 0 ? first : last; i >= first && i <= last; i += direction) {
        const int curveIndex = (i % curveCount + curveCount) % curveCount;
        const auto& curve = curves[curveIndex];
        const auto tangent = [&](double t) {
            const double u = 1 - t;
            return direction * (u * u * (curve.pt2() - curve.pt1())
                   + 2 * u * t * (curve.pt3() - curve.pt2())
                   + t * t * (curve.pt4() - curve.pt3()));
        };
        const auto offset = [&](double t) {
            const auto v = tangent(t);
            return target.x() * v.y() - target.y() * v.x();
        };
        const double a = std::max(0.0, low - i);
        const double b = std::min(1.0, high - i);
        const double begin = direction > 0 ? a : b;
        const double end = direction > 0 ? b : a;
        double previous = begin;
        for (int sample = 1; sample <= 24; ++sample) {
            const double current = begin + (end - begin) * sample / 24;
            if (offset(previous) > 0 && offset(current) <= 0) {
                double steep = previous, shallow = current;
                for (int iteration = 0; iteration < 24; ++iteration) {
                    const double middle = (steep + shallow) / 2;
                    if (offset(middle) > 0) steep = middle;
                    else shallow = middle;
                }
                const double t = (steep + shallow) / 2;
                const auto v = tangent(t);
                const double length = std::hypot(v.x(), v.y());
                if (v.x() > 0 && v.y() > 0 && length > 1e-8) {
                    const PathExtrema outer{0, curveIndex + t, curve.pointAt(t), false, false};
                    const bool invertNormal = (crest != nullptr) != oppositeFlank;
                    const QPointF normal = (invertNormal ? -1 : 1) * QPointF(v.y() / length, -v.x() / length);
                    const auto hits = FindInterections(context.transformedPath,
                        QLineF(outer.point, outer.point + normal * (3 * stroke)), true);
                    for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit) {
                        const auto& top = crest ? outer : *hit;
                        const auto& bottom = crest ? *hit : outer;
                        if (!interiorChord(context.transformedPath, top, bottom)) continue;
                        cut.topBaseLine = top;
                        cut.bottomBaseLine = bottom;
                        return true;
                    }
                }
            }
            previous = current;
        }
    }
    return false;
}

// A lam-alef has a shared corner-pair boundary. Initial and attached forms
// have different lower returns, described by their own neighbor constraints.
static bool searchLamAlefLiga(SearchContext& context) {
    const bool attached = context.form.value("lamPosition").toString("initial") == "medial";
    if (context.baseText.size() < 2 || (context.baseText.size() > 2) != attached ||
        context.baseText[context.baseText.size() - 2] != QChar(0x0644) ||
        !QStringLiteral("اآأإٱ").contains(context.baseText.back())) return false;
    JoinCheck check;
    check.bottomCheck = false;
    check.sameDirectionCheck = context.form.value("sameDirectionCheck").toBool(true);
    check.customCheck = [&](xsortedtype::iterator) {
        const auto& cut = context.res;
        const auto& box = context.bbox;
        const double x = cut.topBaseLine.point.x();
        auto crossings = FindInterections(context.transformedPath,
            QLineF(x, box.top(), x, box.bottom()));
        // This probe passes through a known contour landmark. At a Bezier
        // endpoint the root solver can round both adjoining roots just outside
        // [0, 1], dropping this hit. Count the exact landmark once, whether or
        // not the solver returned it, without weakening the crossing test.
        if (std::none_of(crossings.begin(), crossings.end(), [&](const auto& hit) {
                return QLineF(hit.point, cut.topBaseLine.point).length() <= 1e-5;
            })) crossings.push_back(cut.topBaseLine);
        return crossings.size() >= formNumber(context, "minCrossings", 4) &&
            interiorChord(context.transformedPath, cut.topBaseLine, cut.bottomBaseLine);
    };
    return getJoin(context, check);
}

static bool finalAlefStemCut(const SearchContext& context, const QJsonObject& form, Stretching& result) {
    const auto start = std::prev(context.xSorted.end());
    const auto& ink = context.transformedPath;
    const auto box = ink.boundingRect();
    const double y = start->point.y() + box.height() * 0.25;
    const auto crossSection = [&](double probeY) {
        auto hits = FindInterections(ink, QLineF(box.left() - 1, probeY, box.right() + 1, probeY));
        std::sort(hits.begin(), hits.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.point.x() < rhs.point.x();
        });
        return hits;
    };
    const auto stem = crossSection(y);
    if (stem.size() < 2 || stem[0].pathIndex != 0 || stem[1].pathIndex != 0 ||
        !interiorChord(ink, stem[0], stem[1])) return false;
    const double stroke = stem[1].point.x() - stem[0].point.x();
    if (stroke <= 0 || box.height() < stroke * 5) return false;

    std::optional<Stretching> best;
    for (const auto& limit : context.timeSorted) {
        if (limit.point.y() < y || limit.point.x() <= start->point.x()) continue;
        Stretching cut{};
        if (!alefTurnCut(context, limit, form.value("turnAngle").toDouble(45), stroke, cut)) continue;
        const QLineF chord(cut.topBaseLine.point, cut.bottomBaseLine.point);
        if (chord.center().x() - box.left() > 4 * stroke || chord.center().y() < y ||
            chord.length() < 0.5 * stroke || chord.length() > 2.5 * stroke) continue;
        if (!best || cut.bottomBaseLine.point.x() < best->bottomBaseLine.point.x()) best = cut;
    }
    if (!best && form.value("stemWidthLimit").isDouble()) {
        // Straight stems can meet a foot without an eligible diagonal normal.
        // Use the last narrow cross-section before the left stem widens.
        const double step = stroke / 5;
        const double maxWidth = stroke * form.value("stemWidthLimit").toDouble();
        std::optional<Stretching> narrow;
        for (double probeY = y; probeY < box.bottom(); probeY += step) {
            const auto hits = crossSection(probeY);
            if (hits.size() < 2) break;
            const auto& left = hits[0];
            const auto& right = hits[1];
            const double width = right.point.x() - left.point.x();
            if (width > maxWidth) {
                if (narrow && probeY - y >= stroke * 3) best = narrow;
                break;
            }
            if (left.point.x() - box.left() > 4 * stroke || width < 0.5 * stroke ||
                !interiorChord(ink, right, left)) break;
            narrow = Stretching{};
            narrow->topBaseLine = right;
            narrow->bottomBaseLine = left;
        }
    }
    if (!best) return false;
    result = *best;
    result.topLeft = result.topRight = result.topBaseLine;
    result.bottomLeft = result.bottomRight = result.bottomBaseLine;
    return true;
}

static std::optional<PathExtrema> horizontalOpposite(const SearchContext& context,
                                                     const PathExtrema& anchor, bool left) {
    const double edge = left ? context.bbox.left() - 1 : context.bbox.right() + 1;
    const auto hits = FindInterections(context.transformedPath,
        QLineF(anchor.point, QPointF(edge, anchor.point.y())), true);
    std::optional<PathExtrema> opposite;
    double nearest = context.bbox.width() + 1;
    for (const auto& hit : hits) {
        const double distance = (left ? -1 : 1) * (hit.point.x() - anchor.point.x());
        if (distance <= 1e-5 || distance >= nearest || !interiorChord(context.transformedPath, anchor, hit)) continue;
        opposite = hit;
        nearest = distance;
    }
    return opposite;
}

struct ForkRefinement {
    Stretching originalExit;
    PathExtrema corner;
};

static std::vector<LamForkBody> recognizeLamForkBodies(const SearchContext& context, double flankSpan) {
    std::vector<LamForkBody> bodies;
    for (auto tip = context.timeSorted.begin(); tip != context.timeSorted.end(); ++tip) {
        const auto flank = [&](bool previous) {
            auto at = previous ? context.prev(tip) : context.next(tip);
            const bool tiny = QLineF(at->point, tip->point).length() < flankSpan;
            for (size_t n = 0; n + 1 < context.timeSorted.size() &&
                    QLineF(at->point, tip->point).length() < flankSpan; ++n)
                at = previous ? context.prev(at) : context.next(at);
            if (tiny && at->point.y() <= tip->point.y()) {
                // Look past short contour pieces, but not past the descending
                // flank itself. A distant landmark can already be rising into
                // another letter; sample the local curve in that case.
                auto limit = *tip;
                limit.time += (previous ? 1 : -1) * int(baseContourCurves(context.transformedPath).size());
                PathExtrema sample;
                QPointF tangent;
                if (advanceOnContour(context.transformedPath, *tip, limit, previous, flankSpan, sample, tangent))
                    return sample.point - tip->point;
            }
            return at->point - tip->point;
        };
        // Short contour pieces and duplicate extrema can subdivide a flank.
        // Recognize the two branches across that noise, keeping the actual tip.
        const auto a = flank(true), b = flank(false);
        if (a.y() <= 0 || b.y() <= 0) continue;
        // The fork is a concave turn of the outer contour, not the convex
        // top of another stroke. Use winding instead of a fragile ink probe
        // just beside a nearly tangent Bezier corner.
        const double turn = a.x() * b.y() - a.y() * b.x();
        if (std::abs(turn) <= 1e-8) continue;
        const bool fork = a.x() * b.x() < 0;
        // Keep the intrinsic turn sign. A recovered/scaled outline can resolve
        // a degenerate winding estimate differently; that must not require a
        // second landmark search or discard the real fork from the inventory.
        bodies.push_back({*tip, a, b, a.x() > b.x(), fork, turn > 0});
    }
    return bodies;
}

// Cache intrinsic body evidence independently of route and cut-policy alternatives.
static LamBodyEvidence recognizeLamBodies(const SearchContext& context, double flankSpan) {
    LamBodyEvidence result;
    result.forks = recognizeLamForkBodies(context, flankSpan);
    for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest)
        if (crest->point.y() < context.prev(crest)->point.y() &&
            crest->point.y() < context.next(crest)->point.y()) result.crests.push_back(*crest);
    return result;
}

// The lower contour of a forked lam rises to a tip under the stem. Follow
// each flank away from that tip and cut across the same strip of ink.
static std::optional<Stretching> cutLamForkFlank(const SearchContext& context, const PathExtrema& tip,
                                               std::optional<PathExtrema> end, bool forward, int side, double distance) {
    auto anchor = tip;
    if (distance > 0) {
        auto limit = end.value_or(tip);
        if (end && limit.time == tip.time) return {};
        if (!end || forward != (limit.time > tip.time))
            limit.time += (forward ? 1 : -1) * int(baseContourCurves(context.transformedPath).size());
        QPointF tangent;
        if (!advanceOnContour(context.transformedPath, tip, limit, forward, distance, anchor, tangent)) return {};
    }
    Stretching cut{};
    if (!contourNormalCut(context, anchor, forward, false, cut, distance == 0)) return {};
    std::swap(cut.topBaseLine, cut.bottomBaseLine);
    if (distance == 0 && side * (cut.topBaseLine.point.x() - tip.point.x()) <= 0) return {};
    return cut;
}

static bool lamAnchorWithinWalk(const SearchContext& context, const PathExtrema& tip,
                               const Stretching& cut, bool forward, double distance) {
    auto end = cut.bottomBaseLine;
    if (end.pathIndex != 0 || QLineF(tip.point, end.point).length() > distance + 1e-5) return false;
    if (QLineF(tip.point, end.point).length() < 1e-5) return true;
    const double count = baseContourCurves(context.transformedPath).size();
    if (count <= 0) return false;
    if (forward != (end.time > tip.time)) end.time += (forward ? 1 : -1) * count;
    PathExtrema point;
    QPointF tangent;
    return !advanceOnContour(context.transformedPath, tip, end, forward, distance + 1e-5, point, tangent);
}

// Follow only the adjacent lower flank of the same fork. Its intersection at
// the exit's height fixes the lower endpoint; the local tangent fixes the cut.
static std::optional<Stretching> levelForkEntry(SearchContext& context, const PathExtrema& corner,
                                               const Stretching& exit) {
    const auto level = exit.bottomBaseLine.point.y();
    const auto start = context.timeSorted.find(corner);
    if (start == context.timeSorted.end() || level <= corner.point.y() ||
        exit.bottomBaseLine.point.x() >= corner.point.x()) return {};
    for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
        auto current = start;
        for (size_t i = 0; i < context.timeSorted.size(); ++i) {
            const auto next = (context.*advance)(current);
            if (next->point.x() < current->point.x() || next->point.y() < current->point.y()) break;
            if (next->point.y() >= level) {
                const auto hits = FindInterections(context.transformedPath,
                    QLineF(current->point.x(), level, next->point.x(), level), true);
                // Time increases for prev() in the descending contour set.
                const bool increasing = advance == &SearchContext::prev;
                const double lo = increasing ? current->time : next->time;
                const double hi = increasing ? next->time : current->time;
                for (const auto& hit : hits) {
                    const bool onArc = lo <= hi ? lo <= hit.time && hit.time <= hi : hit.time >= lo || hit.time <= hi;
                    if (hit.pathIndex != 0 || !onArc || hit.point.x() <= corner.point.x()) continue;
                    Stretching entry{};
                    if (!contourNormalCut(context, hit, increasing, false, entry, true) ||
                        entry.bottomBaseLine.point.x() <= corner.point.x()) continue;
                    std::swap(entry.topBaseLine, entry.bottomBaseLine);
                    return entry;
                }
                break;
            }
            current = next;
        }
    }
    return {};
}

// Landmark recognition and cut projection share contour arithmetic, but not
// state: a route can be recognized once and cut with different policies.
using ContourAdvance = timesortedtype::iterator (SearchContext::*)(timesortedtype::iterator) const;

static bool walkContourSteps(SearchContext& context, timesortedtype::iterator origin,
        const QJsonArray& steps, ContourAdvance advance,
        PathExtrema& anchor, PathExtrema& before, PathExtrema& end) {
    const auto matchesLandmark = [&](const QJsonObject& features, const PathExtrema& point) {
        if (features.contains("xMinimum")) {
            const auto at = context.timeSorted.find(point);
            if (at == context.timeSorted.end()) return false;
            const double x = point.point.x(), before = context.prev(at)->point.x(), after = context.next(at)->point.x();
            // A zero Bezier derivative can be reported as an extremum without
            // reversing direction. Check both flanks to identify a true turn.
            const bool minimum = x <= before + 1e-8 && x <= after + 1e-8 &&
                (x < before - 1e-8 || x < after - 1e-8);
            if (minimum != features.value("xMinimum").toBool()) return false;
        }
        return (!features.contains("sharp") || features.value("sharp").toBool() == point.isSharp) &&
            (!features.contains("xExtremum") || features.value("xExtremum").toBool() == point.isX) &&
            (!features.contains("yExtremum") || features.value("yExtremum").toBool() == point.isY);
    };

    if (steps.isEmpty()) return false;
    auto current = origin;
    int remaining = static_cast<int>(context.timeSorted.size());
    for (int index = 0; index < steps.size(); ++index) {
        auto bounds = steps[index].toObject();
        if (steps[index].isString()) bounds.insert("direction", steps[index]);
        const bool accumulate = bounds.value("accumulate").toBool();
        const QJsonObject direction{{"direction", bounds.value("direction")}};
        auto runStart = current;
        bool found = false;
        while (remaining-- > 0) {
            const auto next = (context.*advance)(current);
            const auto delta = next->point - current->point;
            const bool continues = !accumulate || matchesOffset(direction, delta);
            if (!continues) runStart = next;
            // Extrema can subdivide one continuous flank. Measure cumulative
            // bounds over that run without stepping across a direction reversal.
            if (continues && matchesLandmark(bounds.value("start").toObject(), *current) &&
                matchesLandmark(bounds.value("end").toObject(), *next) && matchesOffset(bounds,
                accumulate ? next->point - runStart->point : delta)) {
                if (index == steps.size() - 1) {
                    anchor = *current;
                    end = *next;
                }
                if (index != steps.size() - 1) before = *current;
                current = next;
                found = true;
                break;
            }
            // A required adjacent flank must start at the matched turn.
            if (bounds.value("immediate").toBool()) return false;
            before = *current;
            current = next;
        }
        if (!found) return false;
    }
    return true;
}

struct ContourRouteEvidence {
    xsortedtype::iterator start;
    timesortedtype::iterator origin;
    ContourAdvance forward, topDirection;
    PathExtrema anchor{}, topBefore{}, topEnd{};
    double endpointTolerance = 0;
};

static std::optional<ContourRouteEvidence> recognizeContourRoute(SearchContext& context,
        const QJsonObject& form, const PathExtrema* crest = nullptr, bool reverseTop = false) {
    const auto start = form.value("start").toString() == "left" ?
        std::prev(context.xSorted.end()) : context.xSorted.begin();
    const auto origin = context.timeSorted.find(crest ? *crest :
        form.value("start") == "top" ? *std::prev(context.ySorted.end()) : *start);
    auto forward = context.orientation == Orientation::ClockWise ? &SearchContext::prev : &SearchContext::next;
    if (form.value("traversal").toString() == "down") {
        forward = context.prev(origin)->point.y() < origin->point.y() ? &SearchContext::next : &SearchContext::prev;
    }
    const auto backward = forward == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
    Stretching join{};
    PathExtrema topEnd{}, topBefore{};
    const auto topDirection = (form.value("topTraversal") == "forward") != reverseTop ? forward : backward;
    if (form.value("topAtAnchor").toBool()) join.topBaseLine = topEnd = *origin;
    else if (!walkContourSteps(context, origin, form.value("top").toArray(), topDirection, join.topBaseLine, topBefore, topEnd)) return {};
    if (form.value("topUseStepEnd").toBool()) join.topBaseLine = topEnd;
    const auto snap = form.value("snapCurveStart");
    auto& anchor = join.topBaseLine;
    const int curve = static_cast<int>(std::floor(anchor.time));
    if (snap.isDouble() && curve >= 0 && anchor.time - curve <= snap.toDouble()) {
        int index = 0;
        const auto& path = context.transformedPath;
        for (int element = 1; element < path.elementCount(); ++element) {
            const auto current = path.elementAt(element);
            if (current.isMoveTo()) return {};
            if (index++ == curve) {
                anchor.time = curve;
                anchor.point = path.elementAt(element - 1);
                break;
            }
            if (current.isCurveTo()) element += 2;
        }
        if (anchor.time != curve) return {};
        join.topEndpointTolerance = form.value("snapEndpointTolerance").toDouble();
    }
    return ContourRouteEvidence{start, origin, forward, topDirection,
        join.topBaseLine, topBefore, topEnd, join.topEndpointTolerance};
}

static bool projectContourRoute(SearchContext& context, const QJsonObject& form,
        const ContourRouteEvidence& evidence, std::optional<ForkRefinement>* fork = nullptr) {
    const auto& [start, origin, forward, topDirection, anchor, topBefore, topEnd, tolerance] = evidence;
    auto& join = context.res;
    join.topBaseLine = anchor;
    join.topEndpointTolerance = tolerance;
    PathExtrema bottomEnd{}, bottomBefore{};
    const auto probeMode = form.value("bottomProbe").toString();
    if (probeMode == "walk-stroke-up" || probeMode == "down-stroke" || probeMode == "walk-stroke-normal") {
        const auto& ink = context.transformedPath;
        QPointF tangent;
        double stroke = form.value("penWidth").toDouble(context.pairedStrokeWidth);
        if (form.value("strokeSource") == "local") {
            Stretching local{};
            if (!contourNormalCut(context, join.topBaseLine, topDirection == &SearchContext::prev, false, local)) return false;
            stroke = QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length();
        }
        if (!advanceOnContour(ink, join.topBaseLine, topEnd, topDirection == &SearchContext::prev,
                              stroke, join.bottomBaseLine, tangent)) return false;
        const auto bottom = join.bottomBaseLine.point;
        QPointF direction(0, -1);
        if (probeMode == "walk-stroke-normal") {
            const double length = std::hypot(tangent.x(), tangent.y());
            if (length < 1e-8) return false;
            direction = QPointF(-tangent.y(), tangent.x()) / length;
            const double step = stroke * 0.02;
            if (!ink.contains(bottom + step * direction)) direction = -direction;
            if (!ink.contains(bottom + step * direction)) return false;
        }
        const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
        const auto hits = FindInterections(ink, QLineF(bottom, bottom + reach * direction), true);
        auto top = hits.end();
        double nearest = reach;
        for (auto hit = hits.begin(); hit != hits.end(); ++hit) {
            const double distance = QPointF::dotProduct(hit->point - bottom, direction);
            if (distance <= 1e-5 || distance >= nearest) continue;
            nearest = distance;
            top = hit;
        }
        if (top == hits.end() || !interiorChord(ink, *top, join.bottomBaseLine)) return false;
        join.topBaseLine = *top;
    } else if (probeMode == "corner") {
        // Pair an intrinsic shoulder with the nearest opposing sharp corner
        // across ink. Neither a neighboring cut nor its stroke width is used.
        const auto anchor = join.topBaseLine;
        const auto bounds = form.value("oppositeCorner").toObject();
        double nearest = std::numeric_limits<double>::infinity();
        std::optional<PathExtrema> opposite;
        for (auto it = context.timeSorted.begin(); it != context.timeSorted.end(); ++it) {
            const auto& point = *it;
            const double distance = QLineF(anchor.point, point.point).length();
            if (point.isSharp != bounds.value("sharp").toBool(true) ||
                (bounds.contains("xExtremum") && point.isX != bounds.value("xExtremum").toBool()) ||
                (bounds.contains("yExtremum") && point.isY != bounds.value("yExtremum").toBool()) ||
                distance <= 1e-5 || distance >= nearest ||
                !matchesOffset(bounds, point.point - anchor.point) ||
                !matchesOffset(bounds.value("flanks").toObject(), (context.prev(it)->point - point.point) / distance) ||
                !matchesOffset(bounds.value("flanks").toObject(), (context.next(it)->point - point.point) / distance) ||
                !interiorChord(context.transformedPath, anchor, point)) continue;
            opposite = point;
            nearest = distance;
        }
        if (!opposite) return false;
        join.bottomBaseLine = *opposite;
        join.referenceWidth = nearest;
        join.fixedRightBoundary = true;
    } else if (probeMode == "normal") {
        if (!contourNormalCut(context, join.topBaseLine, topDirection == &SearchContext::prev,
                              form.value("topUseStepEnd").toBool(), join)) return false;
        join.fixedRightBoundary = true;
    } else if (probeMode == "up") {
        const auto bottom = join.topBaseLine;
        const auto hits = FindInterections(context.transformedPath,
            QLineF(bottom.point, QPointF(bottom.point.x(), context.bbox.top() - 1)), true);
        std::optional<PathExtrema> top;
        double distance = context.bbox.height() + 1;
        for (const auto& hit : hits) {
            const double rise = bottom.point.y() - hit.point.y();
            if (rise <= 1e-5 || rise >= distance || !interiorChord(context.transformedPath, hit, bottom)) continue;
            top = hit;
            distance = rise;
        }
        if (!top) return false;
        join.topBaseLine = *top;
        join.bottomBaseLine = bottom;
        join.fixedRightBoundary = true;
    } else if (probeMode == "below") {
        const auto anchor = join.topBaseLine;
        const auto hits = FindInterections(context.transformedPath,
            QLineF(anchor.point, QPointF(anchor.point.x(), context.bbox.bottom() + 1)), true);
        bool found = false;
        for (size_t i = 1; i < hits.size(); ++i) {
            if (hits[i - 1].point.y() < anchor.point.y() - 1e-5 ||
                !interiorChord(context.transformedPath, hits[i - 1], hits[i])) continue;
            join.topBaseLine = hits[i - 1];
            join.bottomBaseLine = hits[i];
            found = true;
            break;
        }
        if (!found) return false;
        join.fixedRightBoundary = true;
    } else if (probeMode == "left" || probeMode == "right") {
        const auto opposite = horizontalOpposite(context, join.topBaseLine, probeMode == "left");
        if (!opposite) return false;
        join.bottomBaseLine = *opposite;
        join.fixedRightBoundary = true;
    } else if (form.value("bottomProbe") == "down") {
        const auto top = join.topBaseLine.point;
        const QLineF probe(top, QPointF(top.x(), context.bbox.bottom() + 1));
        const auto hits = FindInterections(context.transformedPath, probe, true);
        const auto bottom = std::find_if(hits.begin(), hits.end(), [&](const PathExtrema& hit) {
            return hit.point.y() > top.y() + std::max(1e-5, join.topEndpointTolerance);
        });
        if (bottom == hits.end() || !interiorChord(context.transformedPath, join.topBaseLine, *bottom, join.topEndpointTolerance))
            return false;
        join.bottomBaseLine = *bottom;
        join.fixedRightBoundary = true;
    } else if (!walkContourSteps(context, origin, form.value("bottom").toArray(), forward, join.bottomBaseLine, bottomBefore, bottomEnd)) {
        return false;
    }
    if (form.contains("visibleFrom") && !interiorChord(context.transformedPath, join.topBaseLine, join.bottomBaseLine)) {
        // A folded rim can cross the chord between the two walk landmarks.
        // Keep the uninterrupted ink segment visible from the specified flank.
        const bool fromBottom = form.value("visibleFrom") == "bottom";
        const auto& fixed = fromBottom ? join.bottomBaseLine : join.topBaseLine;
        auto& other = fromBottom ? join.topBaseLine : join.bottomBaseLine;
        const QLineF chord(fixed.point, other.point);
        const auto hits = FindInterections(context.transformedPath, chord, true);
        const auto visible = std::find_if(hits.begin(), hits.end(), [&](const PathExtrema& hit) {
            return interiorChord(context.transformedPath, fixed, hit);
        });
        if (visible == hits.end()) return false;
        other = *visible;
    }
    if (form.value("walkNormalOffset").isDouble() && form.value("walkNormalOffset").toDouble() > 0) {
        const auto original = join;
        const double width = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).length();
        auto limit = topEnd;
        if (form.value("topUseStepEnd").toBool()) {
            // The offset starts at the walk's endpoint. Continue along the
            // contour, across subdivisions (and its arbitrary SVG start),
            // rather than stopping at a possibly adjacent micro-extremum.
            limit = join.topBaseLine;
            limit.time += (topDirection == &SearchContext::prev ? 1 : -1) *
                int(baseContourCurves(context.transformedPath).size());
        }
        PathExtrema anchor{}; QPointF tangent;
        if (!advanceOnContour(context.transformedPath, join.topBaseLine, limit, topDirection == &SearchContext::prev,
                width * form.value("walkNormalOffset").toDouble(), anchor, tangent) ||
            !contourNormalCut(context, anchor, topDirection == &SearchContext::prev, false, join)) return false;
        if (form.contains("forkSide")) {
            const int side = form.value("forkSide") == "left" ? -1 : 1;
            const double originX = original.topBaseLine.point.x();
            const auto onSide = [&](const PathExtrema& point) {
                return side * (point.point.x() - originX) >= -1e-8;
            };
            // A slanted normal can legitimately swing across the probe. Only
            // correct it if it went past the upward-pointing fork of the lower outline.
            const auto from = getJoinTime(original, context.orientation);
            const auto to = getJoinTime(join, context.orientation);
            const auto lower = join.bottomBaseLine.point;
            if (onSide(anchor) && !onSide(join.bottomBaseLine)) {
                const auto corner = std::find_if(context.timeSorted.begin(), context.timeSorted.end(), [&](const PathExtrema& p) {
                    return p.isSharp && p.isY && (p.point.x()-originX)*(p.point.x()-lower.x()) < 0 &&
                        p.point.y() >= original.topBaseLine.point.y() &&
                        p.point.y() < std::min(original.bottomBaseLine.point.y(), lower.y()) &&
                        from.isStrictLeft(p.time) != to.isStrictLeft(p.time);
                });
                if (corner != context.timeSorted.end()) {
                    if (fork) *fork = ForkRefinement{join, *corner};
                    join = original;
                    if (!strokeNormalCut(context, join, side * form.value("walkNormalOffset").toDouble()) ||
                        !onSide(join.topBaseLine) || !onSide(join.bottomBaseLine)) return false;
                }
            }
        }
        join.fixedRightBoundary = true;
    }
    if (form.value("normalCut").toBool() &&
        !strokeNormalCut(context, join, form.value("strokeOffsetX").toDouble())) return false;
    if (form.value("anchorSide") == "bottom") std::swap(join.topBaseLine, join.bottomBaseLine);
    const bool span = form.value("span").toBool() && !join.fixedRightBoundary;
    join.topRight = span ? topBefore : join.topBaseLine;
    join.bottomRight = span ? bottomBefore : join.bottomBaseLine;
    join.topLeft = span ? topEnd : join.topBaseLine;
    join.bottomLeft = span ? bottomEnd : join.bottomBaseLine;
    join.length = span ? std::min(join.topRight.point.x(), join.bottomRight.point.x()) -
                         std::max(join.topLeft.point.x(), join.bottomLeft.point.x()) : 0;
    context.nextIter = start;
    return true;
}

// Adapter for forms which have not moved to body/cut declarations.
static bool searchContourWalk(SearchContext& context, const PathExtrema* crest = nullptr,
                              bool reverseTop = false, bool anchorOnly = false,
                              std::optional<ForkRefinement>* fork = nullptr) {
    const auto evidence = recognizeContourRoute(context, context.form, crest, reverseTop);
    if (!evidence) return false;
    if (anchorOnly) {
        context.res.topBaseLine = evidence->anchor;
        context.res.topEndpointTolerance = evidence->endpointTolerance;
        return true;
    }
    return projectContourRoute(context, context.form, *evidence, fork);
}

// Lām uses the independent route evidence directly. No method dispatcher or
// legacy form is reconstructed from the declarations.
static bool lamRouteCut(SearchContext& context, const QJsonObject& bodyRoute,
        const QJsonObject& cutPolicy, const PathExtrema* crest = nullptr,
        bool reverse = false, std::optional<ForkRefinement>* fork = nullptr) {
    const auto evidence = recognizeContourRoute(context, bodyRoute, crest, reverse);
    if (!evidence) {
        inspectReason("Body", "The contour did not satisfy this route's directions and constraints.");
        return false;
    }
    inspectLandmark("Route origin", *evidence->origin);
    inspectLandmark("Route anchor", evidence->anchor);
    const bool projected = projectContourRoute(context, withOverrides(bodyRoute, cutPolicy), *evidence, fork);
    if (!projected) inspectReason("Cuts", "Route recognized; its probe or normal did not produce a valid cross-section.");
    return projected;
}

// A route is saved by value, so recognition can run before any neighboring
// boundary exists. Projection rebinds these landmarks to the same source.
struct SavedContourRoute {
    PathExtrema start, origin, anchor, topBefore, topEnd;
    bool forwardPrevious, topPrevious;
    double endpointTolerance;
};

static std::optional<SavedContourRoute> recognizeSavedRoute(SearchContext& geometry,
        const QJsonObject& route, const PathExtrema* crest = nullptr, bool reverse = false) {
    const auto found = recognizeContourRoute(geometry, route, crest, reverse);
    if (!found) return {};
    return SavedContourRoute{*found->start, *found->origin, found->anchor, found->topBefore, found->topEnd,
        found->forward == &SearchContext::prev, found->topDirection == &SearchContext::prev,
        found->endpointTolerance};
}

static bool projectSavedRoute(SearchContext& context, const std::optional<SavedContourRoute>& stored,
        const QJsonObject& route, const QJsonObject& policy, std::optional<ForkRefinement>* fork = nullptr) {
    if (!stored) {
        inspectReason("Body", "The contour did not satisfy this route's directions and constraints.");
        return false;
    }
    const auto& r = *stored;
    const auto start = context.xSorted.find(r.start);
    const auto origin = context.timeSorted.find(r.origin);
    if (start == context.xSorted.end() || origin == context.timeSorted.end()) return false;
    const ContourRouteEvidence evidence{start, origin,
        r.forwardPrevious ? &SearchContext::prev : &SearchContext::next,
        r.topPrevious ? &SearchContext::prev : &SearchContext::next,
        r.anchor, r.topBefore, r.topEnd, r.endpointTolerance};
    const bool projected = projectContourRoute(context, withOverrides(route, policy), evidence, fork);
    if (!projected) inspectReason("Cuts", "Route recognized; its probe or normal did not produce a valid cross-section.");
    return projected;
}

bool static searchKaf(SearchContext& context) {

	JoinCheck joinCheck;

	joinCheck.customCheck = [&context](xsortedtype::iterator currentIter) {

		//Test if intersects with shape below



		auto& bbox = context.bbox;

		auto& res = context.res;

		auto& subPath = context.transformedPath;

		auto atop = res.bottomBaseLine.point.y() + formNumber(context, "probeY", 0.1);
		auto height = bbox.bottom() - atop;
		if (height > 0) {
			QRectF rec{ res.bottomBaseLine.point.x(),atop,formNumber(context, "probeWidth", 1),height };
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


bool static searchHah(SearchContext& context) {

	auto startIter = context.nextIter;

	auto prevJoinTime = getJoinTime(context);

	JoinCheck joinCheck;

	joinCheck.customCheck = [&context, &startIter, &prevJoinTime](xsortedtype::iterator currentIter) {

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

		if (yss.size() < formNumber(context, "minExtrema", 8)) {
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

	QLineF line{ {x - formNumber(context, "ascenderProbeX", 6) ,context.bbox.top()}, {x - formNumber(context, "ascenderProbeX", 6),context.bbox.bottom()} };

	auto intersections = FindInterections(context.transformedPath, line);

	if (intersections.size() >= 2 && intersections.size() == formNumber(context, "ascenderCrossings", 4)) {
		std::sort(intersections.begin(), intersections.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() < b.point.y(); });
		for (auto& subPath : context.subPaths) {
			if (subPath.contains(intersections[1].point)) {
				auto x = subPath.boundingRect().left() - formNumber(context, "counterProbeX", 0.6);

				line = { {x  ,context.bbox.top()}, {x,context.bbox.bottom()} };

				auto intersections = FindInterections(context.transformedPath, line);
				if (intersections.size() >= 2 && intersections.size() == formNumber(context, "counterCrossings", 2)) {
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

auto lambdaPrev = [](timesortedtype::iterator it, typename std::iterator_traits<timesortedtype::iterator>::difference_type n) {
      return std::prev(it, n);
    };

	auto lambdaNext = [](timesortedtype::iterator it, typename std::iterator_traits<timesortedtype::iterator>::difference_type n) {
      return std::next(it, n);
    };

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

	auto nextFunc = res.clockWise ? lambdaPrev: lambdaNext;

	auto item = context.timeSorted.find(res.topBaseLine);
	if (item == context.timeSorted.end()) return false;
	auto prev = item;
	auto advance = [&](timesortedtype::iterator it) {
		return res.clockWise && it == context.timeSorted.begin() ? context.timeSorted.end() : nextFunc(it, 1);
	};
	auto curr = advance(item);
	while (curr != context.timeSorted.end() && (curr->point.y() <= prev->point.y())) {
		prev = curr;
		curr = advance(curr);
	}
	if (curr == context.timeSorted.end()) return false;
	PathExtrema extremum = *prev;

	auto tentative = context.form.value("lookahead").toInt(14);
	curr = advance(curr);
	while (curr != context.timeSorted.end() && !(!curr->isSharp && curr->isY) && tentative != 0) {
		curr = advance(curr);
		tentative--;
	}

	double x;

	if (curr != context.timeSorted.end() && tentative != 0) {
		x = curr->point.x();
	}
	else {
		x = extremum.point.x() + formNumber(context, "probeX", 1.2);
	}

	auto bbox = context.transformedPath.boundingRect();

	QLineF line{ {x ,bbox.top()}, {x ,bbox.bottom()} };

	auto intersections = FindInterections(context.transformedPath, line);

	if (intersections.size() < std::max(2.0, formNumber(context, "minCrossings", 2))) {
		return false;
	}

	std::sort(intersections.begin(), intersections.end(), [](const PathExtrema& a, const PathExtrema& b) { return a.point.y() < b.point.y(); });

	context.res.bottomBaseLine = intersections[1];
	context.res.topBaseLine = intersections[0];

	context.res.length = 0;

	return true;

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
static std::vector<GlyphInfo> cutRegions(const QPainterPath& path, const QString& text,
                                         const std::vector<Stretching>& cuts, Orientation orientation,
                                         bool expanded = false) {
    std::vector<GlyphInfo> regions;
    if (cuts.empty()) return {{text, path}};
    // Both contour sides must agree on reading order. Testing only whether
    // the next endpoints lie to the left accepts a reversed chord whose
    // "body" wraps around the rest of the subword and overlaps every region.
    for (size_t i = 1; i < cuts.size(); ++i) {
        const auto& right = cuts[i - 1];
        const auto& left = cuts[i];
        const auto side = getJoinTime(left, orientation);
        if (!cutPrecedes(right, left, orientation) || !side.isStrictRight(right.topBaseLine.time) ||
            (!side.isStrictRight(right.bottomBaseLine.time) && !sharesLowerForkTip(right, left)))
            return {};
    }

	auto subPaths = toSubpaths(path);

	if (subPaths.isEmpty()) return {};
	subPaths.remove(0);

    const bool hasCounterCut = std::any_of(cuts.begin(), cuts.end(), [](const auto& cut) {
        return cut.counterEntry.pathIndex > 0;
    });
    const auto outerCurves = hasCounterCut ? baseContourCurves(path) : std::vector<QBezier>{};
    const auto pointAt = [](const std::vector<QBezier>& curves, double time) {
        const int index = static_cast<int>(std::floor(time));
        return curves[index % curves.size()].pointAt(time - index);
    };
    // Replace a straight closing chord by the two short ink bridges and the
    // intervening counter arc. Preserve the original cubic curves exactly.
    const auto connectTo = [&](QPainterPath& region, const QPointF& end) {
        if (hasCounterCut) for (const auto& cut : cuts) {
            const int counter = cut.counterEntry.pathIndex;
            if (counter <= 0 || counter != cut.counterExit.pathIndex || counter > subPaths.size()) continue;
            const auto a = pointAt(outerCurves, cut.topBaseLine.time);
            const auto b = pointAt(outerCurves, cut.bottomBaseLine.time);
            const bool fromA = QLineF(region.currentPosition(), a).length() < 1e-6 && QLineF(end, b).length() < 1e-6;
            const bool fromB = QLineF(region.currentPosition(), b).length() < 1e-6 && QLineF(end, a).length() < 1e-6;
            if (!fromA && !fromB) continue;
            const auto curves = baseContourCurves(subPaths[counter - 1]);
            const double start = fromA ? cut.counterEntry.time : cut.counterExit.time;
            double finish = fromA ? cut.counterExit.time : cut.counterEntry.time;
            if (finish <= start) finish += curves.size();
            const auto bend=cut.counterBendPoint(a,pointAt(curves,cut.counterEntry.time),pointAt(curves,cut.counterExit.time));
            if (fromA && !cut.counterBendWeights.isNull()) region.lineTo(bend);
            region.lineTo(pointAt(curves, start));
            for (double time = start; time < finish - 1e-10;) {
                const int index = static_cast<int>(std::floor(time));
                const double next = std::min(finish, double(index + 1));
                const auto arc = curves[index % curves.size()].getSubRange(time - index, next - index);
                region.cubicTo(arc.pt2(), arc.pt3(), arc.pt4());
                time = next;
            }
            if (fromB && !cut.counterBendWeights.isNull()) region.lineTo(bend);
            region.lineTo(end);
            return;
        }
        region.lineTo(end);
    };

	auto currentJoin = cuts.begin();

	for (int joinIndex = 0; joinIndex <= cuts.size(); joinIndex++) {

		JoinTime leftJointTime;
		JoinTime rightJointTime;
		bool leftJoinExists = false;
		bool rightJoinExists = false;
		leftJointTime.startAtLeft = true;

		Stretching prevJoin;


		if (currentJoin != cuts.end()) {
			leftJointTime = getJoinTime(*currentJoin, orientation);
			leftJoinExists = true;
		}

		if (joinIndex != 0) {
			prevJoin = *std::prev(currentJoin);
			rightJointTime = getJoinTime(prevJoin, orientation);
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
        if (leftJoinExists && rightJoinExists && sharesLowerForkTip(prevJoin, *currentJoin)) {
            // Follow the upper outline, then close the wedge through the shared tip.
            const auto curves = baseContourCurves(path);
            if (curves.empty()) return {};
            const auto tip = prevJoin.bottomBaseLine.time;
            double start = prevJoin.topBaseLine.time;
            double finish = currentJoin->topBaseLine.time;
            if ((start < tip && tip < finish) || (start > finish && (tip > start || tip < finish)))
                std::swap(start, finish);
            if (finish < start) finish += curves.size();
            const int index = static_cast<int>(std::floor(start));
            constructingPath.moveTo(curves[index % curves.size()].pointAt(start - index));
            for (double time = start; time < finish;) {
                const int segment = static_cast<int>(std::floor(time));
                const double next = std::min(finish, double(segment + 1));
                const auto arc = curves[segment % curves.size()].getSubRange(time - segment, next - segment);
                constructingPath.cubicTo(arc.pt2(), arc.pt3(), arc.pt4());
                time = next;
            }
            const int segment = static_cast<int>(std::floor(tip));
            constructingPath.lineTo(curves[segment % curves.size()].pointAt(tip - segment));
            constructingPath.closeSubpath();
        } else {
        bool correct = true;
		for (int i = 1; i < nextPoints.size(); i++) {
			//assert(nextPoints[i - 1] < nextPoints[i]);
			if (!(nextPoints[i - 1] < nextPoints[i]) ||
                (expanded && i >= 2 && std::floor(nextPoints[i - 2]) == std::floor(nextPoints[i]))) {
				correct = false;
				break;
			}
		}

		qreal curveIndex = -1;
		int pathIndex = -1;

		int currentJoinTimeIndex = 0;

		if (!correct) {
			std::cout << "Error in joins, text=" << text.toStdString() << std::endl;
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

			if (isOutside) {
				if ((curveIndex + 1) <= currentJoinTime) continue;
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
							connectTo(constructingPath, b1.pt1());
						}

						constructingPath.cubicTo(b1.pt2(), b1.pt3(), b1.pt4());
					}
					else {
						QLineF line{ path.elementAt(currentIndex - 1),e };
						if (constructingPath.isEmpty()) {
							constructingPath.moveTo(line.pointAt(tstart));
						}
						else {
							connectTo(constructingPath, line.pointAt(tstart));
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

							connectTo(constructingPath, b2.pt1());

							constructingPath.cubicTo(b2.pt2(), b2.pt3(), b2.pt4());
						}
					}
					else {
						QLineF line{ path.elementAt(currentIndex - 1),e };
						constructingPath.lineTo(line.pointAt(tend));

						if (tstart != 0.0) {

							connectTo(constructingPath, line.pointAt(tstart));

							constructingPath.lineTo(e);
						}
					}

					currentJoinTimeIndex++;
				}


			}

		}

        }
        if (hasCounterCut && !constructingPath.isEmpty()) {
            connectTo(constructingPath, constructingPath.elementAt(0));
            constructingPath.closeSubpath();
        }
		for (int subpathIndex = 0; subpathIndex < subPaths.size(); ++subpathIndex) {
            if (std::any_of(cuts.begin(), cuts.end(), [&](const auto& cut) {
                    return cut.counterEntry.pathIndex == subpathIndex + 1;
                })) continue;
            const auto& subpath = subPaths[subpathIndex];
            // Qt's coarse path containment can reject a counter tangent to the
            // rim. Test filled area at higher precision before dropping it.
            bool contained = constructingPath.contains(subpath);
            if (!contained && constructingPath.boundingRect().contains(subpath.boundingRect())) {
                const auto box = subpath.boundingRect();
                const double longest = std::max(box.width(), box.height());
                if (longest > 0) {
                    const double units = 512 / longest;
                    QTransform detail;
                    detail.scale(units, units).translate(-box.center().x(), -box.center().y());
                    contained = detail.map(subpath).subtracted(detail.map(constructingPath)).isEmpty();
                }
            }
			if (contained) {
				constructingPath.addPath(subpath);
			}
		}

		regions.push_back({ text ,constructingPath });
	Continue:
		if (currentJoin != cuts.end())
			currentJoin++;
	}



	return regions;


}





static bool interiorInkSegment(const QPainterPath& ink, const QPointF& top, const QPointF& bottom,
                               double topTolerance = 0) {
    const QLineF chord(top, bottom);
    if (chord.length() < 1e-6) return false;
    for (double t : {0.1, 0.25, 0.5, 0.75, 0.9}) {
        if (!ink.contains(chord.pointAt(t))) return false;
    }
    const auto crossings = FindInterections(ink, chord, true);
    for (const auto& crossing : crossings) {
        if (QLineF(top, crossing.point).length() > std::max(1e-5, topTolerance) &&
            QLineF(bottom, crossing.point).length() > 1e-5) return false;
    }
    return true;
}

static bool interiorChord(const QPainterPath& ink, const PathExtrema& top, const PathExtrema& bottom,
                          double topTolerance) {
    return top.pathIndex == 0 && bottom.pathIndex == 0 &&
        std::isfinite(top.time) && std::isfinite(bottom.time) &&
        top.time >= 0 && bottom.time >= 0 && top.time != bottom.time &&
        interiorInkSegment(ink, top.point, bottom.point, topTolerance);
}

// Split the two bridges of a loop attached to an ascender. The outer walk
// supplies the first endpoint; sharp inner left/right corners supply the
// other endpoints. The lower bridge is projected out of the counter's tip.
static bool searchCounterLigature(SearchContext& context) {
    const auto form = context.form;
    context.form = form.value("outer").toObject();
    const bool found = !form.contains("forkRise") && searchContourWalk(context, nullptr, false, true);
    auto outer = context.res.topBaseLine;
    context.form = form;
    const bool fork = form.contains("forkRise");
    const bool stemSide = form.value("lowerBridge") == "stem-side";
    if ((!found || !outer.isSharp) && !fork) return false;
    const auto& ink = context.transformedPath;
    QVector<PathExtrema> points;
    std::vector<QPainterPath> paths;
    ShapeItem::getExtrema(ink, paths, points);
    for (size_t index = 1; index < context.subPaths.size(); ++index) {
        const auto& hole = context.subPaths[index];
        const auto box = hole.boundingRect();
        if (box.height() <= 0 || (fork ? !context.subPaths.front().contains(hole) : ink.contains(box.center()))) continue;
        const bool atBaseline = stemSide || form.value("lowerBridge") == "bottom-extrema";
        std::optional<PathExtrema> left, right;
        for (const auto& point : points) if (point.pathIndex == index) {
                if ((point.isSharp || (stemSide && point.isX)) &&
                    std::abs(point.point.x() - box.left()) < (fork ? .04 * box.height() : 1e-5) &&
                    (!fork || !left || point.point.y() < left->point.y()))
                    left = point;
                if (atBaseline ? point.isY && std::abs(point.point.y() - box.bottom()) < 1e-5
                               : point.isSharp && std::abs(point.point.x() - box.right()) < 1e-5)
                    right = point;
        }
        if (!left || !right)
            continue;
        QPointF bendWeights;
        const auto bridge = [&](const QPointF& from, QPointF& weights) {
            weights = {};
            if (interiorInkSegment(ink, from, left->point))
                return true;
            const double limit = form.value("maxBridgeBend").toDouble();
            if (limit <= 0)
                return false;
            const auto along = left->point - from, across = right->point - from;
            const auto cross = [](QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); };
            const double determinant = cross(along, across);
            if (std::abs(determinant) < 1e-6)
                return false;
            const QPointF normal(-along.y(), along.x());
            for (double fraction : {.2, .4, .65, 1.})
                for (double side : {-1., 1.})
                    for (double t : {.5, .33, .67}) {
                        const auto delta = t * along + side * limit * fraction * normal;
                        const auto turn = from + delta;
                        if (interiorInkSegment(ink, from, turn) && interiorInkSegment(ink, turn, left->point)) {
                            weights = {cross(delta, across) / determinant, cross(along, delta) / determinant};
                            return true;
                        }
                    }
            return false;
        };
        if (fork) {
            bool foundFork = false;
            double nearest = box.height() * form.value("maxBridgeRatio").toDouble();
            for (auto vertex = context.timeSorted.begin(); vertex != context.timeSorted.end(); ++vertex) {
                const double distance = QLineF(vertex->point, left->point).length();
                QPointF weights;
                if ((!vertex->isSharp && !stemSide) || distance >= nearest || vertex->point.y() >= left->point.y() ||
                    !bridge(vertex->point, weights))
                    continue;
                double lean[2]{};
                int side = 0;
                for (auto advance : {&SearchContext::prev, &SearchContext::next}) {
                    auto current = vertex;
                    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                        auto next = (context.*advance)(current);
                        if (next->point.y() > current->point.y() + .06 * box.height())
                            break;
                        if (vertex->point.y() - next->point.y() >=
                            form.value("forkRise").toDouble() * box.height()) {
                            lean[side] = next->point.x() - vertex->point.x();
                            break;
                        }
                        current = next;
                    }
                    ++side;
                }
                if (lean[0] * lean[1] >= 0)
                    continue;
                outer = *vertex;
                bendWeights = weights;
                nearest = distance;
                foundFork = true;
            }
            if (!foundFork)
                continue;
        }
        if (outer.point.y() >= left->point.y() ||
            QLineF(outer.point, left->point).length() > form.value("maxBridgeRatio").toDouble() * box.height() ||
            (bendWeights.isNull() && !interiorInkSegment(ink, outer.point, left->point)))
            continue;
        auto hits = atBaseline ? std::vector<PathExtrema>{} : FindInterections(ink, QLineF(right->point,
            QPointF(context.bbox.right() + box.width(), right->point.y())), true);
        if (atBaseline) for (const auto& point : context.timeSorted) {
            if (point.isY && point.point.y() > right->point.y() &&
                point.point.x() >= box.left() && point.point.x() <= box.right()) hits.push_back(point);
        }
        std::optional<PathExtrema> exit;
        double nearest = form.value("maxBridgeRatio").toDouble() * box.height();
        for (const auto& hit : hits) {
            const double distance = atBaseline ? QLineF(hit.point, right->point).length() : hit.point.x() - right->point.x();
            if (hit.pathIndex != 0 || distance <= 1e-5 || distance >= nearest ||
                !interiorInkSegment(ink, right->point, hit.point)) continue;
            exit = hit; nearest = distance;
        }
        if (stemSide) {
            // Walk down the upright's edge, through the counter, to the
            // point where the lower return of the other letter joins it.
            exit.reset();
            double best = std::numeric_limits<double>::infinity();
            for (const auto &notch : context.timeSorted) {
                const auto d = notch.point - left->point;
                if (d.y() < .4 * box.height() || d.y() > 2 * box.height() || std::abs(d.x()) > .65 * box.height() ||
                    !notch.isSharp)
                    continue;
                const double x = std::clamp(notch.point.x(), box.left() + .18 * box.height(), box.center().x());
                for (auto inner : FindInterections(hole, QLineF(x, box.bottom() + 1, x, box.top() - 1), true)) {
                    if (inner.point.y() < box.center().y() || !interiorInkSegment(ink, notch.point, inner.point))
                        continue;
                    const double fit = QLineF(notch.point, inner.point).length() + std::abs(d.x());
                    if (fit >= best)
                        continue;
                    best = fit;
                    inner.pathIndex = int(index);
                    right = inner;
                    exit = notch;
                }
            }
        }
        if (!exit) continue;
        Stretching cut{};
        cut.topBaseLine = cut.topLeft = cut.topRight = outer;
        cut.bottomBaseLine = cut.bottomLeft = cut.bottomRight = *exit;
        cut.counterEntry = *left; cut.counterExit = *right; cut.counterBendWeights=bendWeights;
        // The outer endpoints span the counter, not a single stroke. Use an
        // actual ink bridge as the scale for subsequent body recognition.
        cut.referenceWidth = QLineF(outer.point, left->point).length();
        cut.fixedRightBoundary = true;
        cut.kashidaOverrides.insert("enabled", false);
        const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
        if (pieces.size() != 2 || pieces[0].path.isEmpty() || pieces[1].path.isEmpty()) continue;
        bool ascender = false;
        for (const auto& point : context.timeSorted) {
            if (point.point.x() >= outer.point.x() ||
                outer.point.y() - point.point.y() < form.value("minAscenderRise").toDouble() * box.height()) continue;
            ascender |= pieces[1].path.contains(point.point + QPointF(0, box.height() * .01));
        }
        if (!ascender || pieces[0].path.boundingRect().right() < context.bbox.right() - 1e-5) continue;
        context.res = cut;
        return true;
    }
    return false;
}

// Track the thin horizontal ink run through a join. The first change in
// thickness or height on either side marks the transition into a body.
static bool findJoinEnds(const QPainterPath& ink, const Stretching& join,
                         double minX, double maxX, const QJsonObject& settings,
                         Stretching& left, Stretching& right) {
    const auto center = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).center();
    const auto box = ink.boundingRect();
    const double anchorWidth = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).length();
    if (anchorWidth < settings.value("minAnchorWidth").toDouble()) return false;
    const double samplesPerWidth = settings.value("samplesPerAnchorWidth").toDouble();
    if (anchorWidth <= 0 || samplesPerWidth <= 0) return false;
    const double step = anchorWidth / samplesPerWidth;
    // A steep anchor follows the cross section of the join. An oblique
    // anchor can sit against the body, so keep the wider clearance there.
    const bool steepAnchor = std::abs(join.bottomBaseLine.point.y() - join.topBaseLine.point.y()) >= anchorWidth * settings.value("steepAnchorRatio").toDouble();
    const double clearanceRatio = settings.value(steepAnchor ? "steepClearanceRatio" : "obliqueClearanceRatio").toDouble();
    const double trackingDistance = anchorWidth * settings.value("trackingDistanceRatio").toDouble();
    const auto sample = [&](double x, double y, Stretching& run, bool& nearInk) {
        const QLineF probe(x, box.top() - step, x, box.bottom() + step);
        const auto hits = FindInterections(ink, probe);
        double distance = trackingDistance;
        bool found = false;
        for (size_t i = 1; i < hits.size(); ++i) {
            const auto& top = hits[i - 1];
            const auto& bottom = hits[i];
            const QLineF chord(top.point, bottom.point);
            const double offset = std::abs(chord.center().y() - y);
            if (offset >= distance || top.pathIndex != 0 || bottom.pathIndex != 0 ||
                !ink.contains(chord.center())) continue;
            run.topBaseLine = top;
            run.bottomBaseLine = bottom;
            const double clearance = chord.length() * clearanceRatio;
            nearInk = (i >= 3 && top.point.y() - hits[i - 2].point.y() < clearance) ||
                      (i + 2 < hits.size() && hits[i + 1].point.y() - bottom.point.y() < clearance);
            distance = offset;
            found = true;
        }
        return found;
    };
    const auto width = [](const Stretching& run) {
        return run.bottomBaseLine.point.y() - run.topBaseLine.point.y();
    };
    const auto height = [](const Stretching& run) {
        return (run.topBaseLine.point.y() + run.bottomBaseLine.point.y()) / 2;
    };
    Stretching seed = join;
    bool seedNearInk = false;
    double best = std::numeric_limits<double>::infinity();
    const int seedSteps = std::clamp(settings.value("seedSteps").toInt(), 0, 1000);
    const double minSeedWidth = anchorWidth * settings.value("minSeedWidthRatio").toDouble();
    const double maxSeedWidth = anchorWidth * settings.value("maxSeedWidthRatio").toDouble();
    const double seedDistancePenalty = settings.value("seedDistancePenalty").toDouble();
    for (int offset = -seedSteps; offset <= seedSteps; ++offset) {
        const double x = center.x() + offset * step;
        Stretching run = join;
        bool nearInk = false;
        if (x <= minX || x >= maxX || !sample(x, center.y(), run, nearInk) ||
            width(run) < minSeedWidth || width(run) > maxSeedWidth) continue;
        const double cost = width(run) + std::abs(offset) * step * seedDistancePenalty;
        if (cost < best) {
            best = cost;
            seed = run;
            seedNearInk = nearInk;
        }
    }
    if (!std::isfinite(best)) return false;

    const int maxTrackSteps = std::clamp(settings.value("maxTrackSteps").toInt(), 0, 1000);
    const double minTrackWidth = width(seed) * settings.value("minTrackWidthRatio").toDouble();
    const double maxTrackWidth = width(seed) * settings.value("maxTrackWidthRatio").toDouble();
    const double maxSteepRise = step * settings.value("maxSteepSlope").toDouble();
    const double maxEdgeDrift = width(seed) * settings.value("maxEdgeDriftRatio").toDouble();
    const double minSpan = width(seed) * settings.value("minSpanToStrokeRatio").toDouble();
    const auto findEnd = [&](int direction, Stretching& end) {
        end = seed;
        const double start = seed.topBaseLine.point.x();
        for (int index = 1; index <= maxTrackSteps; ++index) {
            const double x = start + direction * index * step;
            if (x <= minX || x >= maxX) return false;
            Stretching run = join;
            bool nearInk = false;
            if (!sample(x, height(end), run, nearInk) || (!seedNearInk && nearInk) || width(run) > maxTrackWidth ||
                width(run) < minTrackWidth ||
                (steepAnchor && std::abs(height(run) - height(end)) > maxSteepRise) ||
                std::abs(run.topBaseLine.point.y() - seed.topBaseLine.point.y()) > maxEdgeDrift ||
                std::abs(run.bottomBaseLine.point.y() - seed.bottomBaseLine.point.y()) > maxEdgeDrift) return true;
            end = run;
        }
        return false;
    };
    if (join.fixedRightBoundary) right = join;
    return findEnd(-1, left) && (join.fixedRightBoundary || findEnd(1, right)) &&
           right.topBaseLine.point.x() - left.topBaseLine.point.x() >= minSpan &&
           interiorChord(ink, left.topBaseLine, left.bottomBaseLine) &&
           interiorChord(ink, right.topBaseLine, right.bottomBaseLine);
}

static bool medialChordAt(const SearchContext& context, double x, double y,
                          double tolerance, Stretching& cut, bool useBottom = false,
                          double minY = -std::numeric_limits<double>::infinity()) {
    const auto& ink = context.transformedPath;
    const auto hits = FindInterections(ink, QLineF(x, context.bbox.top() - 1, x, context.bbox.bottom() + 1));
    double best = tolerance;
    bool found = false;
    for (size_t i = 1; i < hits.size(); ++i) {
        const auto& top = hits[i - 1];
        const auto& bottom = hits[i];
        const double sampleY = useBottom ? bottom.point.y() : (top.point.y() + bottom.point.y()) / 2;
        const double distance = std::abs(sampleY - y);
        if (top.point.y() < minY || distance >= best || !interiorChord(ink, top, bottom)) continue;
        cut.topBaseLine = top;
        cut.bottomBaseLine = bottom;
        best = distance;
        found = true;
    }
    return found;
}

// Bisect the directions of the two outline tangents to estimate the local
// stroke direction, then intersect its normal with the surrounding ink.
static bool strokeNormalCut(const SearchContext& context, Stretching& cut, double offsetX) {
    const auto& ink = context.transformedPath;
    const auto curves = baseContourCurves(ink);
    const auto tangentAt = [&](const PathExtrema& point) {
        const double time = std::abs(point.time - curves.size()) < 1e-9 ? 0 : point.time;
        const int index = static_cast<int>(std::floor(time));
        if (point.pathIndex != 0 || index < 0 || index >= static_cast<int>(curves.size())) return QPointF{};
        const auto& curve = curves[index];
        const double t = time - index, u = 1 - t;
        auto tangent = u * u * (curve.pt2() - curve.pt1()) +
            2 * u * t * (curve.pt3() - curve.pt2()) + t * t * (curve.pt4() - curve.pt3());
        if (std::hypot(tangent.x(), tangent.y()) <= 1e-9)
            tangent = curve.pointAt(std::min(1.0, t + 1e-4)) - curve.pointAt(std::max(0.0, t - 1e-4));
        const double length = std::hypot(tangent.x(), tangent.y());
        return length > 1e-9 ? tangent / length : QPointF{};
    };
    const auto center = [](const Stretching& candidate) {
        return QLineF(candidate.topBaseLine.point, candidate.bottomBaseLine.point).center();
    };
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    const auto solve = [&](const QPointF& pivot) -> StrokeNormalSample {
        struct Candidate {
            std::pair<PathExtrema, PathExtrema> endpoints;
            double error;
            double width;
            std::optional<bool> inside;
        };
        const auto evaluate = [&](double angle) -> std::optional<Candidate> {
            const QPointF normal(std::sin(angle), std::cos(angle));
            const auto hits = FindInterections(ink, QLineF(pivot - normal * reach, pivot + normal * reach), true);
            std::optional<PathExtrema> top, bottom;
            double topDistance = -reach, bottomDistance = reach;
            for (const auto& hit : hits) {
                const double distance = QPointF::dotProduct(hit.point - pivot, normal);
                if (distance < -1e-6 && distance > topDistance) { top = hit; topDistance = distance; }
                if (distance > 1e-6 && distance < bottomDistance) { bottom = hit; bottomDistance = distance; }
            }
            if (!top || !bottom) return std::nullopt;
            auto a = tangentAt(*top), b = tangentAt(*bottom);
            if (a.isNull() || b.isNull()) return std::nullopt;
            if (QPointF::dotProduct(a, b) < 0) b = -b;
            auto tangent = a + b;
            const double length = std::hypot(tangent.x(), tangent.y());
            if (length <= 1e-9) return std::nullopt;
            tangent /= length;
            if (tangent.x() < 0) tangent = -tangent;
            return Candidate{{*top, *bottom}, QPointF::dotProduct(normal, tangent),
                             QLineF(top->point, bottom->point).length(), {}};
        };
        const auto inside = [&](Candidate& candidate) {
            if (!candidate.inside.has_value())
                candidate.inside = interiorChord(ink, candidate.endpoints.first, candidate.endpoints.second);
            return *candidate.inside;
        };
        std::optional<Candidate> best, previous;
        double previousAngle = 0;
        std::vector<Candidate> widthSamples;
        widthSamples.reserve(121);
        const auto accept = [&](Candidate& value) {
            if (std::abs(value.error) < 1e-5 && (!best || value.width < best->width) && inside(value)) best = value;
        };
        // Only a root or the two samples bracketing it can supply a normal.
        // Defer the five ink probes and the second intersection scan until then.
        for (int sample = 0; sample <= 120; ++sample) {
            const double angle = -M_PI / 2 + M_PI * sample / 120;
            auto current = evaluate(angle);
            if (current) {
                accept(*current);
                if (previous && previous->error * current->error < 0 && inside(*previous) && inside(*current)) {
                    double lo = previousAngle, hi = angle, loError = previous->error;
                    for (int iteration = 0; iteration < 36; ++iteration) {
                        const double middle = (lo + hi) / 2;
                        auto value = evaluate(middle);
                        if (!value || !inside(*value)) break;
                        if (std::abs(value->error) < 1e-6) { accept(*value); break; }
                        if (loError * value->error <= 0) hi = middle;
                        else { lo = middle; loError = value->error; }
                    }
                }
                if (!best && current->width < reach) widthSamples.push_back(*current);
            }
            previous = current;
            previousAngle = angle;
        }
        if (best) return {best->endpoints, best->width};
        // Without a normal, the caller uses the shortest valid sampled width.
        std::sort(widthSamples.begin(), widthSamples.end(),
                  [](const Candidate& a, const Candidate& b) { return a.width < b.width; });
        for (auto& candidate : widthSamples)
            if (inside(candidate)) return {std::nullopt, candidate.width};
        return {};
    };
    const auto orient = [&](Stretching& candidate, double* measuredWidth = nullptr) {
        const auto pivot = center(candidate);
        StrokeNormalSample sample;
        if (context.outlineCache && std::isfinite(pivot.x()) && std::isfinite(pivot.y())) {
            auto [it, inserted] = context.outlineCache->normalsByCenter.try_emplace(std::make_pair(pivot.x(), pivot.y()));
            if (inserted) it->second = solve(pivot);
            sample = it->second;
        } else {
            sample = solve(pivot);
        }
        if (measuredWidth) *measuredWidth = sample.width;
        if (!sample.endpoints) return false;
        candidate.topBaseLine = sample.endpoints->first;
        candidate.bottomBaseLine = sample.endpoints->second;
        return true;
    };
    auto result = cut;
    double stroke = 0;
    const bool initialNormal = orient(result, &stroke);
    if (offsetX != 0) {
        // At a corner the unshifted chord may have no unique normal. Its
        // shortest local cross-section still supplies a width for the offset.
        if (stroke <= 0) return false;
        const auto pivot = center(cut);
        const double dx = offsetX * stroke;
        if (!medialChordAt(context, pivot.x() + dx, pivot.y(), 2 * stroke, result) ||
            !orient(result)) return false;
    } else if (!initialNormal) return false;
    cut.referenceWidth = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
    cut.topBaseLine = result.topBaseLine;
    cut.bottomBaseLine = result.bottomBaseLine;
    cut.topEndpointTolerance = 0;
    return true;
}

// Validate the body made by the cut, not just the route that reached it.
// Reuse this check when a shared ligature boundary replaces an earlier cut.
static bool containsHeadLips(const QPainterPath& body, const QJsonObject& lips, QLineF* evidence = nullptr);

static bool matchesBoundaryBody(SearchContext& context, const QJsonObject& rule,
                                const Stretching& boundary, bool initial, const QJsonObject& initialLips = {},
                                QString* reason = nullptr) {
    const auto reject = [&](const QString& message) {
        if (reason) *reason = message;
        return false;
    };
    const auto features = rule.value("bodyFeatures").toObject();
    const auto shape = rule.value("body").toObject().value("shape");
    const bool finalCounter = !initial && (rule.value("method") == "final-counter" ||
        shape == "counter-neck" || shape == "counter-shoulder" || shape == "counter-bridge");
    const bool protectOpening = initial && !initialLips.isEmpty();
    const bool protectTail = !initial && rule.value("ownsLeftTip").toBool();
    if (features.isEmpty() && !finalCounter && !protectOpening && !protectTail) return true;
    const auto pieces = cutRegions(context.transformedPath, {}, {boundary}, context.orientation, true);
    if (pieces.size() != 2) return false;
    if (protectTail && pieces[1].path.boundingRect().left() > context.bbox.left() + 1e-6)
        return reject("cut separates the terminal tail");
    if (protectOpening && !containsHeadLips(pieces[0].path, initialLips))
        return reject("cut separates the lips of the initial head");
    if (finalCounter) {
        // Recheck the actual hole, not only its bounding box, when another
        // template proposes replacing this final body's boundary.
        const QPainterPath* counter = nullptr;
        for (size_t i = 1; i < context.subPaths.size(); ++i) {
            const auto& hole = context.subPaths[i];
            if (context.transformedPath.contains(hole.boundingRect().center())) continue;
            if (!counter || hole.boundingRect().center().x() < counter->boundingRect().center().x()) counter = &hole;
        }
        const auto contours = toSubpaths(pieces[1].path);
        if (!counter || std::none_of(contours.begin(), contours.end(), [&](const auto& p) { return p == *counter; }))
            return reject("body counter ownership rejected");
    }
    const auto bounds = pieces[initial ? 0 : 1].path.boundingRect();
    if (bounds.isEmpty()) return false;
    if (features.contains("minWidthToHeight") &&
        bounds.width() < features.value("minWidthToHeight").toDouble() * bounds.height()) return false;
    if (features.contains("maxWidthToHeight") &&
        bounds.width() > features.value("maxWidthToHeight").toDouble() * bounds.height()) return false;
    if (features.contains("maxUpperPeaks")) {
        const auto side = getJoinTime(boundary, context.orientation);
        int peaks = 0;
        for (auto point = context.timeSorted.begin(); point != context.timeSorted.end(); ++point) {
            if (!(initial ? side.isRight(point->time) : side.isLeft(point->time)) ||
                point->point.y() >= context.prev(point)->point.y() ||
                point->point.y() >= context.next(point)->point.y()) continue;
            const auto a = point->point - context.prev(point)->point;
            const auto b = context.next(point)->point - point->point;
            const double turn = a.x() * b.y() - a.y() * b.x();
            if (context.orientation == Orientation::ClockWise ? turn < 0 : turn > 0) ++peaks;
        }
        if (peaks > features.value("maxUpperPeaks").toInt()) return false;
    }
    if (features.contains("minHeight")) {
        double width = rule.value("_bodyPenWidth").toDouble(rule.value("penWidth").toDouble());
        const auto stem = rule.value("descendingStem").toObject();
        if (stem.contains("headLip")) width = stem.value("penWidth").toDouble();
        if (width <= 0) {
            auto normal = boundary;
            // A sharp endpoint can have two valid tangent directions. Keep the
            // original cross-section as a conservative width in that situation.
            if (!strokeNormalCut(context, normal)) normal = boundary;
            width = QLineF(normal.topBaseLine.point, normal.bottomBaseLine.point).length();
        }
        if (width <= 0 || bounds.height() < features.value("minHeight").toDouble() * width) return false;
    }
    return true;
}

// The incoming tooth shoulder turns back to the right; its opposite inner
// bend is a local minimum on the lower contour. Recognize sharp corners and
// their rounded extrema equivalents using only the first tooth's geometry.
static std::optional<Stretching> seenEntryCornerPair(SearchContext& context, const PathExtrema& tip,
                                                    bool increasingTime, double maxDistance) {
    const auto advance = increasingTime ? &SearchContext::prev : &SearchContext::next;
    auto current = context.timeSorted.find(tip);
    if (current == context.timeSorted.end()) return {};
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(current);
        if (next->point.y() <= current->point.y()) break;
        current = next;
        if ((!current->isSharp && !current->isX) ||
            current->point.x() >= context.prev(current)->point.x() ||
            current->point.x() >= context.next(current)->point.x() ||
            QLineF(current->point, tip.point).length() > maxDistance) continue;
        std::optional<Stretching> pair;
        double nearest = maxDistance;
        for (auto bottom = context.timeSorted.begin(); bottom != context.timeSorted.end(); ++bottom) {
            const double distance = QLineF(current->point, bottom->point).length();
            if ((!bottom->isSharp && !bottom->isY) ||
                bottom->point.y() >= context.prev(bottom)->point.y() ||
                bottom->point.y() >= context.next(bottom)->point.y() ||
                bottom->point.y() <= current->point.y() || distance >= nearest ||
                !interiorChord(context.transformedPath, *current, *bottom)) continue;
            Stretching cut{};
            cut.topBaseLine = *current;
            cut.bottomBaseLine = *bottom;
            pair = cut;
            nearest = distance;
        }
        if (pair) return pair;
    }
    return {};
}

// A curved final tooth may not admit a vertical exit chord. Its lower inner
// bend supplies a normal to the branch leaving the tooth. Accept the opposite
// endpoint only on this tooth's descending flank, before its first valley.
static bool seenExitBendCut(SearchContext& context, const PathExtrema& tip,
                            bool increasingTime, double maxDistance, Stretching& cut,
                            double alongContour = 0) {
    const auto advance = increasingTime ? &SearchContext::prev : &SearchContext::next;
    auto end = context.timeSorted.find(tip);
    if (end == context.timeSorted.end()) return false;
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(end);
        if (next->point.y() <= end->point.y()) break;
        end = next;
    }
    const double count = baseContourCurves(context.transformedPath).size();
    const auto distanceOnFlank = [&](double time) {
        const double delta = increasingTime ? time - tip.time : tip.time - time;
        return delta < 0 ? delta + count : delta;
    };
    const double reach = distanceOnFlank(end->time);
    double nearest = maxDistance;
    bool found = false;
    for (auto inner = context.timeSorted.begin(); inner != context.timeSorted.end(); ++inner) {
        const double distance = QLineF(tip.point, inner->point).length();
        if (inner->point.y() <= tip.point.y() || inner->point.x() >= tip.point.x() ||
            distance >= nearest ||
            inner->point.y() >= context.prev(inner)->point.y() ||
            inner->point.y() >= context.next(inner)->point.y()) continue;
        for (bool forward : {false, true}) {
            const auto adjacent = forward ? context.prev(inner) : context.next(inner);
            if (adjacent->point.x() >= inner->point.x()) continue;
            Stretching proposed{};
            if (!contourNormalCut(context, *inner, forward, false, proposed, true)) continue;
            if (proposed.topBaseLine.point.y() > proposed.bottomBaseLine.point.y())
                std::swap(proposed.topBaseLine, proposed.bottomBaseLine);
            if (distanceOnFlank(proposed.topBaseLine.time) > reach) continue;
            if (alongContour > 0) {
                // Stay on the branch leaving the bend. A free normal through a
                // shifted midpoint can re-enter a different stroke on a steep join.
                auto limit = *adjacent;
                if (forward && limit.time <= inner->time) limit.time += count;
                if (!forward && limit.time >= inner->time) limit.time -= count;
                const double travel = alongContour * proposed.referenceWidth;
                PathExtrema shifted{};
                QPointF tangent;
                if (!advanceOnContour(context.transformedPath, *inner, limit, forward, travel, shifted, tangent) ||
                    !contourNormalCut(context, shifted, forward, false, proposed, true)) continue;
                std::swap(proposed.topBaseLine, proposed.bottomBaseLine);
                if (distanceOnFlank(proposed.topBaseLine.time) > reach) continue;
            }
            cut = proposed;
            cut.referenceWidth = QLineF(proposed.topBaseLine.point, proposed.bottomBaseLine.point).length();
            nearest = distance;
            found = true;
        }
    }
    return found;
}

// The outer flank of a folded last tooth descends directly to a leftward
// turn. The facing turn across the ink is the other end of the final's cut.
static bool seenTipMeetsFold(SearchContext& context, const PathExtrema& tip, bool increasingTime,
                             const Stretching& cut) {
    auto outer = context.timeSorted.find(cut.topBaseLine);
    auto inner = context.timeSorted.find(cut.bottomBaseLine);
    if (outer == context.timeSorted.end() || inner == context.timeSorted.end()) return false;
    if (outer->point.x() > inner->point.x()) std::swap(outer, inner);
    const auto leftTurn = [&](auto point) {
        return point->point.x() < context.prev(point)->point.x() &&
               point->point.x() < context.next(point)->point.x();
    };
    if (tip.point.y() >= outer->point.y() || !leftTurn(outer) || !leftTurn(inner) ||
        inner->point.y() >= outer->point.y() ||
        !interiorChord(context.transformedPath, *outer, *inner)) return false;
    auto current = context.timeSorted.find(tip);
    while (current != outer) {
        const auto next = increasingTime ? context.prev(current) : context.next(current);
        if (next->point.x() >= current->point.x() || next->point.y() <= current->point.y()) return false;
        current = next;
    }
    return true;
}

enum class SeenExitMode { Configured, ShiftedBend, AlongContour };

struct SeenExitRetry {
    int slot;
    SeenExitMode mode;
    bool wideLastPitch = false;

    static QJsonValue offset(const QJsonObject& settings, SeenExitMode mode) {
        if (mode == SeenExitMode::AlongContour && settings.value("exitBendFirst").toBool())
            return settings.value("exitOffset");
        return settings.value("retryBendOffset");
    }

    bool supports(const QJsonObject& settings) const {
        return (!wideLastPitch || settings.value("retryMaxLastPitch").isDouble()) &&
               (mode == SeenExitMode::Configured || offset(settings, mode).isDouble());
    }
};

// Find the three prominent tips and their intervening valleys on the
// same contour, with an open (counter-free) body between the cuts.
static bool seenTeethBoundaries(SearchContext& context, const QJsonObject& form,
                                double minX, double maxX, Stretching* right, Stretching* left,
                                const Stretching* finalFold = nullptr, std::array<PathExtrema,3>* matchedTips = nullptr,
                                const SeenExitRetry* retry = nullptr) {
    const auto mode = retry ? retry->mode : SeenExitMode::Configured;
    const bool wideLastPitch = retry && retry->wideLastPitch;
    const bool retryBend = mode != SeenExitMode::Configured;
    const double rise = form.value("minToothRise").toDouble(0.55);
    const double minPitch = form.value("minPitch").toDouble(1);
    const double maxPitch = form.value("maxPitch").toDouble(5);
    const double maxLastPitch = form.value("maxLastPitch").toDouble(maxPitch);
    const auto& ink = context.transformedPath;
    const auto box = ink.boundingRect();
    const double contourLength = baseContourCurves(ink).size();
    std::optional<std::pair<Stretching, Stretching>> match;
    double firstX = minX;
    for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
        const auto reverse = advance == &SearchContext::next ? &SearchContext::prev : &SearchContext::next;
        for (auto first = context.timeSorted.begin(); first != context.timeSorted.end(); ++first) {
            if (first->point.x() >= maxX || first->point.x() <= minX ||
                first->point.y() >= context.prev(first)->point.y() ||
                first->point.y() >= context.next(first)->point.y()) continue;
            std::array<PathExtrema, 5> turns{};
            turns[0] = *first;
            auto current = first;
            auto extreme = first;
            bool down = true;
            size_t count = 1;
            for (int step = 0; step < 24 && count < turns.size(); ++step) {
                current = (context.*advance)(current);
                if (current == first || current->point.x() < minX ||
                    current->point.x() > first->point.x() + maxPitch) break;
                const double delta = current->point.y() - extreme->point.y();
                if (down ? delta > 0 : delta < 0) {
                    extreme = current;
                } else if (down ? delta < -rise : delta > rise) {
                    turns[count++] = *extreme;
                    extreme = current;
                    down = !down;
                }
            }
            if (count != turns.size()) continue;
            if (wideLastPitch || form.value("convexTips").toBool()) {
                bool convex = true;
                for (int index : {0, 2, 4}) {
                    const auto at = context.timeSorted.find(turns[index]);
                    const auto incoming = at->point - context.prev(at)->point;
                    const auto outgoing = context.next(at)->point - at->point;
                    const double turn = incoming.x() * outgoing.y() - incoming.y() * outgoing.x();
                    convex &= context.orientation == Orientation::ClockWise ? turn < 0 : turn > 0;
                }
                if (!convex) continue;
            }
            const double pitch1 = turns[0].point.x() - turns[2].point.x();
            const double pitch2 = turns[2].point.x() - turns[4].point.x();
            const bool lastPitchFits = wideLastPitch
                ? pitch2 > maxLastPitch && pitch2 <= form.value("retryMaxLastPitch").toDouble()
                : pitch2 <= maxLastPitch;
            if (pitch1 < minPitch || pitch1 > maxPitch || pitch2 < minPitch || !lastPitchFits ||
                turns[1].point.y() - turns[0].point.y() < rise ||
                turns[3].point.y() - turns[2].point.y() < rise) continue;
            if (finalFold && !seenTipMeetsFold(context, turns[4], advance == &SearchContext::prev, *finalFold)) continue;

            // An adjacent ascender can form a valid up/down sequence with only
            // two teeth. A seen head has three tips of comparable height.
            // Measure their spread against their own mean pitch, so the test
            // does not use a neighboring letter, cut, or stroke-width estimate.
            const double highestTip = std::min({turns[0].point.y(), turns[2].point.y(), turns[4].point.y()});
            const double lowestTip = std::max({turns[0].point.y(), turns[2].point.y(), turns[4].point.y()});
            if (lowestTip - highestTip > form.value("maxTipHeightSpreadRatio").toDouble(1) *
                (pitch1 + pitch2) / 2) continue;

            const auto verticalCut = [&](const PathExtrema& top, Stretching& cut) {
                const auto hits = FindInterections(ink,
                    QLineF(top.point, QPointF(top.point.x(), box.bottom() + 1)), true);
                const auto bottom = std::find_if(hits.begin(), hits.end(), [&](const auto& point) {
                    return point.point.y() > top.point.y() + 1e-5;
                });
                if (bottom == hits.end() || !interiorChord(ink, top, *bottom)) return false;
                cut.topBaseLine = top;
                cut.bottomBaseLine = *bottom;
                return true;
            };
            const auto project = [&](const PathExtrema& tip, double y, bool onRight, Stretching& cut) {
                const bool forward = (advance == &SearchContext::prev) != onRight;
                const auto contourDistance = [&](double time) {
                    const double delta = forward ? time - tip.time : tip.time - time;
                    return delta < 0 ? delta + contourLength : delta;
                };
                auto end = context.timeSorted.find(tip);
                if (!onRight) for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                    const auto next = (context.*advance)(end);
                    if (next->point.y() <= end->point.y()) break;
                    end = next;
                }
                const double reach = onRight ? 4 : contourDistance(end->time);
                auto hits = FindInterections(ink, QLineF(box.left() - 1, y, box.right() + 1, y));
                double distance = maxPitch;
                bool found = false;
                for (const auto& hit : hits) {
                    const double offset = (hit.point.x() - tip.point.x()) * (onRight ? 1 : -1);
                    if (hit.pathIndex != 0 || contourDistance(hit.time) > reach ||
                        offset <= 0 || offset >= distance ||
                        hit.point.x() <= minX || hit.point.x() >= maxX) continue;
                    if (!verticalCut(hit, cut)) continue;
                    distance = offset;
                    found = true;
                }
                return found;
            };
            Stretching r{}, l{};
            bool cornerEntry = false;
            if (right && form.value("terminalBowl").toBool() && !form.value("roundedEntry").toBool()) {
                const auto tail = std::prev(context.xSorted.end());
                const double pitch = (pitch1 + pitch2) / 2;
                // Follow the incoming flank from the first tip to its root.
                // The opposed landmark is on the underside of the same stroke.
                auto upper = first;
                double best = std::numeric_limits<double>::infinity();
                for (int step = 0; step < 4; ++step) {
                    const auto next = (context.*reverse)(upper);
                    if (next->point.y() < upper->point.y() - .06 ||
                        QLineF(next->point, first->point).length() > 1.15 * pitch)
                        break;
                    upper = next;
                    if (upper->point.y() - first->point.y() < .25)
                        continue;
                    for (const auto &lower : context.timeSorted) {
                        const auto d = lower.point - upper->point;
                        if (d.y() < .3 || d.y() > 2.5 || std::abs(d.x()) > 1.7 * d.y() ||
                            !interiorChord(ink, *upper, lower))
                            continue;
                        Stretching candidate{};
                        candidate.topBaseLine = *upper;
                        candidate.bottomBaseLine = lower;
                        if (getJoinTime(candidate, context.orientation).isStrictRight(tail->time))
                            std::swap(candidate.topBaseLine, candidate.bottomBaseLine);
                        const auto side = getJoinTime(candidate, context.orientation);
                        if (!side.isStrictLeft(turns[0].time) || !side.isStrictLeft(turns[2].time) ||
                            !side.isStrictLeft(turns[4].time))
                            continue;
                        const double fit =
                            std::hypot(d.x(), d.y()) + .35 * std::abs(upper->point.y() - turns[1].point.y());
                        if (fit < best) {
                            best = fit;
                            r = candidate;
                            cornerEntry = true;
                        }
                    }
                }
            }
            if (right && form.value("roundedEntry").toBool()) {
                const auto tail = std::prev(context.xSorted.end());
                // Stay on the incoming stroke beside the first tooth, including
                // when the contour normal from its sharp tip crosses the tooth.
                for (double offset : {.25, .5, .75}) {
                    Stretching vertical{};
                    if (!medialChordAt(context, turns[0].point.x() + offset, turns[1].point.y(), 2.5, vertical))
                        continue;
                    auto normal = vertical;
                    const bool hasNormal = strokeNormalCut(context, normal);
                    for (auto option : {hasNormal ? normal : vertical, vertical}) {
                        if (getJoinTime(option, context.orientation).isStrictRight(tail->time))
                            std::swap(option.topBaseLine, option.bottomBaseLine);
                        const auto side = getJoinTime(option, context.orientation);
                        const QLineF chord(option.topBaseLine.point, option.bottomBaseLine.point);
                        if (chord.length() < .3 || chord.length() > 2.8 || !side.isStrictLeft(turns[0].time) ||
                            !side.isStrictLeft(turns[2].time) || !side.isStrictLeft(turns[4].time))
                            continue;
                        r = option;
                        cornerEntry = true;
                        break;
                    }
                    if (cornerEntry)
                        break;
                }
            }
            if (right && !cornerEntry && form.contains("rightOffset")) {
                const auto entry = seenEntryCornerPair(context, turns[0], reverse == &SearchContext::prev,
                                                       (pitch1 + pitch2) / 2);
                if (entry) {
                    const QLineF edge(entry->topBaseLine.point, entry->bottomBaseLine.point);
                    const double stroke = edge.length();
                    const auto pivot = edge.center();
                    // Shift into the incoming join by a fraction of the width
                    // between these two landmarks, then use the local stroke normal.
                    const double x = pivot.x() + form.value("rightOffset").toDouble() * stroke;
                    cornerEntry = medialChordAt(context, x, pivot.y(), 2 * stroke, r) && strokeNormalCut(context, r);
                    if (cornerEntry) r.referenceWidth = stroke;
                }
                if (!cornerEntry && !form.value("entryCornerOptional").toBool()) continue;
            }
            if (right && !cornerEntry) {
                auto foot = first;
                for (int step = 0; step < 4; ++step) {
                    const auto next = (context.*reverse)(foot);
                    if (next->point.y() <= foot->point.y() || next->point.x() >= maxX) break;
                    foot = next;
                }
                const bool atFoot = (left || form.value("terminalBowl").toBool()) &&
                                    foot->point.y() - first->point.y() >= rise &&
                                    std::abs(foot->point.x() - first->point.x()) <= maxPitch && verticalCut(*foot, r);
                if (!atFoot && !project(turns[0], turns[1].point.y(), true, r)) continue;
            }
            bool intrinsicExit = finalFold != nullptr;
            if (finalFold) l = *finalFold;
            else if (left && (retryBend || form.value("exitBendFirst").toBool())) {
                const double radius = form.value("maxExitBendDistance").toDouble(2) * (pitch1 + pitch2) / 2;
                const auto offset = retryBend ? SeenExitRetry::offset(form, mode) : form.value("exitOffset");
                const bool followContour = mode == SeenExitMode::AlongContour;
                intrinsicExit = seenExitBendCut(context, turns[4], advance == &SearchContext::prev,
                                               radius, l, followContour ? offset.toDouble() : 0);
                if (intrinsicExit && !followContour && offset.isDouble())
                    intrinsicExit = strokeNormalCut(context, l, -offset.toDouble());
                if (retryBend && !intrinsicExit) continue;
            }
            if (left && !intrinsicExit && !project(turns[4], turns[3].point.y() + form.value("exitDrop").toDouble(0.05), false, l)) {
                auto foot = context.timeSorted.find(turns[4]);
                for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                    const auto next = (context.*advance)(foot);
                    if (next->point.y() <= foot->point.y()) break;
                    foot = next;
                }
                const double y = foot->point.y() - form.value("exitDrop").toDouble(.05);
                if (y <= turns[4].point.y() || !project(turns[4], y, false, l)) {
                    const double radius = form.value("maxExitBendDistance").toDouble() * (pitch1 + pitch2) / 2;
                    if (!seenExitBendCut(context, turns[4], advance == &SearchContext::prev, radius, l)) continue;
                }
            }
            // A closed loop can have a crest resembling a third tooth. Check
            // the proposed body itself so a following loop is not swallowed.
            std::vector<Stretching> cuts;
            if (right) cuts.push_back(r);
            if (left) cuts.push_back(l);
            const auto pieces = cutRegions(ink, {}, cuts, context.orientation, true);
            if (pieces.size() != cuts.size() + 1 ||
                toSubpaths(pieces[right ? 1 : 0].path).size() != 1) continue;
            if (form.value("terminalBowl").toBool()) {
                const auto tail = std::prev(context.xSorted.end());
                const auto part = getJoinTime(r, context.orientation);
                const auto bounds = pieces[1].path.boundingRect();
                const auto pitch = (pitch1 + pitch2) / 2;
                if (!part.isStrictLeft(tail->time) || bounds.bottom() <= turns[4].point.y() + pitch ||
                    turns[4].point.x() <= tail->point.x() + pitch || !part.isStrictLeft(turns[0].time))
                    continue;
                // The third tooth must feed the terminal bowl, without a fourth intervening tooth.
                auto down = context.timeSorted.find(turns[4]);
                for (size_t i = 0; i < context.timeSorted.size(); ++i) {
                    const auto step = (context.*advance)(down);
                    if (step->point.y() < down->point.y() - .03)
                        break;
                    down = step;
                }
                if (down->point.y() < bounds.bottom() - .8 * pitch)
                    continue;
            }
            if (match && first->point.x() <= firstX) continue;
            firstX = first->point.x();
            if (matchedTips) *matchedTips={turns[0],turns[2],turns[4]};
            match = std::pair{r, l};
        }
    }
    if (!match) return false;
    if (right) *right = match->first;
    if (left) *left = match->second;
    return true;
}

static bool searchSeenTeeth(SearchContext& context, const SeenExitRetry* retry = nullptr) {
    const bool final = context.form.value("side").toString() == "right";
    const double maxX = context.subword.joins.empty() ? context.bbox.right() + 1e-6
        : context.subword.joins.rbegin()->bottomBaseLine.point.x();
    Stretching cut{};
    if (!seenTeethBoundaries(context, context.form, context.bbox.left(),
                             context.form.value("terminalBowl").toBool() ? context.bbox.right() + 1e-6 : maxX,
                             final ? &cut : nullptr, final ? nullptr : &cut, nullptr, nullptr, retry))
        return false;
    context.res = cut;
    context.res.topLeft = context.res.topRight = cut.topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = cut.bottomBaseLine;
    context.res.fixedRightBoundary = !final;
    context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto& point) {
        return point.point.x() < cut.topBaseLine.point.x();
    });
    return true;
}

static bool searchInitialHeh(SearchContext& context) {
    const auto& ink = context.transformedPath;
    std::vector<QRectF> counters;
    for (size_t i = 1; i < context.subPaths.size(); ++i) {
        const auto hole = context.subPaths[i].boundingRect();
        if (hole.width() > 0 && hole.height() > 0 && !ink.contains(hole.center()))
            counters.push_back(hole);
    }
    if (counters.size() < 2) return false;
    std::sort(counters.begin(), counters.end(), [](const auto& a, const auto& b) {
        return a.center().x() > b.center().x();
    });
    const auto& right = counters[0];
    const auto& left = counters[1];
    const double height = (right.height() + left.height()) / 2;
    if (right.left() - left.right() > height ||
        std::min(right.bottom(), left.bottom()) <= std::max(right.top(), left.top())) return false;

    const double x = left.left() - height * formNumber(context, "counterOffset", 0.35);
    const double y = std::max(right.bottom(), left.bottom()) +
        height * formNumber(context, "baselineOffset", 0.6);
    Stretching cut{};
    if (!medialChordAt(context, x, y, height, cut)) return false;
    context.res = cut;
    context.res.topLeft = context.res.topRight = cut.topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = cut.bottomBaseLine;
    context.res.fixedRightBoundary = true;
    context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto& point) {
        return point.point.x() < x;
    });
    return true;
}

// Anchor a loop's incoming boundary at its own neck. Most loops use the lower
// inward corner; suspended loops and waw can use the upper attachment instead.
// A loop's right corner pair consists of its inward upper shoulder and the
// lower valley below the counter. The pair supplies a local stroke section
// for an optional outward offset, without borrowing a neighboring landmark.
// At a corner, use the tangent leaving toward the join, not into the body.
struct CounterCornerBody {
    PathExtrema anchor;
    QPointF tangent;
    double distance;
    std::vector<PathExtrema> oppositeCorners;
};
struct CounterCornerEvidence {
    QRectF counter;
    double maxDistance = 0, upperLimit = 0;
    bool upper = false, top = false, paired = false;
    std::vector<CounterCornerBody> corners;
};

static CounterCornerEvidence recognizeCounterCorners(const InkOutline& outline, const QRectF& counter,
                                                       const QJsonObject& settings) {
    CounterCornerEvidence result;
    result.counter = counter;
    if (settings.isEmpty()) return result;
    result.maxDistance = settings.value("maxDistance").toDouble(1.5) * counter.height();
    result.upper = settings.value("placement") == "upper";
    result.top = settings.value("placement") == "top-right";
    result.paired = settings.value("placement") == "right-corner-pair";
    result.upperLimit = counter.top() + settings.value("maxCounterY").toDouble() * counter.height();
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    const auto curves = baseContourCurves(outline.ink);
    if (curves.empty()) return result;
    for (auto at = geometry.timeSorted.begin(); at != geometry.timeSorted.end(); ++at) {
        const auto p = at->point;
        const double distance = QLineF(p, result.upper || result.top ? counter.topRight() : counter.bottomRight()).length();
        if ((!at->isSharp && !(result.top && at->isY)) || distance >= result.maxDistance) continue;
        CounterCornerBody body{*at, {}, distance, {}};
        if (result.paired) {
            if (p.x() < counter.center().x() || p.y() < counter.bottom() ||
                p.x() >= geometry.prev(at)->point.x() || p.x() >= geometry.next(at)->point.x()) continue;
            for (auto lower = geometry.timeSorted.begin(); lower != geometry.timeSorted.end(); ++lower) {
                const auto q = lower->point;
                if (!lower->isSharp || q.x() < counter.left() || q.x() >= p.x() || q.y() <= p.y() ||
                    q.y() >= geometry.prev(lower)->point.y() || q.y() >= geometry.next(lower)->point.y() ||
                    QLineF(p, q).length() >= result.maxDistance || !interiorChord(outline.ink, *at, *lower)) continue;
                body.oppositeCorners.push_back(*lower);
            }
            result.corners.push_back(std::move(body));
            continue;
        }
        if (result.top) {
            if (p.x() <= counter.right() || p.y() <= geometry.prev(at)->point.y() || p.y() <= geometry.next(at)->point.y()) continue;
        } else if (result.upper ? (p.x() < counter.center().x() || p.y() > result.upperLimit) :
            (p.x() <= counter.right() || p.y() < counter.center().y() ||
             p.y() >= geometry.prev(at)->point.y() || p.y() >= geometry.next(at)->point.y())) continue;
        const int count = static_cast<int>(curves.size());
        if (std::abs(at->time - std::round(at->time)) <= 1e-6) {
            const int index = static_cast<int>(std::lround(at->time)) % count;
            const auto& previous = curves[(index + count - 1) % count];
            const auto& next = curves[index];
            auto a = previous.pt3() - previous.pt4(), b = next.pt2() - next.pt1();
            if (std::hypot(a.x(), a.y()) <= 1e-9) a = previous.pointAt(1 - 1e-4) - p;
            if (std::hypot(b.x(), b.y()) <= 1e-9) b = next.pointAt(1e-4) - p;
            body.tangent = a.x() > b.x() ? a : b;
        } else {
            const int index = static_cast<int>(std::floor(at->time)) % count;
            const double t = at->time - std::floor(at->time), u = 1 - t;
            const auto& curve = curves[index];
            body.tangent = u * u * (curve.pt2() - curve.pt1()) +
                2 * u * t * (curve.pt3() - curve.pt2()) + t * t * (curve.pt4() - curve.pt3());
            if (body.tangent.x() < 0) body.tangent = -body.tangent;
        }
        if (body.tangent.x() > 0 && std::hypot(body.tangent.x(), body.tangent.y()) > 1e-9)
            result.corners.push_back(std::move(body));
    }
    return result;
}

static bool cutCounterCorners(SearchContext& context, const CounterCornerEvidence& evidence,
                               const QJsonObject& policy, bool defaultNormal, Stretching& cut) {
    const bool normalCut = policy.value("normalCut").toBool(defaultNormal);
    const auto& ink = context.transformedPath;
    const auto& counter = evidence.counter;
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    double nearest = evidence.maxDistance;
    bool found = false;
    for (const auto& body : evidence.corners) {
        if (body.distance >= nearest) continue;
        const auto& corner = body.anchor;
        const auto p = corner.point;
        if (evidence.paired) {
            std::optional<Stretching> paired;
            double closest = evidence.maxDistance;
            for (const auto& lower : body.oppositeCorners) {
                const double gap = QLineF(p, lower.point).length();
                if (gap >= closest) continue;
                Stretching proposal{};
                proposal.topBaseLine = corner; proposal.bottomBaseLine = lower;
                const double offset = policy.value("strokeOffsetX").toDouble();
                if (offset != 0) {
                    const auto center = QLineF(p, lower.point).center();
                    if (!medialChordAt(context, center.x() + offset * gap, center.y(), 2 * gap, proposal)) continue;
                }
                if (normalCut && !strokeNormalCut(context, proposal)) continue;
                paired = proposal; closest = gap;
            }
            if (paired) { cut = *paired; nearest = body.distance; found = true; }
            continue;
        }
        const double length = std::hypot(body.tangent.x(), body.tangent.y());
        const QPointF normal = (evidence.top ? -1 : 1) *
            (normalCut ? QPointF(body.tangent.y(), -body.tangent.x()) / length : QPointF(0, -1));
        const auto hits = FindInterections(ink, QLineF(p, p + normal * reach), true);
        std::optional<PathExtrema> top;
        double width = reach;
        for (const auto& hit : hits) {
            const double d = QLineF(p, hit.point).length();
            if (d < width && (evidence.top ? hit.point.y() > p.y() : hit.point.y() < p.y()) && interiorChord(ink, hit, corner)) {
                top = hit; width = d;
            }
        }
        if (!top || (!evidence.top && (evidence.upper ? std::max(top->point.y(), p.y()) > evidence.upperLimit :
                     std::min(top->point.x(), p.x()) <= counter.right()))) continue;
        cut.topBaseLine = evidence.top ? corner : *top;
        cut.bottomBaseLine = evidence.top ? *top : corner;
        nearest = body.distance; found = true;
    }
    return found;
}

static bool counterEntryCorner(SearchContext& context, const QRectF& counter,
                               const QJsonObject& settings, Stretching& cut) {
    const auto corner = settings.value("entryCorner").toObject();
    return cutCounterCorners(context, recognizeCounterCorners(inkOutline(context), counter, corner),
        corner, settings.value("normalCut").toBool(), cut);
}

// The outgoing neck is the reflected version of the same intrinsic corner
// feature. Reflection preserves Bezier times, so the resulting cut still
// addresses the original contour. No neighboring boundary supplies its anchor.
static bool counterExitCorner(SearchContext& context, const QRectF& counter,
                              const QJsonObject& settings, Stretching& cut) {
    const auto exit = settings.value("exitCorner").toObject();
    if (exit.isEmpty()) return false;
    const QTransform reflection(-1, 0, 0, 1, 0, 0);
    SearchContext mirrored{.transformedPath = reflection.map(context.transformedPath), .subword = context.subword};
    mirrored.bbox = reflection.mapRect(context.bbox);
    for (auto point : context.timeSorted) {
        point.point = reflection.map(point.point);
        mirrored.timeSorted.insert(point);
    }
    auto reflectedSettings = settings;
    reflectedSettings.insert("entryCorner", exit);
    if (!counterEntryCorner(mirrored, reflection.mapRect(counter), reflectedSettings, cut)) return false;
    cut.topBaseLine.point = reflection.map(cut.topBaseLine.point);
    cut.bottomBaseLine.point = reflection.map(cut.bottomBaseLine.point);
    return true;
}

struct CounterHole {
    size_t index;
    QRectF box;
};

static std::vector<CounterHole> recognizeCounterHoles(const InkOutline& outline) {
    std::vector<CounterHole> result;
    for (size_t i = 1; i < outline.contours.size(); ++i) {
        const auto box = outline.contours[i].boundingRect();
        if (!outline.ink.contains(box.center()) && box.width() > 0 && box.height() > 0)
            result.push_back({i, box});
    }
    return result;
}

static bool cutFinalCounterBody(SearchContext& context, const QRectF& counter,
                                const CounterCornerEvidence& entry, const QJsonObject& bounds) {
    const auto& ink = context.transformedPath;
    const double offset = bounds.value("offset").toDouble() * counter.height();
    const double x = counter.right() + offset;
    const double y = counter.bottom() + bounds.value("baselineOffset").toDouble() * counter.height();
    Stretching cut{};
    if (!cutCounterCorners(context, entry, bounds.value("entryCorner").toObject(), bounds.value("normalCut").toBool(), cut)) {
        if (bounds.value("entryCorner").toObject().value("required").toBool()) return false;
        if (!medialChordAt(context, x, y, counter.height() * 2, cut)) return false;
        if (bounds.value("normalCut").toBool() && !strokeNormalCut(context, cut)) return false;
    }
    const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
    if (pieces.size() != 2) return false;
    const auto body = pieces[1].path.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
    if (!body.contains(counter) || !body.contains(std::prev(context.xSorted.end())->point) ||
        body.contains(context.xSorted.begin()->point)) return false;
    context.res = cut;
    context.res.topLeft = context.res.topRight = cut.topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = cut.bottomBaseLine;
    return true;
}

static bool searchFinalCounter(SearchContext& context) {
    const auto bounds = context.form.value("counter").toObject();
    const auto holes = recognizeCounterHoles(inkOutline(context));
    if (holes.empty()) return false;
    const auto found = std::min_element(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
        return a.box.center().x() < b.box.center().x();
    });
    const auto evidence = recognizeCounterCorners(inkOutline(context), found->box, bounds.value("entryCorner").toObject());
    return cutFinalCounterBody(context, found->box, evidence, bounds);
}

// The initial loop ends at the inward corner below and to the left of its
// counter. Locate that corner from the loop itself, independent of the next letter.
struct InitialCounterBody {
    QRectF counter;
    std::vector<PathExtrema> valleys, normalAnchors;
};

static std::optional<InitialCounterBody> recognizeInitialCounterBody(const InkOutline& outline,
                                                                     const QJsonObject& settings) {
    const auto holes = recognizeCounterHoles(outline);
    if (holes.empty()) return {};
    const auto found = std::max_element(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
        return a.box.center().x() < b.box.center().x();
    });
    const auto& counter = found->box;
    if (outline.bounds.right() - counter.right() > settings.value("maxOuterDistance").toDouble(1) * counter.height())
        return {};
    InitialCounterBody result{counter, {}, {}};
    WordResultInfo::SubWordInfo empty;
    const auto geometry = outline.geometry(empty);
    const double radius = settings.value("maxNeckDistance").toDouble(1.5) * counter.height();
    for (auto corner = geometry.timeSorted.begin(); corner != geometry.timeSorted.end(); ++corner) {
        const auto p = corner->point;
        if (QLineF(p, counter.bottomLeft()).length() >= radius) continue;
        if (p.x() <= counter.left() && p.y() >= counter.center().y() &&
            p.y() < geometry.prev(corner)->point.y() && p.y() < geometry.next(corner)->point.y())
            result.valleys.push_back(*corner);
        if (corner->isSharp && p.x() < counter.left()) result.normalAnchors.push_back(*corner);
    }
    return result;
}

static bool cutInitialCounterBody(SearchContext& context, const InitialCounterBody& body,
                                   const QJsonObject& settings) {
    const auto& ink = context.transformedPath;
    const auto* counter = &body.counter;
    std::optional<Stretching> best;
    double nearest = settings.value("maxNeckDistance").toDouble(1.5) * counter->height();
    for (const auto& landmark : body.valleys) {
        const auto* corner = &landmark;
        const auto p = corner->point;
        const double distance = QLineF(p, counter->bottomLeft()).length();
        if (distance >= nearest) continue;
        const auto hits = FindInterections(ink, QLineF(p, QPointF(p.x(), context.bbox.top() - 1)), true);
        for (const auto& top : hits) {
            if (top.point.y() >= p.y() || !interiorChord(ink, top, *corner)) continue;
            Stretching cut{};
            cut.topBaseLine = top;
            cut.bottomBaseLine = *corner;
            best = cut;
            nearest = distance;
            break;
        }
    }
    if (!best && settings.value("normalNeck").toBool()) {
        double closest = settings.value("maxNeckDistance").toDouble(1.5) * counter->height();
        for (const auto& corner : body.normalAnchors) {
            const double distance = QLineF(corner.point, counter->bottomLeft()).length();
            if (distance >= closest) continue;
            for (bool previous : {false, true}) {
                Stretching cut{};
                if (!contourNormalCut(context, corner, previous, false, cut)) continue;
                const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
                if (pieces.size() != 2 || !pieces[0].path.boundingRect().contains(*counter) ||
                    pieces[0].path.boundingRect().left() < counter->left() -
                        settings.value("maxNeckDistance").toDouble(1.5) * counter->height()) continue;
                best = cut; closest = distance;
            }
        }
    }
    if (!best) return false;
    context.res = *best;
    context.res.topLeft = context.res.topRight = best->topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = best->bottomBaseLine;
    context.res.fixedRightBoundary = true;
    context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto& point) {
        return point.point.x() < best->bottomBaseLine.point.x();
    });
    return true;
}

static bool searchInitialCounter(SearchContext& context) {
    const auto settings = context.form.value("counter").toObject();
    const auto body = recognizeInitialCounterBody(inkOutline(context), settings);
    return body && cutInitialCounterBody(context, *body, settings);
}

// Initial feh and qaf share a loop suspended above their outgoing stroke.
// Its own counter identifies the head; its outer left extremum locates the
// outgoing boundary. Neither the following letter nor its cut supplies a
// landmark, baseline, or measurement scale.
struct LoopHeadBody {
    QRectF counter;
    std::optional<PathExtrema> edge;
    struct Neck { PathExtrema crest; double distance; std::optional<SavedContourRoute> route; };
    std::vector<Neck> necks;
};

static std::optional<LoopHeadBody> recognizeLoopHead(const InkOutline& outline, const QJsonObject& settings) {
    const auto holes = recognizeCounterHoles(outline);
    if (holes.empty()) return {};
    const auto selected = std::max_element(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
        return a.box.center().x() < b.box.center().x();
    });
    LoopHeadBody result{selected->box};
    const auto* counter = &result.counter;
    const double scale = counter->height();
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    const auto route = settings.value("neckWalk").toObject();
    if (!route.isEmpty()) {
        for (auto crest = geometry.timeSorted.begin(); crest != geometry.timeSorted.end(); ++crest) {
            if (crest->point.y() >= counter->top() || crest->point.y() >= geometry.prev(crest)->point.y() ||
                crest->point.y() >= geometry.next(crest)->point.y()) continue;
            const double distance = QLineF(crest->point, QPointF(counter->center().x(), counter->top())).length();
            if (distance >= 3 * scale) continue;
            result.necks.push_back({*crest, distance, recognizeSavedRoute(geometry, route, &*crest)});
        }
        return result;
    }
    double nearest = settings.value("maxEdgeDistance").toDouble(1.5) * scale;
    const QPointF counterLeft(counter->left(), counter->center().y());
    for (auto point = geometry.timeSorted.begin(); point != geometry.timeSorted.end(); ++point) {
        const auto p = point->point;
        const double distance = QLineF(p, counterLeft).length();
        if (p.x() >= counter->left() || p.x() >= geometry.prev(point)->point.x() ||
            p.x() >= geometry.next(point)->point.x() || distance >= nearest) continue;
        result.edge = *point; nearest = distance;
    }
    return result;
}

static bool cutLoopHead(SearchContext& context, const LoopHeadBody& evidence,
                        const QJsonObject& bodySettings, const QJsonObject& policy) {
    const auto& ink = context.transformedPath;
    const auto settings = mergeShapeSettings(bodySettings, policy);
    const auto* counter = &evidence.counter;
    const double scale = counter->height();
    const auto neckWalk = bodySettings.value("neckWalk").toObject();
    if (!neckWalk.isEmpty()) {
        // A descending loop has a return bend rather than a horizontal neck
        // below its left edge. Anchor the same route engine on this loop's own
        // crest; a neighboring ascender must not become its starting point.
        std::optional<Stretching> best;
        double nearest = 3 * scale;
        for (const auto& candidate : evidence.necks) {
            const auto* crest = &candidate.crest;
            const double distance = candidate.distance;
            if (distance >= nearest) continue;
            context.res = {};
            if (!projectSavedRoute(context, candidate.route, bodySettings.value("neckWalk").toObject(),
                    policy.value("neckWalk").toObject())) continue;
            const auto cut = context.res;
            const QLineF chord(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (std::min(chord.p1().y(), chord.p2().y()) <= counter->bottom() ||
                chord.length() > 3 * scale ||
                chord.center().y() - counter->bottom() > settings.value("maxNeckDistance").toDouble(3) * scale) continue;
            const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
            if (pieces.size() != 2 || toSubpaths(pieces[0].path).size() != 2) continue;
            const auto body = pieces[0].path.boundingRect();
            if (!body.contains(*counter) || body.left() < counter->left() - 2 * scale ||
                !body.adjusted(-1e-6,-1e-6,1e-6,1e-6).contains(crest->point) || body.height() > 6 * scale) continue;
            best = cut; nearest = distance;
        }
        if (!best) return false;
        context.res = *best;
        context.res.fixedRightBoundary = true;
        return true;
    }

    const auto& edge = evidence.edge;
    if (!edge) return false;

    std::optional<Stretching> boundary;
    const double maxInset = settings.value("maxProjectionInset").toDouble(2) * scale;
    const double rimWidth = counter->left() - edge->point.x();
    // A bent neck may not reach the head's exact left edge. Search only a
    // bounded strip inside that head, never following the connecting stroke
    // toward another letter. A cusp thinner than the head's own rim is not
    // a usable neck cross-section.
    for (int sample = 0; sample <= 16 && !boundary; ++sample) {
        const double probeX = edge->point.x() + maxInset * sample / 16;
        const auto hits = FindInterections(ink, QLineF(probeX, edge->point.y(),
                                                       probeX, context.bbox.bottom() + 1));
        double distance = settings.value("maxNeckDistance").toDouble(3) * scale;
        for (size_t i = 1; i < hits.size(); ++i) {
            const auto& top = hits[i - 1];
            const auto& bottom = hits[i];
            if (top.pathIndex != 0 || bottom.pathIndex != 0 ||
                top.point.y() <= edge->point.y() || top.point.y() < counter->bottom() ||
                !interiorChord(ink, top, bottom)) continue;
            const double gap = QLineF(top.point, bottom.point).center().y() - edge->point.y();
            if (gap >= distance) continue;
            Stretching cut{};
            cut.topBaseLine = top;
            cut.bottomBaseLine = bottom;
            if (settings.value("normalCut").toBool() && !strokeNormalCut(context, cut)) continue;
            if (QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length() < rimWidth ||
                std::min(cut.topBaseLine.point.y(), cut.bottomBaseLine.point.y()) < counter->bottom()) continue;
            // A chord across a small spur can also be normal to its ink. It
            // is not this head's neck: the resulting initial region must
            // retain the loop and stay inside the head's left envelope.
            const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
            if (pieces.size() != 2) continue;
            const auto bodyBounds = pieces.front().path.boundingRect();
            const QPointF rimSample((edge->point.x() + counter->left()) / 2, counter->center().y());
            if (!bodyBounds.contains(*counter) || bodyBounds.left() < edge->point.x() - rimWidth ||
                !pieces.front().path.contains(rimSample)) continue;
            boundary = cut;
            distance = gap;
        }
    }
    if (!boundary) return false;
    context.res = *boundary;
    context.res.topLeft = context.res.topRight = boundary->topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = boundary->bottomBaseLine;
    context.res.fixedRightBoundary = true;
    const double x = QLineF(boundary->topBaseLine.point, boundary->bottomBaseLine.point).center().x();
    context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto& point) {
        return point.point.x() < x;
    });
    return true;
}

// Projected tooth boundaries retain their intrinsic tip and bend for the
// assembler to verify that the complete body survives its neighboring cuts.
struct ToothBody {
    PathExtrema tip;
    PathExtrema bend;
    Stretching left;
    std::optional<Stretching> right;
    QVector<QPointF> curvedAxis;
    bool retainedBy(const QPainterPath& mask) const {
        if (!curvedAxis.isEmpty())
            return std::all_of(curvedAxis.begin(), curvedAxis.end(),
                               [&](const auto& point) { return mask.contains(point); });
        const QLineF axis(tip.point, bend.point);
        return mask.contains(axis.pointAt(.05)) && mask.contains(axis.pointAt(.95));
    }
};

static std::vector<ToothBody> loopExitToothBoundaries(SearchContext& context, const QJsonObject& settings);

// A loop's tooth can have a descending spur on the underside. A normal
// through the tooth root may land on the wrong side of that spur. Use the
// narrow section at the return corner when it keeps both tips on the body.
static Stretching loopToothExit(const SearchContext& context, const ToothBody& tooth) {
    const auto& cut = tooth.left;
    const auto& upper = cut.topBaseLine.point;
    const auto& lower = cut.bottomBaseLine.point;
    if (lower.x() <= tooth.tip.point.x()) return cut;
    const auto previous = getJoinTime(cut, context.orientation);
    const double cutLength = QLineF(upper, lower).length();
    double nearest = cutLength;
    Stretching result = cut;
    for (const auto& corner : context.timeSorted) {
        const auto p = corner.point;
        const double gap = std::abs(p.x() - upper.x());
        if (!corner.isSharp || !corner.isY || gap >= nearest || p.x() >= tooth.tip.point.x() ||
            p.y() <= tooth.bend.point.y() || !previous.isStrictLeft(corner.time)) continue;
        const auto hits = FindInterections(context.transformedPath,
            QLineF(p, QPointF(p.x(), context.bbox.top() - cutLength)), true);
        for (const auto& top : hits) {
            const double width = p.y() - top.point.y();
            if (width <= 1e-6 || width * 2 >= cutLength || gap > width ||
                !interiorChord(context.transformedPath, top, corner)) continue;
            auto candidate = cut;
            candidate.topBaseLine = top;
            candidate.bottomBaseLine = corner;
            candidate.referenceWidth = width;
            const auto side = getJoinTime(candidate, context.orientation);
            if (!side.isStrictRight(cut.bottomBaseLine.time) ||
                !side.isStrictRight(tooth.tip.time) || !side.isStrictRight(tooth.bend.time)) continue;
            const bool retainsSpur = std::any_of(context.timeSorted.begin(), context.timeSorted.end(), [&](const auto& point) {
                return point.isY && point.point.x() > p.x() && point.point.x() < lower.x() &&
                    point.point.y() > std::max(p.y(), lower.y()) + width &&
                    previous.isStrictLeft(point.time) && side.isStrictRight(point.time);
            });
            if (!retainsSpur) continue;
            nearest = gap;
            result = candidate;
            break;
        }
    }
    return result;
}

struct LoopBodyEvidence {
    CounterHole anchor;
    QRectF hole;
    double bandY = -1;
    bool valid = false;
    CounterCornerEvidence entry, exit;
};

static SearchContext reflectedCounterContext(const InkOutline& outline, WordResultInfo::SubWordInfo& empty) {
    const QTransform reflection(-1, 0, 0, 1, 0, 0);
    SearchContext result{.transformedPath = reflection.map(outline.ink), .subword = empty};
    result.bbox = reflection.mapRect(outline.bounds);
    for (auto p : outline.time) {
        p.point = reflection.map(p.point);
        result.timeSorted.insert(p);
    }
    return result;
}

static LoopBodyEvidence recognizeLoopBody(const InkOutline& outline, const QJsonObject& settings,
                                          const CounterHole& anchor) {
    LoopBodyEvidence result{anchor};
    WordResultInfo::SubWordInfo empty;
    auto context = outline.geometry(empty);
    const auto& ink = context.transformedPath;
    const auto occurrence = anchor.index;
    auto hole = context.subPaths[occurrence].boundingRect();
    if (settings.value("counterPair") == "horizontal") {
        std::optional<QRectF> partner;
        double nearest = hole.height();
        for (size_t i = 1; i < context.subPaths.size(); ++i) {
            if (i == occurrence) continue;
            const auto other = context.subPaths[i].boundingRect();
            const double gap = hole.left() - other.right();
            if (gap >= 0 && gap < nearest && !ink.contains(other.center()) &&
                std::min(hole.bottom(), other.bottom()) > std::max(hole.top(), other.top())) {
                partner = other; nearest = gap;
            }
        }
        if (!partner) return result;
        hole = hole.united(*partner);
    }
    double bandY = -1;
    if (settings.value("counterPair") == "vertical") {
        std::optional<QRectF> partner;
        double nearest = context.bbox.height();
        for (size_t i = 1; i < context.subPaths.size(); ++i) {
            if (i == occurrence) continue;
            const auto other = context.subPaths[i].boundingRect();
            const double gap = std::max(other.top() - hole.bottom(), hole.top() - other.bottom());
            if (gap >= 0 && gap < nearest && gap <= settings.value("maxCounterGap").toDouble(2) *
                std::max(hole.height(), other.height()) && !ink.contains(other.center()) &&
                std::min(hole.right(), other.right()) > std::max(hole.left(), other.left())) {
                partner = other; nearest = gap;
            }
        }
        if (!partner) return result;
        bandY = partner->top() > hole.top() ? (hole.bottom() + partner->top()) / 2 :
            (partner->bottom() + hole.top()) / 2;
        hole = hole.united(*partner);
    }
    if (hole.width() < settings.value("minAspect").toDouble() * hole.height()) return result;
    for (size_t j = 1; j < context.subPaths.size(); ++j) {
        if (j == occurrence) continue;
        const auto other = context.subPaths[j].boundingRect();
        // Stacked counters belong to a different body shape, such as medial heh.
        if (!ink.contains(other.center()) && !hole.contains(other) && other.left() < hole.right() &&
            other.right() > hole.left()) return result;
    }

    result.hole = hole; result.bandY = bandY; result.valid = true;
    result.entry = recognizeCounterCorners(outline, hole, settings.value("entryCorner").toObject());
    auto reflected = reflectedCounterContext(outline, empty);
    result.exit = recognizeCounterCorners(inkOutline(reflected), QTransform(-1,0,0,1,0,0).mapRect(hole),
        settings.value("exitCorner").toObject());
    return result;
}

static std::vector<LoopBodyEvidence> recognizeLoopBodies(const InkOutline& outline, const QJsonObject& body) {
    std::vector<LoopBodyEvidence> result;
    for (const auto& hole : recognizeCounterHoles(outline)) result.push_back(recognizeLoopBody(outline, body, hole));
    return result;
}

static bool cutLoopExitCorner(SearchContext& context, const CounterCornerEvidence& evidence,
                              const QJsonObject& policy, Stretching& cut) {
    WordResultInfo::SubWordInfo empty;
    auto reflected = reflectedCounterContext(inkOutline(context), empty);
    if (!cutCounterCorners(reflected, evidence, policy.value("exitCorner").toObject(), policy.value("normalCut").toBool(), cut))
        return false;
    const QTransform reflection(-1,0,0,1,0,0);
    cut.topBaseLine.point = reflection.map(cut.topBaseLine.point);
    cut.bottomBaseLine.point = reflection.map(cut.bottomBaseLine.point);
    return true;
}

static std::optional<std::pair<Stretching, Stretching>> cutLoopBody(
        SearchContext& context, const LoopBodyEvidence& body, const QJsonObject& settings, bool needEntry) {
    if (!body.valid) return {};
    const auto& ink = context.transformedPath;
    const auto& hole = body.hole;
    const auto occurrence = body.anchor.index;
    const auto bandY = body.bandY;
    const double offset = settings.value("sideOffset").toDouble(0.5) * hole.height();
    const double y = bandY >= 0 ? bandY : hole.bottom() + settings.value("baselineOffset").toDouble(.3) * hole.height();
    Stretching right{}, left{};
    const auto entryWalk = settings.value("entryWalk").toObject();
    const auto exitTooth = settings.value("exitTooth").toObject();
    bool anchoredRight = false;
    if (needEntry && !entryWalk.isEmpty()) {
        const auto originalForm = context.form;
        const auto originalResult = context.res;
        const auto originalNext = context.nextIter;
        context.form = entryWalk;
        double nearest = hole.height() * 2;
        // The counter supplies this body's crest; neither neighboring cut
        // supplies a landmark or a measurement scale for the route.
        for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest) {
            const auto p = crest->point;
            const double gap = hole.top() - p.y();
            if (p.x() < hole.left() || p.x() > hole.right() || gap < 0 || gap >= nearest ||
                p.y() >= context.prev(crest)->point.y() || p.y() >= context.next(crest)->point.y()) continue;
            context.res = {};
            if (!searchContourWalk(context, &*crest)) continue;
            right = context.res;
            nearest = gap;
            anchoredRight = true;
        }
        context.form = originalForm;
        context.res = originalResult;
        context.nextIter = originalNext;
        if (!anchoredRight) return {};
    } else if (needEntry) {
        anchoredRight = cutCounterCorners(context, body.entry, settings.value("entryCorner").toObject(), settings.value("normalCut").toBool(), right);
        if (!anchoredRight && settings.value("entryCorner").toObject().value("required").toBool()) return {};
    }
    const double entryMinY = settings.value("entryBelowCounter").toBool() ? hole.bottom() :
        -std::numeric_limits<double>::infinity();
    if (needEntry && !anchoredRight &&
        !medialChordAt(context, hole.right() + offset, y, hole.height() * 2, right, false, entryMinY)) return {};
    const bool anchoredLeft = exitTooth.isEmpty() && cutLoopExitCorner(context, body.exit, settings, left);
    if (!anchoredLeft && settings.value("exitCorner").toObject().value("required").toBool()) return {};
    if (!exitTooth.isEmpty()) {
        bool found = false;
        double nearest = exitTooth.value("maxGap").toDouble(1.5) * hole.height();
        for (const auto& tooth : loopExitToothBoundaries(context, exitTooth)) {
            const double gap = hole.left() - std::max(tooth.tip.point.x(), tooth.bend.point.x());
            if (gap < 0 || gap >= nearest || tooth.tip.point.y() >= hole.bottom() ||
                (needEntry && !Stretching::cmpbyx{}(right, tooth.left))) continue;
            left = loopToothExit(context, tooth);
            nearest = gap;
            found = true;
        }
        if (!found) return {};
    } else if (!anchoredLeft && !medialChordAt(context, hole.left() - offset, y, hole.height() * 2, left)) return {};
    if (settings.value("normalCut").toBool() &&
        ((needEntry && !anchoredRight && !strokeNormalCut(context, right)) ||
         (!anchoredLeft && !strokeNormalCut(context, left)))) return {};
    if (needEntry && std::min(right.topBaseLine.point.y(), right.bottomBaseLine.point.y()) < entryMinY) return {};
    if (settings.contains("maxBodyHeight")) {
        const auto cuts = needEntry ? std::vector<Stretching>{right, left} : std::vector<Stretching>{left};
        const auto pieces = cutRegions(ink, {}, cuts, context.orientation, true);
        if (pieces.size() != cuts.size() + 1 || pieces[needEntry ? 1 : 0].path.boundingRect().height() >
            settings.value("maxBodyHeight").toDouble() * hole.height()) return {};
    }
    // Rotation must leave the entire enclosed counter in the body.
    if (!needEntry) {
        const auto pieces = cutRegions(ink, {}, {left}, context.orientation, true);
        if (pieces.size() != 2 || !pieces[0].path.boundingRect().contains(hole)) return {};
    } else if (settings.contains("exitCorner")) {
        // A suspended loop's incoming cut can overlap its counter in x while
        // staying above it. Check ownership of the actual closed contour,
        // rather than rejecting an otherwise valid diagonal neck by its box.
        const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3) return {};
        const auto contours = toSubpaths(pieces[1].path);
        if (std::none_of(contours.begin(), contours.end(), [&](const auto& p) {
                return p == context.subPaths[occurrence];
            })) return {};
    } else if (entryWalk.isEmpty() && exitTooth.isEmpty() && !settings.contains("counterPair")) {
        if (std::min(right.topBaseLine.point.x(), right.bottomBaseLine.point.x()) <= hole.right() ||
            std::max(left.topBaseLine.point.x(), left.bottomBaseLine.point.x()) >= hole.left()) return {};
    } else {
        const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3 || !pieces[1].path.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(hole)) return {};
    }
    return std::pair{right, left};
}


// Slot selection is assembly: unavailable or out-of-order counters cannot
// erase their independently recognized features. Never skip the first loop
// just because its projection fails, which would borrow a later letter.
static const LoopBodyEvidence* selectLoopBody(const SearchContext& context,
        const std::vector<LoopBodyEvidence>& bodies, double minX, double maxX, const QPainterPath* availableInk) {
    const auto availableContours = availableInk ? toSubpaths(*availableInk) : QVector<QPainterPath>{};
    const LoopBodyEvidence* selected = nullptr;
    double rightmost = minX;
    for (const auto& body : bodies) {
        if (availableInk && std::none_of(availableContours.begin(), availableContours.end(),
                [&](const auto& contour) { return contour == context.subPaths[body.anchor.index]; })) continue;
        const double x = body.anchor.box.center().x();
        if (x < maxX && x > rightmost) { selected = &body; rightmost = x; }
    }
    return selected;
}

static std::optional<std::pair<Stretching, Stretching>> medialLoopBoundaries(
        SearchContext& context, const QJsonObject& settings, double minX, double maxX, bool needEntry = true,
        const QPainterPath* availableInk = nullptr) {
    const auto bodies = recognizeLoopBodies(inkOutline(context), settings);
    const auto body = selectLoopBody(context, bodies, minX, maxX, availableInk);
    return body ? cutLoopBody(context, *body, settings, needEntry) : std::nullopt;
}

// The right face of a tall tooth descends to its own baseline turn. Use the
// local pen thickness to distinguish that face from a shallow joining stroke.
struct ToothEntryLandmark {
    PathExtrema foot;
    bool forward;
};

static std::vector<ToothEntryLandmark> recognizeToothEntries(SearchContext& context,
        const PathExtrema& tip, double stroke, const QJsonObject& entry) {
    std::vector<ToothEntryLandmark> result;
    if (entry.isEmpty() || stroke <= 0) return result;
    const auto peak = context.timeSorted.find(tip);
    if (peak == context.timeSorted.end()) return result;
    for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
        auto foot = peak;
        for (size_t step = 1; step < context.timeSorted.size(); ++step) {
            const auto next = (context.*advance)(foot);
            if (next->point.y() <= foot->point.y() + 1e-7) break;
            foot = next;
        }
        if (foot->point.x() <= tip.point.x() ||
            foot->point.y() - tip.point.y() < entry.value("minRise").toDouble() * stroke)
            continue;
        result.push_back({*foot, advance == &SearchContext::prev});
    }
    return result;
}

static std::optional<Stretching> cutToothEntry(SearchContext& context, const PathExtrema& tip,
        const PathExtrema& bend, double stroke, const std::vector<ToothEntryLandmark>& entries,
        const QJsonObject& policy) {
    if (policy.isEmpty()) return std::nullopt;
    for (const auto& entry : entries) {
        const auto& foot = entry.foot;
        const bool forward = entry.forward;
        Stretching cut{};
        if (!alefTurnCut(context, foot, policy.value("turnAngle").toDouble(45), stroke, cut, &tip, forward ? 1 : -1) &&
            !contourNormalCut(context, foot, forward, true, cut))
            continue;
        cut.referenceWidth = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
        const auto side = getJoinTime(cut, context.orientation);
        if (cut.referenceWidth < 2.5 * stroke && side.isStrictLeft(tip.time) && side.isStrictLeft(bend.time)) return cut;
    }
    return std::nullopt;
}

// The ordinary tip detector can enclose the crown while leaving the entire
// inclined shaft in the preceding connector. Replace that short entry only
// when the same tooth's baseline turn produces a solid, ordered body.
static void extendToothToBaseline(SearchContext& context, ToothBody& body,
        const QJsonObject& entry, const QJsonObject& policy) {
    if (!body.right) return;
    const auto chord = [](const Stretching& cut) {
        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
    };
    const double stroke = std::min(chord(*body.right).length(), chord(body.left).length());
    const auto entries = recognizeToothEntries(context, body.tip, stroke, entry);
    const auto turn = cutToothEntry(context, body.tip, body.bend, stroke, entries, policy);
    if (!turn || chord(*turn).center().y() - chord(*body.right).center().y() <= .5 * stroke) return;
    const auto side = getJoinTime(*turn, context.orientation);
    if (!side.isStrictLeft(body.left.topBaseLine.time) || !side.isStrictLeft(body.left.bottomBaseLine.time)) return;
    const auto pieces = cutRegions(context.transformedPath, {}, {*turn, body.left}, context.orientation, true);
    if (pieces.size() == 3 && toSubpaths(pieces[1].path).size() == 1 && body.retainedBy(pieces[1].path))
        body.right = turn;
}

// A pointed or sloping tooth can turn sideways before its two flanks reach
// the baseline. Follow the underside both ways from its concave bend and
// measure a normal on each attachment, including across the path's closure.
// Probes shared by intrinsic measurement and cut placement. A measurement
// cross-section supplies scale; it is never committed as a body boundary.
struct ToothFlankGeometry {
    SearchContext& context;
    PathExtrema tip, bend;
    const QPainterPath& ink;
    int count;
    double distance;
    ToothFlankGeometry(SearchContext& context, const PathExtrema& tip, const PathExtrema& bend, int count)
        : context(context), tip(tip), bend(bend), ink(context.transformedPath), count(count),
          distance(QLineF(tip.point, bend.point).length()) {}
    PathExtrema contourPoint(const PathExtrema& start, double distance, bool forward) const {
        PathExtrema end = start, point = start;
        end.time += forward ? count : -count;
        QPointF tangent;
        advanceOnContour(ink, start, end, forward, distance, point, tangent);
        return point;
    }
    bool normalSections(double travel, std::array<Stretching, 2>& sections) const {
        for (int i = 0; i < 2; ++i) {
            const bool forward = i == 0;
            PathExtrema end = bend, at{};
            end.time += forward ? count : -count;
            QPointF tangent;
            if (!advanceOnContour(ink, bend, end, forward, travel, at, tangent) ||
                !contourNormalCut(context, at, forward, false, sections[i]))
                return false;
            std::swap(sections[i].topBaseLine, sections[i].bottomBaseLine);
        }
        return true;
    }
    bool projectSections(double travel, std::array<Stretching, 2>& sections) const {
        const QPointF axis = (bend.point - tip.point) / distance;
        const QPointF across(axis.y(), -axis.x());
        for (int i = 0; i < 2; ++i) {
            const auto origin = bend.point + (i == 0 ? travel : -travel) * across;
            const auto hits = FindInterections(
                ink, QLineF(origin - 3 * distance * axis, origin + 3 * distance * axis), true);
            double nearest = 2 * distance;
            for (size_t j = 1; j < hits.size(); ++j) {
                const auto& a = hits[j - 1];
                const auto& b = hits[j];
                const double gap = QLineF(origin, QLineF(a.point, b.point).center()).length();
                if (gap >= nearest || !interiorChord(ink, a, b)) continue;
                const double thickness = QLineF(a.point, b.point).length();
                if (thickness < .12 * distance) continue;
                nearest = gap;
                sections[i].topBaseLine = a;
                sections[i].bottomBaseLine = b;
                sections[i].referenceWidth = thickness;
            }
            if (nearest == 2 * distance) return false;
        }
        return true;
    }
};

struct BentToothBodyEvidence {
    PathExtrema tip, bend;
    double stroke;
    QVector<QPointF> axis;
    std::vector<ToothEntryLandmark> entries;
};

static std::vector<BentToothBodyEvidence> recognizeToothBends(
        SearchContext& context, const QJsonObject& body) {
    const auto& ink = context.transformedPath;
    const int count = static_cast<int>(baseContourCurves(ink).size());
    std::vector<BentToothBodyEvidence> results;
    if (!count) return results;
    const auto turn = [&](auto at) {
        const auto a = at->point - context.prev(at)->point;
        const auto b = context.next(at)->point - at->point;
        const double length = std::hypot(a.x(), a.y()) * std::hypot(b.x(), b.y());
        const double cross = a.x() * b.y() - a.y() * b.x();
        return length <= 1e-10 ? 0. : (context.orientation == Orientation::ClockWise ? -cross : cross) / length;
    };
    const auto minTurn = body.value("minBendTurn").toDouble();
    const auto maxRatio = body.value("maxPairDistance").toDouble(4.);
    const bool hooked = body.value("shape") == "tooth-hook";
    const bool rounded = body.value("shape") != "tooth-bend";
    for (auto tip = context.timeSorted.begin(); tip != context.timeSorted.end(); ++tip) {
        if ((!rounded && turn(tip) <= 0) || tip->point.y() > context.prev(tip)->point.y() + 1e-6 ||
            tip->point.y() > context.next(tip)->point.y() + 1e-6)
            continue;
        for (auto bend = context.timeSorted.begin(); bend != context.timeSorted.end(); ++bend) {
            const double depth = bend->point.y() - tip->point.y();
            if (depth <= 0 || (!rounded && turn(bend) >= -minTurn)) continue;
            const double distance = QLineF(tip->point, bend->point).length();
            if (std::abs(tip->point.x() - bend->point.x()) > depth) continue;
            QVector<QPointF> curved;
            if (!interiorChord(ink, *tip, *bend)) {
                if (!hooked) continue;
                // The inner face of a rolled tip can cross its straight
                // tip-to-bend chord. Trace a connected strip inside the ink.
                const QLineF axis(tip->point, bend->point);
                for (int sample = 1; sample < 20; ++sample) {
                    auto point = axis.pointAt(sample / 20.);
                    if (!ink.contains(point)) {
                        const double reach = .2 * distance;
                        auto hits =
                            FindInterections(ink, QLineF(point - QPointF(reach, 0), point + QPointF(reach, 0)));
                        std::sort(hits.begin(), hits.end(),
                                  [](const auto& a, const auto& b) { return a.point.x() < b.point.x(); });
                        double best = reach;
                        for (size_t j = 1; j < hits.size(); ++j) {
                            const auto middle = (hits[j - 1].point + hits[j].point) / 2.;
                            const double dx = std::abs(middle.x() - axis.pointAt(sample / 20.).x());
                            if (dx < best && ink.contains(middle)) {
                                best = dx;
                                point = middle;
                            }
                        }
                        if (best == reach) break;
                    }
                    if (!curved.empty() && !ink.contains((curved.back() + point) / 2.)) break;
                    curved.append(point);
                }
                if (curved.size() != 19) continue;
            }
            ToothFlankGeometry geometry(context, *tip, *bend, count);
            std::array<Stretching, 2> sections;
            double stroke = 0;
            if (rounded) {
                const auto above = [&](const PathExtrema& p, double length) {
                    const auto a = geometry.contourPoint(p, length, true).point;
                    const auto b = geometry.contourPoint(p, length, false).point;
                    return std::min(a.y(), b.y()) - p.point.y();
                };
                if (above(*tip, .65 * distance) < -.03 * distance ||
                    (above(*bend, .65 * distance) < -.07 * distance && turn(bend) > -minTurn))
                    continue;
                if (!geometry.projectSections(.55 * distance, sections) && (!hooked || !geometry.normalSections(.4 * distance, sections))) continue;
                stroke = std::min(sections[0].referenceWidth, sections[1].referenceWidth);
                if (distance > maxRatio * stroke || distance < body.value("minPairDistance").toDouble(.5) * stroke) continue;
            } else {
                if (!geometry.normalSections(.4 * distance, sections)) continue;
                stroke = std::min(sections[0].referenceWidth, sections[1].referenceWidth);
                if (distance > maxRatio * stroke || distance < body.value("minPairDistance").toDouble(.6) * stroke)
                    continue;
            }
            results.push_back({*tip, *bend, stroke, std::move(curved),
                recognizeToothEntries(context, *tip, stroke, body.value("baselineEntry").toObject())});
        }
    }
    return results;
}

static std::vector<ToothBody> cutToothBends(SearchContext& context,
        const std::vector<BentToothBodyEvidence>& bodies, const QJsonObject& policy) {
    const auto& ink = context.transformedPath;
    const int count = static_cast<int>(baseContourCurves(ink).size());
    std::vector<ToothBody> results;
    const double offset = policy.value("flankOffset").toDouble(.5);
    const bool hooked = policy.value("flankMode") == "contour-search";
    const bool projected = policy.value("flankMode") == "axis-projection" || hooked;
    for (const auto& evidence : bodies) {
        const auto& tip = evidence.tip;
        const auto& bend = evidence.bend;
        const double stroke = evidence.stroke;
        ToothFlankGeometry geometry(context, tip, bend, count);
        std::array<Stretching, 2> cuts;
        if (projected) {
            if (hooked) {
                // On a curled flank the underside normal can point back
                // through the tip. Find the shortest solid section to
                // each outer flank and classify its side by contour order.
                std::array<double, 2> best{3 * stroke, 3 * stroke};
                for (bool forward : {true, false}) {
                    const auto lower = geometry.contourPoint(bend, offset * stroke, forward);
                    for (int angle = -70; angle <= 70; angle += 10) {
                        const double radians = angle * M_PI / 180;
                        const QPointF direction(std::sin(radians), -std::cos(radians));
                        const auto hits = FindInterections(
                            ink, QLineF(lower.point, lower.point + 3 * stroke * direction), true);
                        for (const auto& upper : hits) {
                            const double length = QLineF(upper.point, lower.point).length();
                            if (length < .25 * stroke || !interiorChord(ink, upper, lower)) continue;
                            Stretching sample{};
                            sample.topBaseLine = upper;
                            sample.bottomBaseLine = lower;
                            const auto side = getJoinTime(sample, context.orientation);
                            // A short chord can clip just a corner of the
                            // underside. An attachment separates the two
                            // ends of the connected writing stroke.
                            if (!side.isStrictRight(context.xSorted.begin()->time) ||
                                !side.isStrictLeft(std::prev(context.xSorted.end())->time))
                                continue;
                            int arm = side.isStrictLeft(tip.time) ? 0 : 1;
                            if ((arm == 0 && !side.isStrictLeft(bend.time)) ||
                                (arm == 1 && !side.isStrictRight(bend.time)) || length >= best[arm])
                                continue;
                            best[arm] = length;
                            sample.referenceWidth = length;
                            cuts[arm] = sample;
                        }
                    }
                }
                if (best[0] == 3 * stroke || best[1] == 3 * stroke) continue;
            } else if (!geometry.projectSections(offset * stroke, cuts))
                continue;
            for (auto& cut : cuts) {
                auto normal = cut;
                if (strokeNormalCut(context, normal)) {
                    const auto side = getJoinTime(normal, context.orientation);
                    const auto nominal = getJoinTime(cut, context.orientation);
                    if ((!hooked || (side.isStrictRight(context.xSorted.begin()->time) &&
                                     side.isStrictLeft(std::prev(context.xSorted.end())->time))) &&
                        side.isStrictLeft(tip.time) == nominal.isStrictLeft(tip.time) &&
                        side.isStrictLeft(bend.time) == nominal.isStrictLeft(bend.time))
                        cut = normal;
                }
            }
        } else if (!geometry.normalSections(offset * stroke, cuts)) continue;
        const auto entryTurn = cutToothEntry(context, tip, bend, stroke, evidence.entries,
                                             policy.value("baselineEntry").toObject());
        for (int side = 0; side < 2; ++side) {
            const auto& right = entryTurn ? *entryTurn : cuts[side];
            const auto& left = cuts[1 - side];
            const auto entry = getJoinTime(right, context.orientation),
                       exit = getJoinTime(left, context.orientation);
            if (!entry.isStrictLeft(tip.time) || !entry.isStrictLeft(bend.time) ||
                !exit.isStrictRight(tip.time) || !exit.isStrictRight(bend.time) ||
                !entry.isStrictLeft(left.topBaseLine.time) || !entry.isStrictLeft(left.bottomBaseLine.time))
                continue;
            if (std::max(right.referenceWidth, left.referenceWidth) > policy.value("maxSectionWidth").toDouble(2.5) * stroke) {
                if (inspectingForm) inspectReason("Cuts", QString("Section width exceeds maxSectionWidth (%1 × local stroke).")
                    .arg(policy.value("maxSectionWidth").toDouble(2.5)));
                continue;
            }
            const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
            if (pieces.size() != 3 || toSubpaths(pieces[1].path).size() != 1) continue;
            const auto& body = pieces[1].path;
            const QLineF axis(tip.point, bend.point);
            ToothBody tooth{tip, bend, left, right, evidence.axis};
            if (!tooth.retainedBy(body)) {
                inspectReason("Cuts", "The projected mask would discard part of the tooth core.");
                continue;
            }
            if (evidence.axis.isEmpty() && (!body.contains(axis.pointAt(.25)) || !body.contains(axis.pointAt(.5)) ||
                                     !body.contains(axis.pointAt(.75))))
                continue;
            results.push_back(std::move(tooth));
            break;
        }
    }
    return results;
}

// Rounded shoulders and curled hooks include extrema at cubic junctions.
// Keep the resulting landmark inventory with the body evidence so projection
// uses exactly that contour vocabulary, even after a cut policy is edited.
struct BentToothRecognition {
    bool refined = false;
    timesortedtype landmarks;
    std::vector<BentToothBodyEvidence> bodies;
};

static BentToothRecognition recognizeBentToothBodies(SearchContext& context, const QJsonObject& body) {
    if (body.value("shape") == "tooth-bend") return {false, {}, recognizeToothBends(context, body)};
    SearchContext local{.transformedPath=context.transformedPath, .xSorted=context.xSorted,
        .timeSorted=context.timeSorted, .bbox=context.bbox, .subword=context.subword, .orientation=context.orientation,
        .outlineCache=context.outlineCache};
    const auto curves = baseContourCurves(local.transformedPath);
    for (int i = 0; i < static_cast<int>(curves.size()); ++i) {
        const auto p = curves[i].pt1();
        const auto a = curves[(i+curves.size()-1)%curves.size()].pointAt(.97);
        const auto b = curves[i].pointAt(.03);
        if ((a.y()-p.y())*(b.y()-p.y()) <= 0) continue;
        const bool already = std::any_of(local.timeSorted.begin(), local.timeSorted.end(), [&](const auto& e) {
            return std::abs(e.time-i)<.05 && QLineF(e.point,p).length()<.01;
        });
        if (!already) local.timeSorted.insert({0,double(i),p,false,true});
    }
    return {true, local.timeSorted, recognizeToothBends(local, body)};
}

static std::vector<ToothBody> cutBentToothBodies(SearchContext& context,
        const BentToothRecognition& evidence, const QJsonObject& policy) {
    if (!evidence.refined) return cutToothBends(context, evidence.bodies, policy);
    SearchContext local{.transformedPath=context.transformedPath, .xSorted=context.xSorted,
        .timeSorted=evidence.landmarks, .bbox=context.bbox, .subword=context.subword, .orientation=context.orientation,
        .outlineCache=context.outlineCache};
    return cutToothBends(local, evidence.bodies, policy);
}

// Recognize a tooth from its outer tip and the inner bend across the ink.
// Its two flanks and their local cross-section supply the scale and cuts;
// no neighboring join, character or stroke width is used here.
// Recognition stores only intrinsic landmarks and cross-sections used for
// measurement. It has no committed boundaries or masks and accepts no cut
// policy. Keeping both valid scale hypotheses lets a policy reject a baseline
// projection without losing a tooth supported by its own opposite flank.
struct ToothScaleEvidence {
    double stroke;
    std::vector<PathExtrema> flankSamples;
};
struct ToothPairEvidence {
    PathExtrema bend;
    Stretching localSection;
    std::optional<ToothScaleEvidence> localScale, entryScale;
};
struct ToothBodyEvidence {
    PathExtrema tip;
    std::vector<PathExtrema> forwardFlank, reverseFlank;
    bool reverseIsRight;
    std::optional<Stretching> baselineSection;
    double entryStroke;
    bool requiredEntry;
    double minEntryRise;
    std::vector<ToothPairEvidence> pairs;
};

static std::vector<ToothBodyEvidence> recognizeToothBodies(
        SearchContext& context, const QJsonObject& settings) {
    const auto& ink = context.transformedPath;
    std::vector<ToothBodyEvidence> results;
    // Derivative roots and curve endpoints can name the same extremum
    // within floating-point precision. It is one tip, with two real flanks.
    const auto adjacent = [&](auto point, auto advance) {
        auto next = (context.*advance)(point);
        while (next != point && std::abs(next->time - point->time) < 1e-7)
            next = (context.*advance)(next);
        return next;
    };
    const auto duplicate = [&](auto point) {
        return point != context.timeSorted.begin() && std::abs(std::prev(point)->time - point->time) < 1e-7;
    };
    const auto minimum = [&](auto point) {
        return point->point.y() < adjacent(point, &SearchContext::prev)->point.y() &&
               point->point.y() < adjacent(point, &SearchContext::next)->point.y();
    };
    const auto innerMinimum = [&](auto point) {
        if (minimum(point)) return true;
        if (!settings.value("allowFlatBend").toBool()) return false;
        // A short horizontal contour segment can split one inner minimum into
        // two equal-height endpoints. Examine the flanks of that plateau.
        for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
            auto flank = adjacent(point, advance);
            for (size_t step = 0; step < context.timeSorted.size() &&
                 std::abs(flank->point.y() - point->point.y()) < 1e-7; ++step)
                flank = adjacent(flank, advance);
            if (flank->point.y() <= point->point.y() + 1e-7) return false;
        }
        return true;
    };
    for (auto peak = context.timeSorted.begin(); peak != context.timeSorted.end(); ++peak) {
        if (duplicate(peak) || !minimum(peak)) continue;
        const auto tip = peak->point;
        const auto flank = [&](auto advance) {
            std::vector<PathExtrema> points{*peak};
            auto current = peak;
            for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                const auto next = adjacent(current, advance);
                if (next == peak || next->point.y() <= current->point.y()) break;
                points.push_back(*next);
                current = next;
            }
            return points;
        };
        auto reverseFlank = flank(&SearchContext::next), forwardFlank = flank(&SearchContext::prev);
        if (reverseFlank.size() < 2 || forwardFlank.size() < 2) continue;
        const bool reverseIsRight = reverseFlank.back().point.x() > forwardFlank.back().point.x();
        const auto& rightFlank = reverseIsRight ? reverseFlank : forwardFlank;
        const auto& leftFlank = reverseIsRight ? forwardFlank : reverseFlank;
        const auto baselineEntry = settings.value("baselineEntry").toObject();
        std::optional<Stretching> footEntry;
        double entryStroke = 0;
        if (!baselineEntry.isEmpty()) {
            // The tooth's own descending flank ends at the baseline turn.
            // Its cross-section provides the scale, without a neighbor's join.
            const auto& anchor = rightFlank.back();
            const auto below = FindInterections(ink, QLineF(anchor.point,
                QPointF(anchor.point.x(), context.bbox.bottom() + context.bbox.height())), true);
            for (const auto& bottom : below) {
                if (bottom.point.y() <= anchor.point.y() || !interiorChord(ink, anchor, bottom)) continue;
                Stretching cut{};
                cut.topBaseLine = anchor;
                cut.bottomBaseLine = bottom;
                if (strokeNormalCut(context, cut)) footEntry = cut;
                break;
            }
            if (footEntry) {
                const double width = QLineF(footEntry->topBaseLine.point, footEntry->bottomBaseLine.point).length();
                if (anchor.point.x() <= tip.x() || anchor.point.y() - tip.y() <
                    baselineEntry.value("minRise").toDouble() * width) footEntry.reset();
                else entryStroke = width;
            }
        }
        const auto onFlank = [&](const PathExtrema& point, const auto& points) {
            if (point.pathIndex != 0) return false;
            const bool forward = &points == &forwardFlank;
            for (size_t i = 1; i < points.size(); ++i) {
                const double from = forward ? points[i - 1].time : points[i].time;
                const double to = forward ? points[i].time : points[i - 1].time;
                const bool after = point.time >= from - 1e-6, before = point.time <= to + 1e-6;
                if (from <= to ? after && before : after || before) return true;
            }
            return false;
        };
        std::vector<PathExtrema> bends;
        for (auto inner = context.timeSorted.begin(); inner != context.timeSorted.end(); ++inner)
            if (!duplicate(inner) && innerMinimum(inner) && inner->point.y() > tip.y() && interiorChord(ink, *peak, *inner))
                bends.push_back(*inner);
        std::sort(bends.begin(), bends.end(), [&](const auto& x, const auto& y) {
            return QLineF(tip, x.point).length() < QLineF(tip, y.point).length();
        });
        ToothBodyEvidence body{*peak, forwardFlank, reverseFlank, reverseIsRight, footEntry, entryStroke,
            baselineEntry.value("required").toBool(), baselineEntry.value("minRise").toDouble(), {}};
        for (const auto& inner : bends) {
            const double distance = QLineF(tip, inner.point).length();
            // Sample the joining stroke just past the inner bend, on the
            // tooth's descending flank. Normalization belongs to this tooth.
            Stretching local{};
            if (!medialChordAt(context, inner.point.x() - 0.5 * distance, inner.point.y(), distance, local, true) ||
                !onFlank(local.topBaseLine, leftFlank)) {
                local.topBaseLine = *peak;
                local.bottomBaseLine = inner;
            }
            const double localWidth = QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length();
            const auto measure = [&](double stroke) -> std::optional<ToothScaleEvidence> {
                if (stroke <= 0 || distance < settings.value("minPairDistance").toDouble() * stroke ||
                    distance > settings.value("maxPairDistance").toDouble(2.5) * stroke) {
                    if (inspectingForm) inspectReason("Body", QString("Tip–bend distance outside [%1, %2] × local stroke.")
                        .arg(settings.value("minPairDistance").toDouble()).arg(settings.value("maxPairDistance").toDouble(2.5)));
                    return std::nullopt;
                }
                const double rise = std::min(rightFlank.back().point.y(), leftFlank.back().point.y()) - tip.y();
                if (rise < settings.value("minFlankRise").toDouble() * stroke) {
                    inspectReason("Body", "Flank rise is below minFlankRise.");
                    return std::nullopt;
                }
                const double drop = std::min(settings.value("flankDrop").toDouble(0.6) * stroke, 0.95 * rise);
                const auto hits = FindInterections(ink, QLineF(context.bbox.left() - 1, tip.y() + drop,
                                                              context.bbox.right() + 1, tip.y() + drop), true);
                const auto foot = [&](const auto& points) -> std::optional<QPointF> {
                    for (const auto& hit : hits) if (onFlank(hit, points)) return hit.point;
                    return std::nullopt;
                };
                const auto r = foot(rightFlank), l = foot(leftFlank);
                if (!r || !l) return std::nullopt;
                const double width = r->x() - l->x();
                if (r->x() + settings.value("maxTipOverhang").toDouble(0.12) * stroke < tip.x() ||
                    tip.x() < l->x() || width < settings.value("minWidth").toDouble(0.2) * stroke ||
                    width > settings.value("maxWidth").toDouble(3) * stroke) {
                    inspectReason("Body", "Tooth width or tip overhang is outside the configured limits.");
                    return std::nullopt;
                }
                return ToothScaleEvidence{stroke, hits};
            };
            auto localScale = measure(localWidth);
            auto entryScale = footEntry ? (entryStroke == localWidth ? localScale : measure(entryStroke)) : std::nullopt;
            if (localScale || entryScale)
                body.pairs.push_back({inner, local, std::move(localScale), std::move(entryScale)});
        }
        if (!body.pairs.empty()) results.push_back(std::move(body));
    }
    return results;
}

// Only this stage places the normal sections and chooses an accepted pair of
// boundaries. Candidate order and all offsets retain the previous geometry.
static std::vector<ToothBody> cutToothBodies(SearchContext& context,
        const std::vector<ToothBodyEvidence>& bodies, const QJsonObject& settings, bool bothSides = true) {
    const auto& ink = context.transformedPath;
    std::vector<ToothBody> results;
    const auto adjacent = [&](auto point, auto advance) {
        auto next = (context.*advance)(point);
        while (next != point && std::abs(next->time - point->time) < 1e-7)
            next = (context.*advance)(next);
        return next;
    };
    for (const auto& evidence : bodies) {
        const auto tip = evidence.tip.point;
        const auto& forwardFlank = evidence.forwardFlank;
        const auto& reverseFlank = evidence.reverseFlank;
        const auto& rightFlank = evidence.reverseIsRight ? reverseFlank : forwardFlank;
        const auto& leftFlank = evidence.reverseIsRight ? forwardFlank : reverseFlank;
        const auto baselineEntry = settings.value("baselineEntry").toObject();
        auto footEntry = evidence.baselineSection;
        if (footEntry) {
            const auto& anchor = rightFlank.back();
            const auto width = evidence.entryStroke;
            if (baselineEntry.contains("turnAngle") && anchor.point.x() - tip.x() >=
                baselineEntry.value("minTurnRun").toDouble() * width) {
                Stretching turn{};
                const int direction = &rightFlank == &forwardFlank ? 1 : -1;
                if (alefTurnCut(context, anchor, baselineEntry.value("turnAngle").toDouble(),
                                width, turn, &evidence.tip, direction)) {
                    turn.referenceWidth = width;
                    footEntry = turn;
                } else if (!baselineEntry.contains("minTurnRun")) footEntry.reset();
            }
        }
        const auto onFlank = [&](const PathExtrema& point, const auto& points) {
            if (point.pathIndex != 0) return false;
            const bool forward = &points == &forwardFlank;
            for (size_t i = 1; i < points.size(); ++i) {
                const double from = forward ? points[i - 1].time : points[i].time;
                const double to = forward ? points[i].time : points[i - 1].time;
                const bool after = point.time >= from - 1e-6, before = point.time <= to + 1e-6;
                if (from <= to ? after && before : after || before) return true;
            }
            return false;
        };
        for (const auto& pair : evidence.pairs) {
            const auto& inner = pair.bend;
            const auto& local = pair.localSection;
            const double localWidth = QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length();
            auto rightTurn = footEntry;
            if (!rightTurn && evidence.requiredEntry) {
                // An inclined entry can end at a corner whose vertical misses
                // the underside. Measure the tooth at its own opposite flank
                // and search the descending edge directly for its turning normal.
                const auto& root = rightFlank.back();
                const int direction = &rightFlank == &forwardFlank ? 1 : -1;
                Stretching turn{};
                if (root.point.x() <= tip.x() || root.point.y() - tip.y() <
                    evidence.minEntryRise * localWidth ||
                    !baselineEntry.contains("turnAngle") ||
                    !alefTurnCut(context, root, baselineEntry.value("turnAngle").toDouble(),
                                 localWidth, turn, &evidence.tip, direction)) continue;
                turn.referenceWidth = localWidth;
                rightTurn = turn;
            }
            const auto& profile = footEntry ? pair.entryScale : pair.localScale;
            if (!profile) continue;
            const double stroke = profile->stroke;
            const auto& hits = profile->flankSamples;
            Stretching right{}, left{};
            // Keep the known bend as the lower endpoint. Re-discovering an
            // exact curve endpoint by intersection can lose it to rounding.
            const auto leftHits = FindInterections(ink, QLineF(inner.point,
                QPointF(inner.point.x(), context.bbox.top() - stroke)), true);
            double shortest = 2 * stroke;
            bool foundLeft = false;
            for (const auto& top : leftHits) {
                const double length = QLineF(top.point, inner.point).length();
                if (top.point.y() >= inner.point.y() || length >= shortest ||
                    !onFlank(top, leftFlank) || !interiorChord(ink, top, inner)) continue;
                left.topBaseLine = top;
                left.bottomBaseLine = inner;
                shortest = length;
                foundLeft = true;
            }
            if (!foundLeft) {
                for (bool previous : {false, true}) {
                    Stretching normal{};
                    if (!contourNormalCut(context, inner, previous, false, normal)) continue;
                    if (!onFlank(normal.topBaseLine, leftFlank) && onFlank(normal.bottomBaseLine, leftFlank))
                        std::swap(normal.topBaseLine, normal.bottomBaseLine);
                    if (!onFlank(normal.topBaseLine, leftFlank)) continue;
                    left = normal; foundLeft = true; break;
                }
            }
            bool shiftedLeft = false;
            if (!foundLeft && onFlank(local.topBaseLine, leftFlank)) {
                left = local; foundLeft = true; shiftedLeft = true;
            }
            if (!foundLeft) continue;
            bool haveRightSection = true;
            if (rightTurn) right = *rightTurn;
            if (bothSides && !rightTurn && (!medialChordAt(context, tip.x() + settings.value("rightOffset").toDouble(0.5) * stroke,
                               inner.point.y(), 2 * stroke, right, true) || !onFlank(right.topBaseLine, rightFlank))) {
                bool found = false;
                for (const auto& top : hits) {
                    if (!onFlank(top, rightFlank)) continue;
                    const auto below = FindInterections(ink, QLineF(top.point,
                        QPointF(top.point.x(), context.bbox.bottom() + stroke)), true);
                    for (const auto& bottom : below) {
                        if (bottom.point.y() <= top.point.y() || !interiorChord(ink, top, bottom)) continue;
                        right.topBaseLine = top; right.bottomBaseLine = bottom; found = true; break;
                    }
                    if (found) break;
                }
                haveRightSection = found;
            }
            const auto originalLeft = left;
            const double leftOffset = shiftedLeft ? 0 : -settings.value("leftOffset").toDouble(.45);
            bool normalLeft = strokeNormalCut(context, left, leftOffset);
            const double tolerance = settings.value("normalOffsetTolerance").toDouble(0.25);
            for (int step = 1; !normalLeft && step <= 5; ++step) {
                for (double direction : {-1., 1.}) {
                    left = originalLeft;
                    const double offset = leftOffset + direction * tolerance * step / 5;
                    if (offset < 0 && strokeNormalCut(context, left, offset)) { normalLeft = true; break; }
                }
            }
            const auto ownsTooth = [&](const Stretching& cut) {
                const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
                return pieces.size() == 2 && pieces[0].path.contains(tip + .05 * (inner.point - tip)) &&
                    pieces[0].path.contains(tip + .95 * (inner.point - tip));
            };
            if (!normalLeft || (!bothSides && !onFlank(left.topBaseLine, leftFlank)) || !ownsTooth(left)) {
                normalLeft = false;
                const auto start = context.timeSorted.find(inner);
                for (auto advance : {&SearchContext::next, &SearchContext::prev}) {
                    auto end = adjacent(start, advance);
                    if (end->point.x() >= inner.point.x()) continue;
                    for (int step = 0; step < 4; ++step) {
                        PathExtrema anchor{}; QPointF tangent;
                        if (advanceOnContour(ink, inner, *end, advance == &SearchContext::prev,
                                settings.value("leftOffset").toDouble(.45) * stroke, anchor, tangent)) {
                            Stretching proposal{};
                            if (contourNormalCut(context, anchor, advance == &SearchContext::prev, false, proposal)) {
                                std::swap(proposal.topBaseLine, proposal.bottomBaseLine);
                                if ((bothSides || onFlank(proposal.topBaseLine, leftFlank)) && ownsTooth(proposal)) {
                                    left = proposal; normalLeft = true;
                                }
                            }
                            break;
                        }
                        const auto next = adjacent(end, advance);
                        if (next->point.x() >= end->point.x()) break;
                        end = next;
                    }
                    if (normalLeft) break;
                }
            }
            if (!normalLeft) {
                inspectReason("Cuts", "No left normal at this offset keeps the tooth on its own flank.");
                continue;
            }
            std::vector<Stretching> rightOptions;
            if (!bothSides) rightOptions.push_back(right);
            else if (rightTurn) rightOptions.push_back(*rightTurn);
            else {
                auto candidate = right;
                if (haveRightSection && strokeNormalCut(context, candidate)) rightOptions.push_back(candidate);
                for (int step = 1; step <= 4; ++step) {
                    if (!medialChordAt(context, tip.x() + (settings.value("rightOffset").toDouble(.5) + .25 * step) * stroke,
                            inner.point.y(), 3 * stroke, candidate, true) ||
                        !onFlank(candidate.topBaseLine, rightFlank) || !strokeNormalCut(context, candidate) ||
                        !onFlank(candidate.topBaseLine, rightFlank)) continue;
                    rightOptions.push_back(candidate);
                }
                // On a narrow or overhanging shoulder a vertical probe can
                // miss the stroke. Walk the underside from the inner bend
                // toward the right, and project its normal onto this flank.
                const auto start = context.timeSorted.find(inner);
                for (auto advance : {&SearchContext::prev, &SearchContext::next}) {
                    auto end = start;
                    for (size_t step = 0; step + 1 < context.timeSorted.size(); ++step) {
                        const auto next = adjacent(end, advance);
                        if (next->point.x() <= end->point.x()) break;
                        end = next;
                        PathExtrema anchor{}; QPointF tangent;
                        if (!advanceOnContour(ink, inner, *end, advance == &SearchContext::prev,
                                settings.value("rightOffset").toDouble(.5) * stroke, anchor, tangent)) continue;
                        Stretching normal{};
                        if (contourNormalCut(context, anchor, advance == &SearchContext::prev, false, normal)) {
                            const auto& upper = normal.bottomBaseLine;
                            // A very short upright shoulder can end before
                            // this half-width offset. Accept its adjacent
                            // baseline only within one local stroke of the foot.
                            const auto& foot = rightFlank.back();
                            if (onFlank(upper, rightFlank) ||
                                (upper.point.x() >= foot.point.x() && upper.point.y() < anchor.point.y() &&
                                 QLineF(foot.point, upper.point).length() <= stroke &&
                                 QLineF(anchor.point, upper.point).length() <= 1.75 * stroke)) {
                                std::swap(normal.topBaseLine, normal.bottomBaseLine);
                                rightOptions.push_back(normal);
                            }
                        }
                        break;
                    }
                }
            }
            bool accepted = false;
            for (const auto& proposal : rightOptions) {
                const auto rightTime = getJoinTime(proposal, context.orientation);
                if (bothSides && (!rightTime.isStrictLeft(left.topBaseLine.time) ||
                    !rightTime.isStrictLeft(left.bottomBaseLine.time))) continue;
                const std::vector<Stretching> cuts = bothSides ? std::vector{proposal, left} : std::vector{left};
                const auto pieces = cutRegions(ink, {}, cuts, context.orientation, true);
                const int bodyIndex = bothSides ? 1 : 0;
                if (pieces.size() != cuts.size() + 1 || pieces[bodyIndex].path.boundingRect().isEmpty()) continue;
                const auto& bodyPath = pieces[bodyIndex].path;
                // An open tooth has no counter. A loop crest can satisfy the
                // same tip/bend distances, but belongs to a loop-shaped body.
                if (bothSides && toSubpaths(bodyPath).size() != 1) continue;
                const auto body = bodyPath.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
                if (!body.contains(tip) || !body.contains(inner.point) ||
                    !bodyPath.contains(tip + .05 * (inner.point - tip)) ||
                    !bodyPath.contains(tip + .95 * (inner.point - tip))) continue;
                results.push_back({evidence.tip, inner, left, bothSides ? std::optional{proposal} : std::nullopt});
                accepted = true;
                break;
            }
            if (!accepted) {
                inspectReason("Cuts", "No right/left pair passed contour ordering, single-body topology and core containment.");
                continue;
            }
            break;
        }
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.right && b.right ? Stretching::cmpbyx{}(*a.right, *b.right) : a.tip.point.x() > b.tip.point.x();
    });
    return results;
}

// Loop bodies still describe an exit tooth in their existing loop policy.
// This adapter is not used by any medial senna declaration.
static std::vector<ToothBody> loopExitToothBoundaries(SearchContext& context, const QJsonObject& settings) {
    return cutToothBodies(context, recognizeToothBodies(context, settings), settings, false);
}

static std::vector<ToothBody> declaredToothBoundaries(SearchContext& context, const QJsonObject& form) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto body = form.value("body").toObject();
    const auto cuts = form.value("cuts").toObject();
    std::vector<ToothBody> result;
    bool recognized = false;
    if (bentToothDescription(body)) {
        const auto evidence = recognizeBentToothBodies(context, body);
        recognized = !evidence.bodies.empty();
        if (inspectingForm) for (const auto& item : evidence.bodies) {
            inspectLandmark("Tooth tip", item.tip);
            inspectLandmark("Inner bend", item.bend);
            for (const auto& entry : item.entries) inspectLandmark("Baseline foot", entry.foot);
        }
        if (evidence.bodies.empty()) inspectReason("Body", "No tip–bend pair passed the bend, flank, ink continuity and distance constraints.");
        result = cutBentToothBodies(context, evidence, cuts);
    } else {
        const auto evidence = recognizeToothBodies(context, body);
        recognized = !evidence.empty();
        if (inspectingForm) for (const auto& item : evidence) {
            inspectLandmark("Tooth tip", item.tip);
            for (const auto& pair : item.pairs) inspectLandmark("Inner bend", pair.bend);
            if (!item.forwardFlank.empty()) inspectLandmark("Flank foot", item.forwardFlank.back());
            if (!item.reverseFlank.empty()) inspectLandmark("Flank foot", item.reverseFlank.back());
        }
        if (evidence.empty()) inspectReason("Body", "No tip–bend pair passed the body constraints.");
        result = cutToothBodies(context, evidence, cuts);
    }
    if (recognized && result.empty())
        inspectReason("Cuts", "Body evidence found, but no boundary pair passed projection and retained-body checks.");
    if (inspectingForm) for (const auto& item : result) inspectCandidate(QString("Tip %1 / bend %2").arg(item.tip.time, 0, 'f', 2)
        .arg(item.bend.time, 0, 'f', 2), item.left, item.right);
    return result;
}

// The stacked counters identify the medial heh itself. Locate the inward
// corners beside the lower counter, then keep each corner fixed while
// projecting across the joining stroke. All measurements belong to this
// pair of counters, independently of the neighboring letters and cuts.
static std::vector<std::pair<Stretching, Stretching>> medialPairedCounterBoundaries(
        SearchContext& context, const QJsonObject& settings) {
    const auto& ink = context.transformedPath;
    const auto curves = baseContourCurves(ink);
    std::vector<std::pair<Stretching, Stretching>> results;
    if (curves.empty()) return results;
    const double footInwardTolerance = std::sin(settings.value("maxFootInwardAngle").toDouble(10) * M_PI / 180);
    std::vector<QRectF> holes;
    for (size_t i = 1; i < context.subPaths.size(); ++i) {
        const auto hole = context.subPaths[i].boundingRect();
        if (hole.width() > 0 && hole.height() > 0 && !ink.contains(hole.center())) holes.push_back(hole);
    }
    for (const auto& upper : holes) for (const auto& lower : holes) {
        const double scale = lower.height();
        if (upper.bottom() >= lower.top() ||
            std::min(upper.right(), lower.right()) <= std::max(upper.left(), lower.left()) ||
            lower.top() - upper.bottom() > settings.value("maxCounterGap").toDouble(2) *
                std::max(upper.height(), lower.height())) continue;
        const auto cornerCut = [&](bool rightSide) -> std::optional<Stretching> {
            const QPointF reference = rightSide ? lower.topRight() : lower.topLeft();
            const double maxDistance = settings.value("maxCornerDistance").toDouble(2) * scale;
            double closest = maxDistance;
            const double bandTop = (upper.bottom() + lower.top()) / 2;
            std::optional<Stretching> best;
            bool bestHasFoot = false;
            for (const auto& corner : context.timeSorted) {
                const auto p = corner.point;
                const double distance = QLineF(p, reference).length();
                if (!corner.isSharp || distance >= maxDistance || p.y() < bandTop ||
                    p.y() >= lower.center().y() || (rightSide ? p.x() <= lower.center().x() : p.x() >= lower.center().x()) ||
                    std::abs(corner.time - std::round(corner.time)) > 1e-6) continue;
                const int count = static_cast<int>(curves.size());
                const int index = static_cast<int>(std::lround(corner.time)) % count;
                const auto& previous = curves[(index + count - 1) % count];
                const auto& next = curves[index];
                auto a = previous.pt3() - previous.pt4();
                auto b = next.pt2() - next.pt1();
                if (std::hypot(a.x(), a.y()) < 1e-9) a = previous.pointAt(1 - 1e-4) - p;
                if (std::hypot(b.x(), b.y()) < 1e-9) b = next.pointAt(1e-4) - p;
                const double al = std::hypot(a.x(), a.y()), bl = std::hypot(b.x(), b.y());
                if (al < 1e-9 || bl < 1e-9) continue;
                a /= al; b /= bl;
                // The descending side of a stepped foot can lean slightly
                // inward. Allow that small angular variation around vertical;
                // the other branch still has to lead out towards the join.
                // Prefer the outer corner so the complete foot stays in the body.
                // Without a step, the nearest inward corner is the attachment.
                const bool hasFoot = rightSide ? std::min(a.x(), b.x()) >= -footInwardTolerance - 1e-6 :
                                                std::max(a.x(), b.x()) <= footInwardTolerance + 1e-6;
                if (best) {
                    if (bestHasFoot && !hasFoot) continue;
                    if (bestHasFoot == hasFoot) {
                        // Both the outside step and its inner attachment can
                        // pass the angle tolerance. Keep the outside step,
                        // rather than the corner nearest the counter.
                        const double outward = (rightSide ? 1 : -1) *
                            (p.x() - best->bottomBaseLine.point.x());
                        if (hasFoot ? outward <= 0 : distance >= closest) continue;
                    }
                }
                const auto bisector = a + b;
                const double length = std::hypot(bisector.x(), bisector.y());
                if (length < 1e-3) continue;
                const auto probe = bisector * (0.01 * scale / length);
                // At an inward corner the smaller wedge is outside the ink.
                if (ink.contains(p + probe) || !ink.contains(p - probe)) continue;
                const auto tangent = (rightSide ? a.x() > b.x() : a.x() < b.x()) ? a : b;
                if (rightSide ? tangent.x() <= 0 : tangent.x() >= 0) continue;
                QPointF normal = settings.value("normalCut").toBool() ? QPointF(tangent.y(), -tangent.x()) : QPointF(0, -1);
                if (normal.y() > 0) normal = -normal;
                const double reach = upper.united(lower).height() + 2 * scale;
                const auto hits = FindInterections(ink, QLineF(p, p + normal * reach), true);
                for (const auto& top : hits) {
                    if (top.pathIndex != 0 || top.point.y() >= p.y() || !interiorChord(ink, top, corner)) continue;
                    Stretching cut{};
                    cut.topBaseLine = top;
                    cut.bottomBaseLine = corner;
                    cut.referenceWidth = QLineF(top.point, p).length();
                    best = cut;
                    closest = distance;
                    bestHasFoot = hasFoot;
                    break;
                }
            }
            return best;
        };
        const auto right = cornerCut(true), left = cornerCut(false);
        if (!right || !left || !Stretching::cmpbyx{}(*right, *left)) continue;
        const auto pieces = cutRegions(ink, {}, {*right, *left}, context.orientation, true);
        if (pieces.size() != 3 || !pieces[1].path.boundingRect().contains(upper) ||
            !pieces[1].path.boundingRect().contains(lower)) continue;
        results.emplace_back(*right, *left);
    }
    return results;
}

// Follow a head's flank through intermediate corners to its horizontal
// extremum. A Bezier vertex on the flank is not necessarily its side edge.
static timesortedtype::iterator headFlank(SearchContext& context, timesortedtype::iterator crest,
                                         bool increasingTime) {
    const auto advance = increasingTime ? &SearchContext::prev : &SearchContext::next;
    auto point = (context.*advance)(crest);
    const double direction = point->point.x() - crest->point.x();
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(point);
        if (next == crest || direction * (next->point.x() - point->point.x()) <= 0) break;
        point = next;
    }
    return point;
}

// The open head has a crest flanked by a rightmost and leftmost contour
// extremum, without an enclosed counter. Project those two edges onto the
// joining stroke below the head. All proportions are relative to this head.
static std::vector<std::pair<Stretching, Stretching>> medialOpenHeadBoundaries(
        SearchContext& context, const QJsonObject& settings, double minX, double maxX) {
    std::vector<std::pair<Stretching, Stretching>> candidates;
    for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest) {
        if (crest->point.x() >= maxX || crest->point.x() <= minX ||
            crest->point.y() >= context.prev(crest)->point.y() ||
            crest->point.y() >= context.next(crest)->point.y()) continue;
        const auto a = headFlank(context, crest, true), b = headFlank(context, crest, false);
        const auto rightEdge = a->point.x() > b->point.x() ? a : b;
        const auto leftEdge = rightEdge == a ? b : a;
        const auto r = rightEdge->point, l = leftEdge->point;
        const double width = r.x() - l.x();
        if (width <= 0 || r.x() <= crest->point.x() || l.x() >= crest->point.x() ||
            r.x() <= context.prev(rightEdge)->point.x() || r.x() <= context.next(rightEdge)->point.x() ||
            l.x() >= context.prev(leftEdge)->point.x() || l.x() >= context.next(leftEdge)->point.x()) continue;
        const double minRise = settings.value("minFlankRise").toDouble(0.15) * width;
        const double maxRise = settings.value("maxFlankRise").toDouble(0.8) * width;
        if (r.y() - crest->point.y() < minRise || r.y() - crest->point.y() > maxRise ||
            l.y() - crest->point.y() < minRise || l.y() - crest->point.y() > maxRise) continue;
        const double height = std::max(r.y(), l.y()) - crest->point.y();
        const QRectF head(QPointF(l.x(), crest->point.y()), QPointF(r.x(), crest->point.y() + height));
        bool closed = false;
        for (size_t i = 1; i < context.subPaths.size(); ++i) {
            const auto hole = context.subPaths[i].boundingRect();
            if (head.contains(hole.center()) && !context.transformedPath.contains(hole.center())) {
                closed = true; break;
            }
        }
        if (closed) continue;
        Stretching right{}, left{};
        if (medialChordAt(context, r.x(), r.y() + height, height * 2, right) &&
            medialChordAt(context, l.x(), l.y() + height, height * 2, left) &&
            right.topBaseLine.point.y() >= r.y() && left.topBaseLine.point.y() >= l.y())
            candidates.emplace_back(right, left);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return Stretching::cmpbyx{}(a.first, b.first);
    });
    return candidates;
}

// A medial feh/qaf head encloses one counter. Its crest and side edges locate
// that counter, which supplies the scale and baseline for both neck cuts.
// The resulting body must retain the entire loop and the crest. Neighboring
// letters supply neither landmarks nor stroke-width/height estimates.
struct CrestedLoopBody { QRectF counter; PathExtrema crest, rightEdge, leftEdge; };
static std::vector<CrestedLoopBody> recognizeCrestedLoops(const InkOutline& outline, const QJsonObject& settings) {
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    const auto& ink = outline.ink;
    std::vector<CrestedLoopBody> result;
    for (auto crest = geometry.timeSorted.begin(); crest != geometry.timeSorted.end(); ++crest) {
        if (crest->point.y() >= geometry.prev(crest)->point.y() ||
            crest->point.y() >= geometry.next(crest)->point.y()) continue;
        const auto a = headFlank(geometry, crest, true), b = headFlank(geometry, crest, false);
        const auto rightEdge = a->point.x() > b->point.x() ? a : b;
        const auto leftEdge = rightEdge == a ? b : a;
        const auto r = rightEdge->point, l = leftEdge->point;
        if (r.x() <= crest->point.x() || l.x() >= crest->point.x() ||
            r.x() <= geometry.prev(rightEdge)->point.x() || r.x() <= geometry.next(rightEdge)->point.x() ||
            l.x() >= geometry.prev(leftEdge)->point.x() || l.x() >= geometry.next(leftEdge)->point.x()) continue;
        for (size_t i = 1; i < geometry.subPaths.size(); ++i) {
            const auto counter = geometry.subPaths[i].boundingRect();
            const double scale = counter.height();
            if (scale <= 0 || counter.width() <= 0 || ink.contains(counter.center()) ||
                counter.left() <= l.x() || counter.right() >= r.x() ||
                counter.top() <= crest->point.y() || counter.top() >= std::max(r.y(), l.y()) ||
                !matchesOffset(settings.value("crestToRight").toObject(), (r - crest->point) / scale) ||
                !matchesOffset(settings.value("crestToLeft").toObject(), (l - crest->point) / scale)) continue;
            result.push_back({counter, *crest, *rightEdge, *leftEdge});
        }
    }
    return result;
}

static std::vector<std::pair<Stretching, Stretching>> cutCrestedLoops(SearchContext& context,
        const std::vector<CrestedLoopBody>& bodies, const QJsonObject& policy) {
    std::vector<std::pair<Stretching, Stretching>> candidates;
    const auto& ink = context.transformedPath;
    for (const auto& body : bodies) {
        const auto& counter = body.counter;
        const auto* crest = &body.crest;
        const auto r = body.rightEdge.point, l = body.leftEdge.point;
        const double scale = counter.height();
            Stretching right{}, left{};
            const double distance = policy.value("maxNeckDistance").toDouble(2) * scale;
            if (!medialChordAt(context, r.x(), counter.bottom(), distance, right, false, counter.bottom()) ||
                !medialChordAt(context, l.x(), counter.bottom(), distance, left, false, counter.bottom())) continue;
            const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
            if (pieces.size() != 3 || !pieces[1].path.boundingRect().contains(counter) ||
                !pieces[1].path.contains(crest->point + QPointF(0, .01 * scale)) ||
                toSubpaths(pieces[1].path).size() != 2) continue;
            right.referenceWidth = QLineF(right.topBaseLine.point, right.bottomBaseLine.point).length();
            left.referenceWidth = QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length();
            candidates.emplace_back(right, left);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return Stretching::cmpbyx{}(a.first, b.first);
    });
    return candidates;
}

static bool searchZigzagKaf(SearchContext& context) {
    if (!searchKaf(context)) return false;
    const auto seed = context.res;
    const QLineF chord(seed.topBaseLine.point, seed.bottomBaseLine.point);
    const double stroke = chord.length();
    if (stroke <= 0) return false;
    const auto center = chord.center();
    const auto edge = context.form.value("edge").toObject();
    const auto flank = context.form.value("edgeFlank").toObject();
    std::optional<Stretching> aligned;
    for (auto point = context.timeSorted.begin(); point != context.timeSorted.end(); ++point) {
        const auto p = point->point;
        if (!matchesOffset(edge, (p - center) / stroke) ||
            !matchesOffset(flank, (context.prev(point)->point - p) / stroke) ||
            !matchesOffset(flank, (context.next(point)->point - p) / stroke)) continue;
        auto cut = seed;
        if (!medialChordAt(context, p.x(), center.y(), stroke, cut) ||
            cut.topBaseLine.point.y() <= p.y()) continue;
        if (aligned) return false;
        aligned = cut;
    }
    if (!aligned) return false;
    context.res = *aligned;
    context.res.fixedRightBoundary = true;
    context.res.length = 0;
    return true;
}

// A contour may record several landmarks along a single rising or falling flank.
static std::optional<PathExtrema> contourFlank(SearchContext& context, timesortedtype::iterator anchor,
                                               bool previous, const QJsonObject& bounds) {
    const auto advance = previous ? &SearchContext::prev : &SearchContext::next;
    auto current = anchor;
    QPointF direction;
    for (size_t step = 0; step < context.timeSorted.size(); ++step) {
        const auto next = (context.*advance)(current);
        const auto delta = next->point - current->point;
        if (step == 0) direction = delta;
        else if (delta.x() * direction.x() < -1e-8 || delta.y() * direction.y() < -1e-8) return {};
        if (matchesOffset(bounds, next->point - anchor->point)) return *next;
        if (!bounds.value("accumulate").toBool()) return {};
        current = next;
    }
    return {};
}

// Reusable paired contour routes, anchored on a feature of the body itself.
static std::vector<std::pair<Stretching, Stretching>> contourBodyBoundaries(
        SearchContext& context, const QJsonObject& settings, const Stretching* sharedExit = nullptr) {
    std::vector<std::pair<Stretching, Stretching>> candidates;
    const auto feature = settings.value("anchor").toObject();
    const auto flanks = feature.value("flanks").toArray();
    const auto originalForm = context.form;
    const auto originalResult = context.res;
    const auto originalNext = context.nextIter;
    for (auto anchor = context.timeSorted.begin(); anchor != context.timeSorted.end(); ++anchor) {
        if ((feature.contains("sharp") && feature.value("sharp").toBool() != anchor->isSharp) ||
            (feature.contains("xExtremum") && feature.value("xExtremum").toBool() != anchor->isX) ||
            (feature.contains("yExtremum") && feature.value("yExtremum").toBool() != anchor->isY)) continue;
        if (feature.contains("convex")) {
            const auto incoming = anchor->point - context.prev(anchor)->point;
            const auto outgoing = context.next(anchor)->point - anchor->point;
            const double turn = incoming.x() * outgoing.y() - incoming.y() * outgoing.x();
            const bool convex = context.orientation == Orientation::ClockWise ? turn < 0 : turn > 0;
            if (std::abs(turn) < 1e-10 || convex != feature.value("convex").toBool()) continue;
        }
        std::vector<double> retainedBranchTimes;
        if (flanks.size() == 2) {
            int first = 0;
            auto a = contourFlank(context, anchor, true, flanks[0].toObject());
            auto b = contourFlank(context, anchor, false, flanks[1].toObject());
            if (!a || !b) {
                first = 1;
                a = contourFlank(context, anchor, true, flanks[1].toObject());
                b = contourFlank(context, anchor, false, flanks[0].toObject());
            }
            if (!a || !b) continue;
            if (flanks[first].toObject().value("insideBody").toBool()) retainedBranchTimes.push_back(a->time);
            if (flanks[1-first].toObject().value("insideBody").toBool()) retainedBranchTimes.push_back(b->time);
        }
        // Either boundary may start from a second intrinsic feature. Resolve
        // it relative to this body's anchor, never a neighboring letter's cut.
        const auto secondaryAnchor = [&](const QJsonObject& secondary) {
            if (secondary.isEmpty()) return anchor;
            auto selected = context.timeSorted.end();
            double nearest = std::hypot(context.bbox.width(), context.bbox.height());
            bool found = false;
            for (auto point = context.timeSorted.begin(); point != context.timeSorted.end(); ++point) {
                if ((secondary.contains("sharp") && secondary.value("sharp").toBool() != point->isSharp) ||
                    (secondary.contains("xExtremum") && secondary.value("xExtremum").toBool() != point->isX) ||
                    (secondary.contains("yExtremum") && secondary.value("yExtremum").toBool() != point->isY) ||
                    !matchesOffset(secondary.value("offset").toObject(), point->point - anchor->point)) continue;
                const auto branches = secondary.value("flanks").toObject();
                if (!matchesOffset(branches, context.prev(point)->point - point->point) ||
                    !matchesOffset(branches, context.next(point)->point - point->point)) continue;
                const double distance = QLineF(anchor->point, point->point).length();
                if (distance < nearest) {
                    nearest = distance;
                    selected = point;
                    found = true;
                }
            }
            return found ? selected : context.timeSorted.end();
        };
        const auto rightSettings = settings.value("right").toObject();
        const auto leftSettings = settings.value("left").toObject();
        const auto rightAnchor = secondaryAnchor(rightSettings.value("anchor").toObject());
        const auto leftAnchor = secondaryAnchor(leftSettings.value("anchor").toObject());
        if (rightAnchor == context.timeSorted.end() || leftAnchor == context.timeSorted.end()) continue;
        bool hasFeatures = true;
        for (const auto& feature : settings.value("retain").toArray()) {
            const auto point = secondaryAnchor(feature.toObject());
            if (point == context.timeSorted.end()) { hasFeatures = false; break; }
            retainedBranchTimes.push_back(point->time);
        }
        if (!hasFeatures) continue;
        for (bool reverseProbe : {false, true}) {
        if (reverseProbe && !rightSettings.value("probeBothSides").toBool()) break;
        Stretching right{}, left{};
        context.form = rightSettings; context.res = {};
        if (!searchContourWalk(context, &*rightAnchor, reverseProbe)) continue;
        right = context.res;
        context.form = leftSettings;
        context.res = {};
        if (sharedExit) left = *sharedExit;
        else {
            if (!searchContourWalk(context, &*leftAnchor)) continue;
            left = context.res;
        }
        if (settings.value("orientToBody").toBool()) {
            // "Upper" and "lower" exchange sides on a folded stroke. The
            // retained body feature determines the contour side of each cut.
            if (!getJoinTime(right, context.orientation).isLeft(anchor->time))
                std::swap(right.topBaseLine, right.bottomBaseLine);
            if (!getJoinTime(left, context.orientation).isRight(anchor->time))
                std::swap(left.topBaseLine, left.bottomBaseLine);
        }
        const auto leftOfRight = getJoinTime(right, context.orientation);
        if (!leftOfRight.isStrictLeft(left.topBaseLine.time) || !leftOfRight.isStrictLeft(left.bottomBaseLine.time)) continue;
        // A route must retain its defining landmark in the actual contour
        // interval. Bounding-box containment alone can accept a wrapped cut
        // that leaves this head on the other side of the boundary.
        if (!getJoinTime(right, context.orientation).isLeft(anchor->time) ||
            !getJoinTime(left, context.orientation).isRight(anchor->time)) continue;
        // A defining branch belongs to this body, too. Keeping just its elbow
        // can otherwise accept cuts that assign the entire rising arm elsewhere.
        if (std::any_of(retainedBranchTimes.begin(), retainedBranchTimes.end(), [&](double time) {
            return !getJoinTime(right, context.orientation).isLeft(time) ||
                !getJoinTime(left, context.orientation).isRight(time);
        })) continue;
        const auto pieces = cutRegions(context.transformedPath, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3 ||
            !pieces[1].path.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6).contains(anchor->point)) continue;
        const bool duplicate = std::any_of(candidates.begin(), candidates.end(), [&](const auto& pair) {
            return QLineF(pair.first.topBaseLine.point, right.topBaseLine.point).length() < 1e-5 &&
                QLineF(pair.second.topBaseLine.point, left.topBaseLine.point).length() < 1e-5;
        });
        if (!duplicate) candidates.emplace_back(right, left);
        }
    }
    context.form = originalForm;
    context.res = originalResult;
    context.nextIter = originalNext;
    return candidates;
}

// Locate a tall stem from its own crest and width; a bowl may extend to its left.
struct TerminalStemBody {
    timesortedtype::iterator crest, limit, bowlEdge;
    QPointF bowlBottom, sample;
    double stroke;
    int traversal;
};

static std::vector<TerminalStemBody> recognizeTerminalStemBodies(SearchContext& context, const QJsonObject& body) {
    // The ascender is on the right of the final bowl. Its shorter upturned
    // left end is also a narrow stroke, but is not the letter's incoming stem.
    const auto bowlEdge = context.timeSorted.find(*std::prev(context.xSorted.end()));
    std::optional<QPointF> bowlBottom;
    for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
        auto bottom = bowlEdge;
        for (size_t step = 0; step < context.timeSorted.size(); ++step) {
            const auto next = (context.*advance)(bottom);
            if (next->point.y() <= bottom->point.y()) break;
            bottom = next;
        }
        if (bottom != bowlEdge && (!bowlBottom || bottom->point.y() > bowlBottom->y()))
            bowlBottom = bottom->point;
    }
    if (!bowlBottom) return {};
    std::vector<TerminalStemBody> bodies;
    for (auto crest = context.timeSorted.begin(); crest != context.timeSorted.end(); ++crest) {
        if (crest->point.x() <= bowlBottom->x() ||
            crest->point.y() >= context.prev(crest)->point.y() ||
            crest->point.y() >= context.next(crest)->point.y()) continue;
        for (auto advance : {&SearchContext::next, &SearchContext::prev}) {
            auto limit = crest;
            for (size_t step = 0; step < context.timeSorted.size(); ++step) {
                auto next = (context.*advance)(limit);
                if (next->point.y() <= limit->point.y()) break;
                limit = next;
            }
            const double rise = limit->point.y() - crest->point.y();
            if (rise <= 0) continue;
            const double y = crest->point.y() + rise * 0.25;
            const auto hits = FindInterections(context.transformedPath,
                QLineF(context.bbox.left() - 1, y, context.bbox.right() + 1, y), true);
            double stroke = 0, nearest = rise;
            QPointF stemSample;
            for (size_t i = 1; i < hits.size(); ++i) {
                if (!interiorChord(context.transformedPath, hits[i - 1], hits[i])) continue;
                const QLineF chord(hits[i - 1].point, hits[i].point);
                const double distance = std::abs(chord.center().x() - crest->point.x());
                if (distance < nearest) {
                    nearest = distance;
                    stroke = chord.length();
                    stemSample = chord.center();
                }
            }
            if (stroke <= 0 || rise < body.value("minRise").toDouble(3) * stroke || nearest > stroke) continue;
            bodies.push_back({crest, limit, bowlEdge, *bowlBottom, stemSample, stroke,
                advance == &SearchContext::prev ? 1 : -1});
        }
    }
    return bodies;
}

static bool cutTerminalStemBodies(SearchContext& context, const std::vector<TerminalStemBody>& bodies,
        const QJsonObject& body, const QJsonObject& policy) {
    std::optional<Stretching> best;
    double leftmost = context.bbox.right() + 1;
    for (const auto& evidence : bodies) {
        const auto& [crest, limit, bowlEdge, bowlBottom, stemSample, stroke, traversal] = evidence;
        if (crest->point.x() >= leftmost) continue;
        Stretching cut{};
        if (!alefTurnCut(context, *limit, policy.value("turnAngle").toDouble(45), stroke, cut, &*crest,
                        traversal)) continue;
        std::vector<Stretching> options{cut};
        // An incoming stroke may attach above the bowl. Its inward
        // corner distinguishes that neck from a chord through the stem
        // itself, which would assign the whole bowl to the kashida.
        const auto foot = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).center();
        for (auto corner = context.timeSorted.begin(); corner != context.timeSorted.end(); ++corner) {
            if (!corner->isSharp || corner->point.x() <= crest->point.x() ||
                corner->point.y() >= context.prev(corner)->point.y() ||
                corner->point.y() >= context.next(corner)->point.y() ||
                QLineF(foot, corner->point).length() >
                    body.value("maxNeckDistance").toDouble(2) * stroke) continue;
            const bool increasing = context.prev(corner)->point.x() > context.next(corner)->point.x();
            Stretching neck{};
            if (contourNormalCut(context, *corner, increasing, false, neck, true)) {
                std::swap(neck.topBaseLine, neck.bottomBaseLine);
                options.push_back(neck);
            }
        }
        std::optional<Stretching> accepted;
        for (const auto& option : options) {
            const auto pieces = cutRegions(context.transformedPath, {}, {option}, context.orientation, true);
            if (pieces.size() != 2) continue;
            const auto& body = pieces[1].path;
            const auto bounds = body.boundingRect().adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
            if (!bounds.contains(crest->point) || !bounds.contains(bowlEdge->point) ||
                !body.contains(stemSample) || !body.contains(bowlBottom - QPointF(0, .01 * stroke))) continue;
            accepted = option;
            break;
        }
        if (!accepted) continue;
        best = *accepted;
        leftmost = crest->point.x();
    }
    if (!best) return false;
    context.res = *best;
    context.res.topLeft = context.res.topRight = best->topBaseLine;
    context.res.bottomLeft = context.res.bottomRight = best->bottomBaseLine;
    return true;
}

static bool searchStemEntry(SearchContext& context) {
    return cutTerminalStemBodies(context, recognizeTerminalStemBodies(context, context.form),
                                 context.form, context.form);
}

// Walk the lower side of the outline and select a cross-section near its landmark.
static bool medialBodyChord(SearchContext& context, const PathExtrema& start,
                            const QJsonObject& boundary, Stretching& cut) {
    const auto steps = boundary.value("walk").toArray();
    if (steps.isEmpty()) return false;
    auto current = context.timeSorted.find(start);
    const auto advance = context.orientation == Orientation::ClockWise ? &SearchContext::prev : &SearchContext::next;
    auto landmark = current;
    int remaining = static_cast<int>(context.timeSorted.size());
    for (const auto& step : steps) {
        auto bounds = step.toObject();
        if (step.isString()) bounds.insert("direction", step);
        bool found = false;
        while (remaining-- > 0) {
            const auto next = (context.*advance)(current);
            if (matchesOffset(bounds, next->point - current->point)) {
                const auto maxStepEnd = boundary.value("maxStepEndDistance");
                const bool useEnd = boundary.value("useStepEnd").toBool() &&
                    (!maxStepEnd.isDouble() || QLineF(current->point, next->point).length() <= maxStepEnd.toDouble());
                landmark = useEnd ? next : current;
                current = next;
                found = true;
                break;
            }
            current = next;
        }
        if (!found) return false;
    }

    const double x = landmark->point.x() + boundary.value("offsetX").toDouble();
    return medialChordAt(context, x, landmark->point.y(), 1, cut, true);
}

static bool connectorContainsLeadingStroke(const QVector<PathExtrema>& extrema,
        const Stretching& right, const Stretching& left, Orientation orientation) {
    const PathExtrema* origin = nullptr;
    for (const auto& p : extrema) if (p.pathIndex == 0 &&
        (!origin || p.point.x() > origin->point.x())) origin = &p;
    return origin && getJoinTime(right, orientation).isStrictLeft(origin->time) &&
        getJoinTime(left, orientation).isStrictRight(origin->time);
}

// Partition a local proposal. Policy trials never mutate a published subword.
static bool assembleBodyPartition(const Word& word, WordResultInfo::SubWordInfo& subword,
                                  const QJsonObject& config) {
    const auto shape = word.bodyShape(subword);
    auto ink = shape.transform.map(shape.path);
    ink.translate(shape.pos);
    ink.setFillRule(Qt::WindingFill);

    std::vector<Stretching> cuts(subword.joins.begin(), subword.joins.end());
    subword.kashidaPaths.assign(cuts.size(), {});
    subword.kashidaCuts.assign(cuts.size(), {});
    if (cuts.empty()) {
        subword.baseGlyphs = {{subword.text, shape.path}};
        return true;
    }
    auto regions = cutRegions(shape.path, subword.text, cuts, subword.basePathOrientation);
    if (regions.size() != cuts.size() + 1) {
        subword.baseGlyphs = std::move(regions);
        return false;
    }

    // Replace a single cut with the two contour chords at the ends of its join.
    // Keep the single cut if either endpoint is ambiguous or the cuts conflict.
    const auto defaults = config.value("kashidaDefaults").toObject();
    std::vector<size_t> joinRegions;
    size_t cutIndex = 0;
    for (const auto& join : subword.joins) {
        const size_t candidate = cutIndex++;
        joinRegions.push_back(0);
        const auto settings = withOverrides(defaults, join.kashidaOverrides);
        if (!settings.value("enabled").toBool(true)) continue;

        const double neighborRatio = settings.value("neighborSearchRatio").toDouble();
        auto right = join;
        auto left = join;
        const double x = QLineF(join.topBaseLine.point, join.bottomBaseLine.point).center().x();
        const auto cutX = [](const Stretching& cut) {
            return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).center().x();
        };
        const double minX = candidate + 1 < cuts.size() ? x + neighborRatio * (cutX(cuts[candidate + 1]) - x) : ink.boundingRect().left();
        const double maxX = candidate > 0 ? x + neighborRatio * (cutX(cuts[candidate - 1]) - x) : ink.boundingRect().right();
        if (join.fixedLeftBoundary) {
            left.topBaseLine = join.topLeft;
            left.bottomBaseLine = join.bottomLeft;
        } else if (!settings.value("trackingEnabled").toBool(false) ||
                   !findJoinEnds(ink, join, minX, maxX, settings, left, right)) {
            if (join.length > 0 &&
                interiorChord(ink, join.topRight, join.bottomRight) &&
                interiorChord(ink, join.topLeft, join.bottomLeft)) {
                if (!join.fixedRightBoundary) {
                    right.topBaseLine = join.topRight;
                    right.bottomBaseLine = join.bottomRight;
                }
                left.topBaseLine = join.topLeft;
                left.bottomBaseLine = join.bottomLeft;
            } else continue;
        }

        const double minSpan = settings.value("minSpan").toDouble();
        if (minSpan > 0 && cutX(right) - cutX(left) < minSpan) continue;

        // Attachment order follows the contour, not X on the page. Folded
        // bodies can have an incoming endpoint to the left of their exit.
        const auto before = [&](const Stretching& a, const Stretching& b) {
            return cutPrecedes(a, b, subword.basePathOrientation);
        };
        if (!before(right, left) ||
            (candidate > 0 && !before(cuts[candidate - 1], right)) ||
            (candidate + 1 < cuts.size() && !before(left, cuts[candidate + 1]))) continue;

        auto trial = cuts;
        trial[candidate] = right;
        trial.insert(trial.begin() + candidate + 1, left);
        const auto chord = [](const Stretching& cut) {
            return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
        };
        const auto crosses = [&](const Stretching& a, const Stretching& b) {
            return !sharesLowerForkTip(a, b) && !sharesLowerForkTip(b, a) &&
                chord(a).intersects(chord(b), nullptr) == QLineF::BoundedIntersection;
        };
        bool crossed = false;
        for (size_t other = 0; other < trial.size(); ++other) {
            if (other != candidate && crosses(right, trial[other]))
                crossed = true;
            if (other != candidate + 1 && crosses(left, trial[other]))
                crossed = true;
        }
        if (crossed) continue;
        auto pieces = cutRegions(shape.path, subword.text, trial, subword.basePathOrientation, true);
        if (pieces.size() != trial.size() + 1 ||
            std::any_of(pieces.begin(), pieces.end(), [](const GlyphInfo& piece) {
                return piece.path.isEmpty() || piece.path.boundingRect().isEmpty();
            })) continue;
        if (candidate == 0) {
            // The first connecting span cannot wrap around the leading stroke.
            // Check the actual proposed span, after shared ligature boundaries
            // have been reconciled, rather than rejecting provisional walks
            // that a later intrinsic ligature template legitimately replaces.
            if (connectorContainsLeadingStroke(subword.extrema, right, left, subword.basePathOrientation)) {
                subword.templateTrace.append("kashida · leading stroke before initial body rejected");
                subword.baseGlyphs = std::move(regions);
                return false;
            }
        }
        cuts = std::move(trial);
        regions = std::move(pieces);
        subword.kashidaCuts[joinRegions.size() - 1] = {
            QLineF(right.topBaseLine.point, right.bottomBaseLine.point),
            QLineF(left.topBaseLine.point, left.bottomBaseLine.point)
        };
        joinRegions.back() = candidate + 1;
        ++cutIndex;
    }

    for (size_t index = joinRegions.size(); index-- > 0;) {
        const auto region = joinRegions[index];
        if (!region) continue;
        subword.kashidaPaths[index] = std::move(regions[region].path);
        regions.erase(regions.begin() + region);
    }
    subword.baseGlyphs = std::move(regions);
    return true;
}

bool PageAnalysisResult::cutSubword(int lineIndex, int wordIndex, int subWordIndex, const QJsonObject& config) {
    const auto& word = page.lines[lineIndex].words[wordIndex];
    return assembleBodyPartition(word, page.lines[lineIndex].words[wordIndex].wordResultInfo.subWords[subWordIndex], config);
}

QString PageAnalysisResult::bodyLetters(const QString& text) {
    QString letters;
    for (const auto ch : text) {
        // U+0640 supplies joining/spacing (often under a combining hamza),
        // not an extra letter body. The marks remain in the source components.
        if (!ch.isMark() && ch != QChar(0x0640)) letters += ch;
    }
    return letters;
}

// Start halfway down the visible terminal shaft. Its cap can have the same
// tangent as the turn at its foot, but only the foot retains this shaft.
static bool terminalStemTurn(SearchContext& context, const QJsonObject& form, Stretching& result) {
    const auto& ink = context.transformedPath;
    const auto terminal = std::prev(context.xSorted.end());
    const auto box = context.bbox;
    const double y = terminal->point.y() + .45 * (box.bottom() - terminal->point.y());
    auto shaft = FindInterections(ink, QLineF(box.left() - 1, y, box.right() + 1, y), true);
    std::sort(shaft.begin(), shaft.end(), [](const auto& a, const auto& b) { return a.point.x() < b.point.x(); });
    if (shaft.size() < 2 || !interiorChord(ink, shaft[0], shaft[1])) return false;
    const QLineF section(shaft[0].point, shaft[1].point);
    const double width = section.length();
    if (width <= 0 || y - terminal->point.y() < 2 * width || section.center().x() - box.left() > 3 * width)
        return false;
    const int count = static_cast<int>(baseContourCurves(ink).size());
    for (int i : {1, 0})
        for (bool forward : {true, false}) {
            const auto& start = shaft[i];
            PathExtrema end = start, next{};
            end.time += forward ? count : -count;
            QPointF tangent;
            if (!advanceOnContour(ink, start, end, forward, .5 * width, next, tangent) || next.point.y() <= y)
                continue;
            for (const auto& foot : context.timeSorted) {
                if (foot.point.y() <= y + width || foot.point.x() <= section.p1().x()) continue;
                Stretching cut{};
                if (!alefTurnCut(context, foot, form.value("turnAngle").toDouble(45), width, cut, &start,
                                 forward ? 1 : -1))
                    continue;
                const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
                if (line.length() < .55 * width || line.length() > 2.8 * width || line.center().y() < y + width ||
                    line.center().x() > section.center().x() + 4 * width)
                    continue;
                if (getJoinTime(cut, context.orientation).isStrictRight(terminal->time))
                    std::swap(cut.topBaseLine, cut.bottomBaseLine);
                const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
                if (pieces.size() != 2 || !pieces[1].path.contains(section.center()) ||
                    toSubpaths(pieces[1].path).size() != 1)
                    continue;
                cut.referenceWidth = width;
                result = cut;
                return true;
            }
        }
    return false;
}

// Follow the underside of the terminal tail to the fork where the incoming
// stroke branches off. Walk one pen width down that branch and take its normal.
static bool terminalTailEntry(SearchContext& context, const QJsonObject& settings, Stretching& result) {
    const auto& ink = context.transformedPath;
    const auto tail = context.timeSorted.find(*std::prev(context.xSorted.end()));
    const int count = static_cast<int>(baseContourCurves(ink).size());
    for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
        auto corner = tail;
        for (size_t i = 1; i < context.timeSorted.size(); ++i) {
            const auto next = (context.*advance)(corner);
            const auto step = next->point - corner->point;
            if (step.x() < -.01) break;
            const auto prev = advance == &SearchContext::prev ? context.next(corner) : context.prev(corner);
            if (corner->isSharp && i > 1 && corner->point.y() < prev->point.y() && step.y() > 0 && step.x() > 0) {
                const bool forward = advance == &SearchContext::prev;
                Stretching first{};
                if (contourNormalCut(context, *corner, forward, false, first, true)) {
                    const double width = first.referenceWidth;
                    const double run = corner->point.x() - tail->point.x();
                    if (run >= settings.value("minRun").toDouble(2) * width && run <= settings.value("maxRun").toDouble(12) * width) {
                        PathExtrema limit = *corner, lower{};
                        limit.time += forward ? count : -count;
                        QPointF tangent;
                        Stretching cut{};
                        bool found;
                        if (settings.contains("turnAngle")) {
                            // From the bowl fork, follow the inner edge down to the foot of the shaft.
                            found = alefTurnCut(context, limit, settings.value("turnAngle").toDouble(), width, cut,
                                                &*corner, forward ? 1 : -1, true);
                            if (found && !getJoinTime(cut, context.orientation).isStrictLeft(tail->time))
                                std::swap(cut.topBaseLine, cut.bottomBaseLine);
                            found = found && cut.topBaseLine.point.y() > corner->point.y() &&
                                    cut.bottomBaseLine.point.x() > corner->point.x();
                        } else {
                            found = advanceOnContour(ink, *corner, limit, forward,
                                                     settings.value("walkWidths").toDouble(1) * width, lower,
                                                     tangent) &&
                                    lower.point.x() > corner->point.x() && lower.point.y() > corner->point.y() &&
                                    contourNormalCut(context, lower, forward, false, cut, true);
                            if (found)
                                std::swap(cut.topBaseLine, cut.bottomBaseLine);
                        }
                        if (found) {
                            const auto side = getJoinTime(cut, context.orientation);
                            const auto pieces = cutRegions(ink, {}, {cut}, context.orientation, true);
                            if (side.isStrictLeft(tail->time) && side.isStrictLeft(corner->time) &&
                                pieces.size() == 2 && toSubpaths(pieces[1].path).size() == 1 &&
                                corner->point.y() - pieces[1].path.boundingRect().top() >=
                                    settings.value("minRise").toDouble(2) * width) {
                                result = cut;
                                return true;
                            }
                        }
                    }
                }
            }
            corner = next;
        }
    }
    return false;
}

// Enumerate distinct, geometrically valid readings of a prefix only when a
// subsequent tooth is unresolved. Curves meeting at the same seam are one choice.
// A stacked tooth can have a valid cut across its cap while the descending
// stroke below it is still inside the hah mask. Its true shared boundary is
// the horizontal chord at the sharp junction with the top of the hah head.
static std::optional<Stretching> stackedAttachment(SearchContext& context, const QJsonObject& settings,
                                                   const QRectF& cap, const QPainterPath& head,
                                                   const Stretching& incoming, const Stretching& outgoing,
                                                   bool clippedCap = false) {
    const auto headFlank = settings.value("headFlank").toObject();
    const auto risingFlank = settings.value("risingFlank").toObject();
    const auto between = [&](const PathExtrema& point) {
        return getJoinTime(incoming, context.orientation).isLeft(point.time) &&
               getJoinTime(outgoing, context.orientation).isStrictRight(point.time);
    };
    const auto atIncoming = [&](const PathExtrema& point) {
        return QLineF(point.point, incoming.topBaseLine.point).length() < 1e-5 ||
               QLineF(point.point, incoming.bottomBaseLine.point).length() < 1e-5;
    };
    std::optional<Stretching> candidate;
    for (auto at = context.timeSorted.begin(); at != context.timeSorted.end(); ++at) {
        if (!at->isSharp || !at->isX || !between(*at)) continue;
        if (!(contourFlank(context, at, true, headFlank) && contourFlank(context, at, false, risingFlank)) &&
            !(contourFlank(context, at, false, headFlank) && contourFlank(context, at, true, risingFlank))) continue;
        auto opposite = horizontalOpposite(context, *at, true);
        bool foundFromOpposite = false;
        if (!opposite && clippedCap) {
            // At a cusp an intersection can be reported from only one end.
            for (const auto& edge : context.timeSorted) {
                if (!edge.isSharp || edge.point.x() >= at->point.x() ||
                    std::abs(edge.point.y() - at->point.y()) > .05 || !between(edge)) continue;
                const auto hit = horizontalOpposite(context, edge, false);
                if (!hit || QLineF(hit->point, at->point).length() > .05) continue;
                if (opposite) return {};
                opposite = edge;
                foundFromOpposite = true;
            }
        }
        if (!opposite || (!atIncoming(*at) &&
            (!between(*opposite) || !head.contains((opposite->point + at->point) / 2)))) continue;
        Stretching shared{};
        shared.topBaseLine = shared.topLeft = shared.topRight = *opposite;
        shared.bottomBaseLine = shared.bottomLeft = shared.bottomRight = *at;
        if (at->point.y() <= cap.bottom() + 1e-5) {
            // An intrinsic initial-body cut can already reach this junction
            // from a rounded lower flank. Keep the established horizontal
            // attachment instead of treating that small overshoot as a new body.
            if (atIncoming(*at) && cap.bottom() - at->point.y() <=
                QLineF(opposite->point, at->point).length()) {
                shared.fixedRightBoundary = shared.fixedLeftBoundary = true;
                shared.kashidaOverrides.insert("enabled", false);
                if (candidate) return {};
                candidate = shared;
                continue;
            }
            // The first walk can clip a speck off the hah lip below the real junction.
            // Only disregard that cap if its entire extent is under one local stroke.
            const double extent = std::hypot(cap.width(), cap.height());
            auto normal = shared;
            if (extent >= QLineF(opposite->point, at->point).length() ||
                (!foundFromOpposite && (!strokeNormalCut(context, normal) ||
                    extent >= QLineF(normal.topBaseLine.point, normal.bottomBaseLine.point).length()))) continue;
        }
        if (candidate) return {};
        shared.fixedRightBoundary = shared.fixedLeftBoundary = true;
        shared.kashidaOverrides.insert("enabled", false);
        candidate = shared;
    }
    return candidate;
}

// Probe the receiving side of an attachment corner. A curved cross-section
// can meet either branch at a sharp turn; both directions give actual ink chords.
static std::vector<Stretching> cornerSections(const SearchContext& context, const PathExtrema& corner) {
    std::vector<Stretching> sections;
    for (bool forward : {true, false}) {
        Stretching s{};
        if (contourNormalCut(context, corner, forward, false, s))
            sections.push_back(s);
    }
    const double reach = std::hypot(context.bbox.width(), context.bbox.height()) + 1;
    for (auto delta : {QPointF(0, -reach), QPointF(0, reach), QPointF(-reach, 0), QPointF(reach, 0)}) {
        const auto hits =
            FindInterections(context.transformedPath, QLineF(corner.point, corner.point + delta), true);
        for (const auto& hit : hits)
            if (interiorChord(context.transformedPath, corner, hit)) {
                Stretching s{};
                s.topBaseLine = corner;
                s.bottomBaseLine = hit;
                s.referenceWidth = QLineF(corner.point, hit.point).length();
                sections.push_back(s);
                break;
            }
    }
    return sections;
}
static void orientSection(const SearchContext& ctx, Stretching& cut, double witness, bool left) {
    if (getJoinTime(cut, ctx.orientation).isLeft(witness) != left)
        std::swap(cut.topBaseLine, cut.bottomBaseLine);
    cut.topRight = cut.topLeft = cut.topBaseLine;
    cut.bottomLeft = cut.bottomRight = cut.bottomBaseLine;
}

// The bowl's upper flank can rise into its own crest beyond the proposed cut.
// Follow that flank to its first turn, then cross the narrow stroke after it.
// The bowl depth bounds the search, so it cannot reach a preceding tooth.
static std::optional<Stretching> cutAfterEntryCrest(SearchContext& ctx, Stretching cut,
                                                  const Stretching* preceding = nullptr,
                                                  bool allowOblique = false) {
    const auto& tip = *ctx.xSorted.rbegin();
    orientSection(ctx, cut, tip.time, true);
    const auto side = getJoinTime(cut, ctx.orientation);
    const auto& upper = cut.topBaseLine;
    const auto& lower = cut.bottomBaseLine;
    if (upper.point.y() > lower.point.y() + 1e-5) return {};
    const bool upperIncreases = (upper.time < lower.time) == side.startAtLeft;
    const auto firstAfter = [&](const PathExtrema& point, bool increases) {
        if (increases) return ctx.prev(ctx.timeSorted.lower_bound(point));
        auto next = ctx.timeSorted.upper_bound(point);
        return next == ctx.timeSorted.end() ? ctx.timeSorted.begin() : next;
    };
    const auto advance = [&](auto point, bool increases) {
        return increases ? ctx.prev(point) : ctx.next(point);
    };
    auto crest = upper;
    auto point = firstAfter(upper, upperIncreases);
    for (; side.isStrictRight(point->time); point = advance(point, upperIncreases)) {
        if (point->point.y() > crest.point.y() + 1e-5) break;
        if (point->point.y() < crest.point.y()) crest = *point;
    }
    if (!side.isStrictRight(point->time) || point->point.x() <= crest.point.x()) return {};
    double floor = lower.point.y();
    for (const auto& landmark : ctx.timeSorted)
        if (side.isLeft(landmark.time)) floor = std::max(floor, landmark.point.y());
    const double depth = floor - crest.point.y();
    const double rise = upper.point.y() - crest.point.y();
    // A short cut across two inner corners can sit below a broad hook.
    // The first turn beyond the crest then lines up with the lower corner
    // even though the crest itself is farther to the left.
    const bool alignedShoulder = allowOblique && upper.isSharp && lower.isSharp &&
        QLineF(upper.point, lower.point).length() < .1 * depth &&
        std::abs(point->point.x() - lower.point.x()) < .08 * depth;
    if (depth <= 0 || rise < .27 * depth ||
        (std::abs(crest.point.x() - lower.point.x()) > .24 * depth && !alignedShoulder)) return {};

    std::optional<Stretching> best;
    double bestScore = std::numeric_limits<double>::infinity();
    const auto consider = [&](const PathExtrema& top, const PathExtrema& bottom) {
        const double width = QLineF(top.point, bottom.point).length();
        if (top.point.y() >= bottom.point.y() || width < .07 * depth || width > .34 * depth ||
            !side.isStrictRight(top.time) || !interiorChord(ctx.transformedPath, top, bottom)) return;
        if (preceding) {
            const auto limit = getJoinTime(*preceding, ctx.orientation);
            if (!limit.isStrictLeft(top.time) || !limit.isStrictLeft(bottom.time)) return;
        }
        auto candidate = cut;
        candidate.topBaseLine = top;
        candidate.bottomBaseLine = bottom;
        orientSection(ctx, candidate, tip.time, true);
        const auto expanded = getJoinTime(candidate, ctx.orientation);
        if (!expanded.isStrictLeft(crest.time) || !expanded.isLeft(upper.time) ||
            !expanded.isLeft(lower.time)) return;
        const double score = width + .4 * (bottom.point.x() - crest.point.x());
        if (score >= bestScore) return;
        candidate.referenceWidth = width;
        candidate.fixedRightBoundary = true;
        candidate.length = 0;
        best = candidate;
        bestScore = score;
    };
    const auto probe = [&](const PathExtrema& bottom) {
        const double dx = bottom.point.x() - crest.point.x();
        if (dx < .04 * depth || dx > .35 * depth) return;
        const QLineF vertical(bottom.point, QPointF(bottom.point.x(), crest.point.y()));
        for (const auto& top : FindInterections(ctx.transformedPath, vertical, true))
            consider(top, bottom);
    };
    probe(lower);
    for (auto p = firstAfter(lower, !upperIncreases); side.isStrictRight(p->time);
         p = advance(p, !upperIncreases)) {
        if (p->point.x() - crest.point.x() > .35 * depth) break;
        probe(*p);
    }
    if (!best && allowOblique && point->point.x() - crest.point.x() < .20 * depth) {
        // The join can lean past the lower corner. A vertical probe then
        // misses the neck or crosses outside the ink.
        for (auto p = firstAfter(lower, !upperIncreases); side.isStrictRight(p->time);
             p = advance(p, !upperIncreases)) {
            const double dx = p->point.x() - crest.point.x();
            if (dx > .35 * depth) break;
            if (p->isSharp && dx > 0 && std::abs(p->point.x() - point->point.x()) < .15 * depth)
                consider(*point, *p);
        }
    }
    return best;
}

static std::optional<Stretching> restoreNoonBowlEntry(SearchContext& ctx, Stretching entry,
                                                     Stretching sending) {
    const auto& tip = *ctx.xSorted.rbegin();
    orientSection(ctx, entry, tip.time, true);
    orientSection(ctx, sending, tip.time, true);
    const auto old = getJoinTime(entry, ctx.orientation);
    const auto limit = getJoinTime(sending, ctx.orientation);
    auto floor = ctx.timeSorted.end();
    for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p)
        if (old.isLeft(p->time) && (floor == ctx.timeSorted.end() || p->point.y() > floor->point.y()))
            floor = p;
    if (floor == ctx.timeSorted.end()) return {};

    Stretching foot{};
    if (!medialChordAt(ctx, floor->point.x(), floor->point.y(), 1., foot, true)) return {};
    const double width = QLineF(foot.topBaseLine.point, foot.bottomBaseLine.point).length();
    if (width <= 1e-5) return {};
    for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
        auto bulge = floor;
        // The lower outline of the bowl moves right and rises from its floor.
        for (size_t n = 0; n < ctx.timeSorted.size(); ++n) {
            const auto next = (ctx.*advance)(bulge);
            if (next->point.x() < bulge->point.x() - 1e-5 ||
                next->point.y() > bulge->point.y() + .05 * width) break;
            bulge = next;
        }
        if (old.isLeft(bulge->time) ||
            bulge->point.x() - floor->point.x() < 1.5 * width ||
            floor->point.y() - bulge->point.y() < width) continue;
        auto elbow = bulge;
        // At the right flank the outer outline turns inward, then heads
        // outward into the connecting stroke.
        for (size_t n = 0; n < ctx.timeSorted.size(); ++n) {
            const auto next = (ctx.*advance)(elbow);
            if (next->point.x() > elbow->point.x() + 1e-5 ||
                next->point.y() > elbow->point.y() + .05 * width) break;
            elbow = next;
        }
        if (bulge->point.y() - elbow->point.y() < .35 * width ||
            bulge->point.x() - elbow->point.x() > .8 * width) continue;
        for (double offset : {.12, .25, .45, .7, 1.}) {
            Stretching candidate{};
            const double x = bulge->point.x() + offset * width;
            if (!medialChordAt(ctx, x, elbow->point.y(), 1.5 * width, candidate) ||
                QLineF(candidate.topBaseLine.point, candidate.bottomBaseLine.point).length() > 1.75 * width) continue;
            orientSection(ctx, candidate, tip.time, true);
            const auto side = getJoinTime(candidate, ctx.orientation);
            if (!side.isStrictLeft(bulge->time) || !side.isLeft(elbow->time) ||
                !side.isLeft(entry.topBaseLine.time) || !side.isLeft(entry.bottomBaseLine.time) ||
                !limit.isStrictLeft(candidate.topBaseLine.time) ||
                !limit.isStrictLeft(candidate.bottomBaseLine.time)) continue;
            return candidate;
        }
    }
    return {};
}

// Follow both flanks from the far end of the bowl. On each flank the long
// sweep returns left, then turns right into the diagonal of the upper letter.
static bool foldedBowlEntry(SearchContext& ctx, const QJsonObject& spec, Stretching& cut,
                            double* shoulderX = nullptr) {
    const auto tip = ctx.timeSorted.find(*ctx.xSorted.rbegin());
    if (tip == ctx.timeSorted.end()) return false;
    const auto turn = [&](bool forward) -> std::optional<std::pair<PathExtrema, double>> {
        auto peak = tip, bend = tip, it = tip;
        bool returning = false;
        for (size_t i = 1; i < ctx.timeSorted.size(); ++i) {
            it = forward ? ctx.next(it) : ctx.prev(it);
            const auto p = it->point;
            if (!returning) {
                if (p.x() >= peak->point.x()) peak = it;
                else if (peak->point.x() - tip->point.x() >= spec.value("minBowlRun").toDouble() &&
                         peak->point.x() - p.x() >= spec.value("minFoldRun").toDouble()) {
                    returning = true; bend = it;
                }
            } else if (p.x() < bend->point.x()) bend = it;
            else if (p.x() - bend->point.x() >= spec.value("minReturn").toDouble()) {
                if (bend->point.y() >= peak->point.y()) return {};
                return std::pair{*bend, peak->point.x()};
            }
        }
        return {};
    };
    const auto a = turn(true), b = turn(false);
    if (!a || !b || QLineF(a->first.point,b->first.point).length() > spec.value("maxChord").toDouble() ||
        !interiorChord(ctx.transformedPath,a->first,b->first)) return false;
    cut.topBaseLine = a->first; cut.bottomBaseLine = b->first;
    orientSection(ctx, cut, tip->time, true);
    cut.fixedLeftBoundary = cut.fixedRightBoundary = true;
    if (shoulderX) *shoulderX = std::max(a->second, b->second);
    return true;
}

static bool readTerminalOutline(SearchContext& context) {
    context.transformedPath.setFillRule(Qt::WindingFill);
    context.bbox = context.transformedPath.boundingRect();
    QVector<PathExtrema> landmarks;
    ShapeItem::getExtrema(context.transformedPath, context.subPaths, landmarks);
    for (const auto& point : landmarks) if (point.pathIndex == 0) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    if (context.timeSorted.size() < 3) return false;
    context.setOrientation();
    return true;
}

// Phase 1: reusable evidence from the uncut source. Neither masks nor cuts are
// inputs. A policy can be changed and previewed using this same recognition.
static bool foldedArmRoot(SearchContext& ctx, const QJsonObject& form, Stretching& result,
                          bool* cornerPair = nullptr);

struct LamBodyRecognition {
    LamBodyEvidence lamBodies;
    std::optional<std::pair<PathExtrema, PathExtrema>> alefAttachment;
};

struct SubwordBodyRecognition {
    QPainterPath source;
    std::vector<QPainterPath> contours;
    QVector<PathExtrema> landmarks;
    QMap<QByteArray, LamBodyRecognition> lamProfiles;
    Orientation orientation = Orientation::ClockWise;
    bool valid = false;
};

struct DeclaredBodyForm {
    QString current;
    QJsonObject body, cuts;
};

struct BodyCutProgram {
    QMap<QString, DeclaredBodyForm> forms;
    QString letters;
};

// QJsonObject copies share their data until edited. Reuse the resolved
// declarations across a page; a template preview invalidates this bounded
// cache by value, without keeping any source geometry or segmentation result.
static const BodyCutProgram& bodyCutProgram(const QJsonObject& config) {
    static thread_local QJsonObject compiledConfig;
    static thread_local BodyCutProgram program;
    if (compiledConfig == config) return program;
    program = {};
    for (const auto* phase : {"initial", "medial", "final"})
        for (const auto& value : config.value(phase).toArray()) {
            const auto form = value.toObject();
            if (!form.contains("body") || !form.contains("cuts")) continue;
            const auto current = form.value("current").toString();
            program.forms.insert(form.value("id").toString(), {current,
                formDescription(config, form, "body"), formDescription(config, form, "cuts")});
            for (QChar letter : current)
                if (!program.letters.contains(letter)) program.letters += letter;
        }
    compiledConfig = config;
    return program;
}

static QByteArray bodyDescriptionKey(const QJsonObject& body) {
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

static const LamBodyRecognition* recognizedLamBody(const SubwordBodyRecognition& recognition,
                                                   const QJsonObject& description) {
    const auto found = recognition.lamProfiles.constFind(bodyDescriptionKey(description));
    return found == recognition.lamProfiles.cend() ? nullptr : &found.value();
}

static SubwordBodyRecognition recognizeSubwordBodies(const Word& word,
        const WordResultInfo::SubWordInfo& sub, const QJsonObject& config) {
    SubwordBodyRecognition result;
    if (sub.paths.empty() || sub.paths.front() < 0 || sub.paths.front() >= int(word.paths.size()) ||
        sub.secondaryBodyPath >= int(word.paths.size())) return result;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    const auto& program = bodyCutProgram(config);
    if (std::none_of(letters.begin(), letters.end(), [&](QChar letter) { return program.letters.contains(letter); }))
        return result;
    QMap<QByteArray, QJsonObject> descriptions;
    for (const auto& form : program.forms) {
        // Tooth declarations have their own intrinsic recognizer. They must
        // not be interpreted as stems or trigger the lām fork search.
        if (!lamDescription(form.body)) continue;
        if (std::none_of(letters.begin(), letters.end(), [&](QChar letter) { return form.current.contains(letter); }))
            continue;
        descriptions.insert(bodyDescriptionKey(form.body), form.body);
    }
    if (descriptions.isEmpty()) return result;
    const auto shape = word.bodyShape(sub);
    result.source = shape.transform.map(shape.path).translated(shape.pos);
    WordResultInfo::SubWordInfo geometryOnly{};
    SearchContext context{.transformedPath = result.source, .subword = geometryOnly};
    if (!readTerminalOutline(context)) return result;
    result.orientation = context.orientation;
    result.contours = context.subPaths;
    for (const auto& point : context.timeSorted) result.landmarks.append(point);
    QMap<double, LamBodyEvidence> forkFeatures;
    QMap<QString, std::optional<std::pair<PathExtrema, PathExtrema>>> armFeatures;
    for (auto it = descriptions.begin(); it != descriptions.end(); ++it) {
        const auto& body = it.value();
        LamBodyRecognition features;
        const auto span = body.value("minFlankSpan").toDouble();
        auto forks = forkFeatures.find(span);
        if (forks == forkFeatures.end()) forks = forkFeatures.insert(span, recognizeLamBodies(context, span));
        features.lamBodies = forks.value();
        if (letters.size() >= 2 && body.value("alefLetters").toString().contains(letters.back())) {
            const auto armId = body.value("alefArm").toString();
            auto root = armFeatures.find(armId);
            if (root == armFeatures.end()) {
                const auto arm = namedLetterForm(config, armId);
                Stretching attachment{};
                bool corners = false;
                std::optional<std::pair<PathExtrema, PathExtrema>> seam;
                if (!arm.isEmpty() && foldedArmRoot(context, arm, attachment, &corners) && corners)
                    seam = std::pair{attachment.topBaseLine, attachment.bottomBaseLine};
                root = armFeatures.insert(armId, seam);
            }
            features.alefAttachment = root.value();
        }
        result.lamProfiles.insert(it.key(), std::move(features));
    }
    result.valid = true;
    return result;
}

// Phase 2: assemble the declared lām candidates alongside the other letters.
// Each recognized lām form supplies its body and boundary policies. Forms not
// yet migrated still supply attachments from the existing matcher.
enum class BodyAttachmentPolicy { ForkFlanks, ForkEntry, PreserveCounterEntry, SharedAlefReturn, SeparateAlefReturn };

struct BodyCutRequest {
    int letterIndex;
    BodyAttachmentPolicy attachment;
    QString formId;
    QJsonObject body, cuts;
};

struct SubwordCutPlan {
    WordResultInfo::SubWordInfo partition;
    std::vector<BodyCutRequest> bodies;
};

static SubwordCutPlan makeSubwordCutPlan(WordResultInfo::SubWordInfo proposals, const QJsonObject& config) {
    SubwordCutPlan plan{std::move(proposals), {}};
    const auto& sub = plan.partition;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (sub.letterDiagnostics.size() != letters.size()) return plan;
    const auto& program = bodyCutProgram(config);
    const std::vector<Stretching> joins(sub.joins.begin(), sub.joins.end());
    for (int slot = 0; slot < letters.size(); ++slot) {
        const auto id = sub.letterDiagnostics[slot].toObject().value("template").toString();
        const auto form = program.forms.constFind(id);
        if (form == program.forms.cend()) continue;
        const auto& body = form->body;
        const auto policy = bodyCutPolicy(form->cuts, letters, slot);
        const auto boundaries = policy.value("boundaries").toObject();
        const auto add = [&](BodyAttachmentPolicy attachment) {
            plan.bodies.push_back({slot, attachment, id, body, policy});
        };
        const bool incoming = slot > 0 && slot <= int(joins.size());
        const bool shared = boundaries.value("left") == "shared-return";
        if (shared) {
            const bool separate = sub.letterDiagnostics.last().toObject().value("boundaryKind") == "separate-contours";
            add(separate ? BodyAttachmentPolicy::SeparateAlefReturn : BodyAttachmentPolicy::SharedAlefReturn);
        }
        if (!incoming || boundaries.value("right") != "fork-normal") continue;
        if (boundaries.value("left") == "fork-normal" && slot < int(joins.size())) {
            add(joins[slot - 1].counterEntry.pathIndex > 0 ?
                BodyAttachmentPolicy::PreserveCounterEntry : BodyAttachmentPolicy::ForkFlanks);
        } else if ((shared || boundaries.value("left") == "free") &&
            !policy.value("preserveEntryAfter").toString().contains(letters[slot - 1]) &&
            joins[slot - 1].counterEntry.pathIndex <= 0) {
            add(BodyAttachmentPolicy::ForkEntry);
        }
    }
    return plan;
}

// A tapered terminal tail turns upward into two close corners at the head.
// Following both sides from its free end distinguishes these corners from
// the two much wider lips of the letter above it.
static bool risingTailEntry(SearchContext& ctx, const QJsonObject& spec, Stretching& cut) {
    const auto tip = ctx.timeSorted.find(*ctx.xSorted.rbegin());
    if (tip == ctx.timeSorted.end()) return false;
    const auto corner = [&](bool forward) -> std::optional<PathExtrema> {
        auto it = tip;
        double bottom = tip->point.y();
        for (size_t i = 1; i < ctx.timeSorted.size(); ++i) {
            it = forward ? ctx.next(it) : ctx.prev(it);
            bottom = std::max(bottom, it->point.y());
            if (it->isSharp && it->point.x() - tip->point.x() >= spec.value("minRun").toDouble() &&
                bottom - it->point.y() >= spec.value("minRise").toDouble()) return *it;
        }
        return {};
    };
    const auto a = corner(true), b = corner(false);
    if (!a || !b || QLineF(a->point, b->point).length() > spec.value("maxChord").toDouble() ||
        !interiorChord(ctx.transformedPath, *a, *b)) return false;
    cut.topBaseLine = *a; cut.bottomBaseLine = *b;
    orientSection(ctx, cut, tip->time, true);
    cut.fixedLeftBoundary = cut.fixedRightBoundary = true;
    return true;
}

// Width and centre of a single uninterrupted stroke at the given height.
static std::optional<QLineF> singleHorizontalStroke(const QPainterPath& body, const QRectF& bounds, double y) {
    auto hits = FindInterections(body, QLineF(bounds.left() - 1, y, bounds.right() + 1, y), true);
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.point.x() < b.point.x(); });
    std::optional<QLineF> result;
    for (size_t i = 1; i < hits.size(); ++i) {
        const QLineF span(hits[i - 1].point, hits[i].point);
        if (span.length() < .005 || !body.contains(span.center()))
            continue;
        if (result)
            return {};
        result = span;
    }
    return result;
}
// Measure each attachment from the loop's own counter. A cut is eligible only
// when the resulting body contains this entire counter and the required tail.
struct RimBodyEvidence {
    CounterHole counter;
    std::vector<PathExtrema> anchors;
};

static std::vector<RimBodyEvidence> recognizeRimBodies(const InkOutline& outline) {
    std::vector<RimBodyEvidence> result;
    for (size_t i = 1; i < outline.contours.size(); ++i) {
        const auto box = outline.contours[i].boundingRect();
        if (box.height() < .12 || box.width() < .12 || !outline.contours[0].contains(outline.contours[i])) continue;
        RimBodyEvidence body{{i, box}, {}};
        for (const auto& p : outline.time) if (p.isSharp || p.isX || p.isY) body.anchors.push_back(p);
        result.push_back(std::move(body));
    }
    return result;
}

static std::vector<std::pair<Stretching, Stretching>> cutRimBodies(SearchContext& ctx, const std::vector<RimBodyEvidence>& bodies, const QJsonObject& settings) {
    struct Side {
        double score;
        Stretching cut;
    };
    struct Candidate {
        double score;
        Stretching entry, exit;
    };
    std::vector<Candidate> candidates;
    const auto& ink = ctx.transformedPath;
    for (const auto& evidence : bodies) {
        const auto& hole = ctx.subPaths[evidence.counter.index];
        const auto box = evidence.counter.box;
        const double h = box.height();
        const auto right = settings.value("entry").toArray();
        const auto left = settings.value("exit").toArray();
        const double radius = settings.value("radius").toDouble(1.4) * h;
        const auto bodyFit = [&](const QPainterPath& body) {
            const auto contours = toSubpaths(body);
            if (contours.size() != 2 || contours[1] != hole)
                return false;
            const auto bounds = body.boundingRect();
            const double margin = settings.value("rimMargin").toDouble(1.6) * h;
            if (bounds.right() > box.right() + margin)
                return false;
            if (settings.contains("shaftRise")) {
                // The counter must have its own narrow ascender, continuous from the rim.
                const auto rise = box.top() - bounds.top();
                if (rise < settings.value("shaftRise").toDouble() * h || rise > 8 * h)
                    return false;
                double prev = 0;
                for (double fraction : {.3, .5, .7}) {
                    const auto y = bounds.top() + rise * fraction;
                    auto shaft = singleHorizontalStroke(body, bounds, y);
                    if (!shaft || shaft->length() > 1.4 * h)
                        return false;
                    const double x = shaft->center().x();
                    if (x < box.left() - 1.6 * h || x > box.right() + .5 * h ||
                        (fraction > .3 && std::abs(x - prev) > .65 * h))
                        return false;
                    prev = x;
                }
            } else if (bounds.top() < box.top() - margin)
                return false;
            if (bounds.bottom() > box.bottom() + margin || bounds.left() < box.left() - margin)
                return false;
            return true;
        };
        const auto side = [&](const QJsonArray& anchor, bool entry) {
            std::vector<Side> out;
            if (anchor.size() != 2)
                return out;
            const QPointF aim(box.left() + anchor[0].toDouble() * box.width(), box.top() + anchor[1].toDouble() * h);
            for (const auto& corner : evidence.anchors) {
                if (QLineF(corner.point, aim).length() > radius)
                    continue;
                for (auto cut : cornerSections(ctx, corner)) {
                    const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
                    if (line.length() < .15 || line.length() > 2.4 * h || QLineF(line.center(), aim).length() > radius)
                        continue;
                    for (int flip = 0; flip < 2; ++flip) {
                        if (flip)
                            std::swap(cut.topBaseLine, cut.bottomBaseLine);
                        cut.topRight = cut.topLeft = cut.topBaseLine;
                        cut.bottomRight = cut.bottomLeft = cut.bottomBaseLine;
                        const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
                        if (parts.size() != 2)
                            continue;
                        const auto& body = parts[entry ? 1 : 0].path;
                        const auto outlines = toSubpaths(body);
                        if (outlines.size() < 2 ||
                            std::find(outlines.begin() + 1, outlines.end(), hole) == outlines.end())
                            continue;
                        const double score = (QLineF(aim, line.center()).length() + .35 * line.length()) / h;
                        out.push_back({score, cut});
                        break;
                    }
                }
            }
            std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score < b.score; });
            if (out.size() > 12)
                out.resize(12);
            return out;
        };
        const auto entries = side(right, true);
        const auto exits = side(left, false);
        for (const auto& a : entries)
            for (const auto& b : exits) {
                const auto time = getJoinTime(a.cut, ctx.orientation);
                if (!time.isStrictLeft(b.cut.topBaseLine.time) || !time.isStrictLeft(b.cut.bottomBaseLine.time))
                    continue;
                const auto bodies = cutRegions(ink, {}, {a.cut, b.cut}, ctx.orientation, true);
                if (bodies.size() != 3 || !bodyFit(bodies[1].path))
                    continue;
                candidates.push_back({a.score + b.score, a.cut, b.cut});
            }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.score < b.score; });
    std::vector<std::pair<Stretching, Stretching>> result;
    for (const auto& a : candidates) {
        const bool duplicate = std::any_of(result.begin(), result.end(), [&](const auto& old) {
            const auto close = [](const Stretching& x, const Stretching& y) {
                return std::abs(x.topBaseLine.time - y.topBaseLine.time) < .01 &&
                       std::abs(x.bottomBaseLine.time - y.bottomBaseLine.time) < .01;
            };
            return close(a.entry, old.first) && close(a.exit, old.second);
        });
        if (!duplicate)
            result.emplace_back(a.entry, a.exit);
        if (result.size() == 8)
            break;
    }
    return result;
}

static std::vector<std::pair<Stretching, Stretching>> rimSections(SearchContext& ctx, const QJsonObject& settings) {
    return cutRimBodies(ctx, recognizeRimBodies(inkOutline(ctx)), settings);
}

// Match both sides of an unbranched ascender where it enters a stacked stroke.
static bool descendingStemExit(SearchContext& ctx, const QJsonObject& spec, Stretching& best) {
    if (ctx.timeSorted.empty())
        return false;
    const double pen = spec.value("penWidth").toDouble(.5);
    const double rise = spec.value("minRise").toDouble(4);
    const auto lip = spec.value("headLip").toObject();
    const bool stacked = !lip.isEmpty();
    double lowest = -std::numeric_limits<double>::infinity();
    for (const auto& p : ctx.timeSorted) {
        if ((!p.isSharp && !stacked) || p.point.y() < ctx.bbox.top() + rise)
            continue;
        std::vector<Stretching> lines = cornerSections(ctx, p);
        for (const auto& q : ctx.timeSorted) {
            if ((!q.isSharp && !stacked) || q.time <= p.time || QLineF(p.point, q.point).length() > 2.4 * pen ||
                !interiorChord(ctx.transformedPath, p, q))
                continue;
            Stretching cut{};
            cut.topBaseLine = p;
            cut.bottomBaseLine = q;
            lines.push_back(cut);
        }
        for (auto cut : lines) {
            QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (line.length() < .32 * pen || line.length() > 2.25 * pen)
                continue;
            if (stacked) {
                const auto overhang = ctx.xSorted.begin()->point - line.center();
                if (overhang.x() < lip.value("minRun").toDouble() || overhang.x() > lip.value("maxRun").toDouble() ||
                    overhang.y() < -.5 * pen || overhang.y() > lip.value("maxDrop").toDouble())
                    continue;
            }
            for (int side = 0; side < 2; ++side) {
                if (side)
                    std::swap(cut.topBaseLine, cut.bottomBaseLine);
                cut.topRight = cut.topLeft = cut.topBaseLine;
                cut.bottomLeft = cut.bottomRight = cut.bottomBaseLine;
                auto parts = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
                if (parts.size() != 2)
                    continue;
                const auto& shaft = parts.front().path;
                const auto b = shaft.boundingRect();
                if (toSubpaths(shaft).size() != 1 || b.height() < rise || b.width() > .48 * b.height() ||
                    (!stacked && b.top() > ctx.bbox.top() + 1.4 * pen) || b.bottom() - line.center().y() > pen ||
                    b.bottom() - line.center().y() < -.05)
                    continue;
                bool simple = true;
                std::optional<double> previousX;
                for (double f : {.18, .34, .5, .66, .82, .94}) {
                    // The diamond nib can have two intersections on either flank.
                    // Measure the shaft below the nib in the stacked allograph.
                    if (stacked && f < .34) continue;
                    const auto y = b.top() + f * b.height();
                    auto sample = singleHorizontalStroke(shaft, b, y);
                    if (!sample || sample->length() > (stacked ? 3.2 : 2.2) * pen || sample->length() < .17 * pen ||
                        (previousX && std::abs(sample->center().x() - *previousX) > .22 * b.height())) {
                        simple = false;
                        break;
                    }
                    previousX = sample->center().x();
                }
                if (!simple || !previousX || std::abs(*previousX - line.center().x()) > 1.45 * pen)
                    continue;
                const double score = line.center().y() - .15 * line.length();
                if (score > lowest) {
                    lowest = score;
                    best = cut;
                }
            }
        }
    }
    return std::isfinite(lowest);
}

static std::vector<PathExtrema> recognizeLoopShoulders(const InkOutline& outline, const QRectF& hole,
                                                      const QJsonObject& body) {
    std::vector<PathExtrema> result;
    if (hole.height() <= 0) return result;
    const auto ideal = QPointF(hole.right(), hole.center().y());
    for (const auto& p : outline.time)
        if (p.isSharp && p.point.x() >= hole.right() - .25 * hole.height() &&
            QLineF(ideal, p.point).length() <= body.value("radius").toDouble(2) * hole.height()) result.push_back(p);
    return result;
}

static bool cutLoopShoulder(SearchContext& ctx, const QRectF& hole, const std::vector<PathExtrema>& anchors,
                            const QJsonObject& settings, double witness,
                         bool final, Stretching& best) {
    const double h = hole.height();
    if (h <= 0)
        return false;
    const auto ideal = QPointF(hole.right(), hole.center().y());
    double score = std::numeric_limits<double>::infinity();
    for (const auto& anchor : anchors) {
        const auto* it = &anchor;
        const auto p = anchor.point;
        for (auto cut : cornerSections(ctx, *it)) {
            const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (line.length() > settings.value("maxWidth").toDouble(2.4) * h ||
                std::min(line.x1(), line.x2()) < hole.right() - .45 * h)
                continue;
            orientSection(ctx, cut, witness, true);
            const auto parts = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
            if (parts.size() != 2)
                continue;
            const auto paths = toSubpaths(parts[1].path);
            bool owns = false;
            for (int i = 1; i < paths.size(); ++i)
                if (paths[i].boundingRect() == hole)
                    owns = true;
            if (!owns)
                continue;
            if (final && (paths.size() != 2 || parts[1].path.boundingRect().top() <
                                                   hole.top() - settings.value("maxCapRise").toDouble(2) * h))
                continue;
            const auto fit = QLineF(ideal, p).length() + .35 * line.length();
            if (fit < score) {
                best = cut;
                score = fit;
            }
        }
    }
    return std::isfinite(score);
}
static bool loopShoulder(SearchContext& ctx, const QRectF& hole, const QJsonObject& settings, double witness,
                         bool final, Stretching& best) {
    return cutLoopShoulder(ctx, hole, recognizeLoopShoulders(inkOutline(ctx), hole, settings), settings, witness, final, best);
}
static bool terminalLoopShoulder(SearchContext& ctx, const QJsonObject& rule, Stretching& best) {
    if (ctx.subPaths.size() < 2)
        return false;
    const auto terminal = std::prev(ctx.xSorted.end());
    auto box = ctx.subPaths[1].boundingRect();
    for (size_t i = 2; i < ctx.subPaths.size(); ++i)
        if (ctx.subPaths[i].boundingRect().center().x() < box.center().x())
            box = ctx.subPaths[i].boundingRect();
    return loopShoulder(ctx, box, rule.value("shoulder").toObject(), terminal->time, true, best);
}
// A medial open hah has a pointed re-entrant mouth. Its upper and lower
// lips run left from the same notch and diverge, each ending at a turn.
struct MouthLips {
    PathExtrema notch, upper, lower;
};

static std::vector<MouthLips> mouthLips(SearchContext& ctx, const QJsonObject& form) {
    std::vector<MouthLips> mouths;
    for (auto notch = ctx.timeSorted.begin(); notch != ctx.timeSorted.end(); ++notch) {
        if (!notch->isSharp)
            continue;
        struct Lip {
            PathExtrema turn;
            double rise;
        };
        auto lip = [&](auto advance) -> std::optional<Lip> {
            auto current = notch;
            double rise = 0;
            bool moved = false;
            for (size_t step = 0; step < ctx.timeSorted.size(); ++step) {
                const auto next = (ctx.*advance)(current);
                const auto q = next->point - notch->point;
                if (next->point.x() > current->point.x() + .04)
                    break;
                if (std::abs(q.y()) > std::abs(rise))
                    rise = q.y();
                current = next;
                moved = true;
                // A lower lip reaches the first valley before the rising stem
                // of the next body, even if its contour keeps moving left.
                if (rise > .2 && (ctx.*advance)(current)->point.y() < current->point.y() - .04)
                    break;
            }
            const double run = notch->point.x() - current->point.x();
            if (!moved || run < form.value("minLipRun").toDouble(1.65) || run > form.value("maxLipRun").toDouble(9))
                return {};
            return Lip{*current, rise};
        };
        auto a = lip(&SearchContext::prev), b = lip(&SearchContext::next);
        if (!a || !b)
            continue;
        if (a->rise > b->rise)
            std::swap(a, b);
        if (a->rise > -.22 || b->rise < .28 || b->turn.point.y() <= a->turn.point.y())
            continue;
        mouths.push_back({*notch, a->turn, b->turn});
    }
    return mouths;
}

static std::vector<std::pair<Stretching, Stretching>> mouthSections(SearchContext& ctx, const QJsonObject& form) {
    std::vector<std::pair<Stretching, Stretching>> pairs;
    for (const auto& [notch, upper, lower] : mouthLips(ctx, form)) {
        const double span = notch.point.x() - upper.point.x();
        // The outer nose lies to the right of the notch. Protect it and both
        // lips from being assigned to an adjacent stem or to a connector.
        auto nose = ctx.timeSorted.end();
        double nearest = 1e9;
        for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p) {
            const auto d = p->point - notch.point;
            if (d.x() < .28 * span || d.x() > 1.75 * span || std::abs(d.y()) > 1.4 ||
                (!p->isX &&
                 !(p->isSharp && ctx.next(p)->point.x() <= p->point.x() && ctx.prev(p)->point.x() <= p->point.x())))
                continue;
            const double v = std::abs(d.y()) + .02 * d.x();
            if (v < nearest) {
                nose = p;
                nearest = v;
            }
        }
        if (nose == ctx.timeSorted.end())
            continue;
        std::vector<Stretching> lefts = cornerSections(ctx, lower);
        if (form.value("projectExit").toBool() && notch.point.x() - lower.point.x() > 1.3 * span) {
            Stretching projected{};
            if (medialChordAt(ctx, upper.point.x() - .25, notch.point.y() + 1.5, 3, projected))
                lefts.insert(lefts.begin(), projected);
        }
        std::vector<std::pair<double, Stretching>> rights;
        for (auto corner = ctx.timeSorted.begin(); corner != ctx.timeSorted.end(); ++corner) {
            const auto d = corner->point - notch.point;
            if (!corner->isSharp || d.x() < -span * .65 || d.x() > span * 1.9 || d.y() > 2.25 || d.y() < -3.5)
                continue;
            for (auto cut : cornerSections(ctx, *corner)) {
                orientSection(ctx, cut, notch.time, true);
                const auto s = getJoinTime(cut, ctx.orientation);
                const auto l = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
                if (!s.isStrictLeft(upper.time) || !s.isStrictLeft(lower.time) || !s.isStrictLeft(nose->time) ||
                    l.length() > 2.8 || l.length() < .18)
                    continue;
                rights.emplace_back(l.length() + .18 * std::abs(d.x()) + .1 * std::abs(d.y()), cut);
            }
        }
        std::sort(rights.begin(), rights.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto left : lefts) {
            orientSection(ctx, left, notch.time, false);
            const auto s = getJoinTime(left, ctx.orientation);
            if (!s.isStrictRight(upper.time) || !s.isStrictRight(nose->time) ||
                QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length() > 3)
                continue;
            for (const auto& [score, right] : rights) {
                const auto side = getJoinTime(right, ctx.orientation);
                if (!side.isStrictLeft(left.topBaseLine.time) || !side.isStrictLeft(left.bottomBaseLine.time))
                    continue;
                const auto parts = cutRegions(ctx.transformedPath, {}, {right, left}, ctx.orientation, true);
                if (parts.size() != 3 || toSubpaths(parts[1].path).size() != 1)
                    continue;
                const auto box = parts[1].path.boundingRect();
                if (box.height() > form.value("maxHeight").toDouble(5.5) || box.width() > 3.6 * span)
                    continue;
                pairs.emplace_back(right, left);
            }
        }
    }
    return pairs;
}
// The bent medial kaf has an ascending blade and a diagonal return. On the
// return the incoming stroke forks to the right before its lower turn.
static std::vector<std::pair<Stretching, Stretching>> bladeSections(SearchContext& ctx, const QJsonObject& form) {
    std::vector<std::pair<Stretching, Stretching>> pairs;
    for (auto elbow = ctx.timeSorted.begin(); elbow != ctx.timeSorted.end(); ++elbow) {
        if (ctx.prev(elbow)->point.x() < elbow->point.x() - .035 ||
            ctx.next(elbow)->point.x() < elbow->point.x() - .035)
            continue;
        for (auto ascend : {&SearchContext::prev, &SearchContext::next}) {
            const auto descend = ascend == &SearchContext::prev ? &SearchContext::next : &SearchContext::prev;
            auto tip = elbow;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                auto next = (ctx.*ascend)(tip);
                if (next->point.y() > tip->point.y() + .08 || next->point.x() < tip->point.x() - .12)
                    break;
                tip = next;
            }
            const auto blade = tip->point - elbow->point;
            if (blade.x() < form.value("minReach").toDouble(3.5) || -blade.y() < form.value("minRise").toDouble(3))
                continue;
            // Ignore the short shoulder across the pen nib before following the
            // longer descending branch to its turn.
            auto turn = elbow;
            bool started = false;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                auto next = (ctx.*descend)(turn);
                if (next->point.x() < turn->point.x() - .1 || (started && next->point.y() < turn->point.y() - .15))
                    break;
                if (next->point.y() - elbow->point.y() > 1.8)
                    started = true;
                turn = next;
            }
            const auto arm = turn->point - elbow->point;
            if (arm.x() < 1.3 || arm.y() < 1.8 || arm.y() > 1.6 * (-blade.y()))
                continue;
            for (auto fork = ctx.timeSorted.begin(); fork != ctx.timeSorted.end(); ++fork) {
                if (!fork->isSharp)
                    continue;
                const auto d = fork->point - turn->point;
                if (d.y() >= -.12 || d.y() < -3.4 || std::abs(d.x()) > 2.4)
                    continue;
                const auto a = ctx.prev(fork)->point - fork->point, b = ctx.next(fork)->point - fork->point;
                if (a.y() < -.07 || b.y() < -.07 || std::max(a.x(), b.x()) < .55)
                    continue;
                const bool forward = a.x() > b.x(); // prev walks towards increasing Bezier time
                Stretching local{};
                if (!contourNormalCut(ctx, *fork, forward, false, local))
                    continue;
                const double width = local.referenceWidth;
                if (width <= .08 || width > 3.5)
                    continue;
                auto rightPoint = *fork;
                auto limit = rightPoint;
                limit.time += forward ? int(baseContourCurves(ctx.transformedPath).size())
                                      : -int(baseContourCurves(ctx.transformedPath).size());
                QPointF tangent;
                const double offset = form.value("margin").toDouble(.5);
                if (offset > 0 && !advanceOnContour(ctx.transformedPath, *fork, limit, forward, width * offset,
                                                    rightPoint, tangent))
                    continue;
                for (auto right : cornerSections(ctx, rightPoint)) {
                    orientSection(ctx, right, tip->time, true);
                    const auto entry = getJoinTime(right, ctx.orientation);
                    if (!entry.isStrictLeft(elbow->time) || !entry.isStrictLeft(turn->time) ||
                        QLineF(right.topBaseLine.point, right.bottomBaseLine.point).length() > 2.3 * width)
                        continue;
                    for (auto left : cornerSections(ctx, *turn)) {
                        orientSection(ctx, left, tip->time, false);
                        const auto exit = getJoinTime(left, ctx.orientation);
                        if (!exit.isStrictRight(elbow->time) || !entry.isStrictLeft(left.topBaseLine.time) ||
                            !entry.isStrictLeft(left.bottomBaseLine.time) ||
                            QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length() > 2.8 * width)
                            continue;
                        const auto parts =
                            cutRegions(ctx.transformedPath, {}, {right, left}, ctx.orientation, true);
                        if (parts.size() == 3 && toSubpaths(parts[1].path).size() == 1)
                            pairs.emplace_back(right, left);
                    }
                }
            }
        }
    }
    return pairs;
}

// In the enclosed hah the counter tapers to the right, under the protruding
// nose. The receiving and departing strokes attach below the two lips.
static std::vector<std::pair<Stretching, Stretching>> enclosedHeadSections(SearchContext& ctx,
                                                                           const QJsonObject& form) {
    std::vector<std::pair<Stretching, Stretching>> result;
    for (size_t i = 1; i < ctx.subPaths.size(); ++i) {
        const auto& hole = ctx.subPaths[i];
        const auto box = hole.boundingRect();
        const double h = box.height();
        if (h <= 0 || box.width() < form.value("minAspect").toDouble(1.6) * h || !ctx.subPaths[0].contains(hole))
            continue;
        // The extended tip is part of the upper lip even where it overhangs
        // the incoming stroke. A cut through the middle of the head loses it.
        for (auto nose = ctx.timeSorted.begin(); nose != ctx.timeSorted.end(); ++nose) {
            const auto d = nose->point - QPointF(box.right(), box.center().y());
            if (d.x() < .35 * h || d.x() > 3.7 * h || d.y() < -1.2 * h || d.y() > .75 * h ||
                ctx.prev(nose)->point.x() > nose->point.x() + 1e-7 ||
                ctx.next(nose)->point.x() > nose->point.x() + 1e-7)
                continue;
            std::vector<std::pair<double, Stretching>> sides[2];
            for (auto corner = ctx.timeSorted.begin(); corner != ctx.timeSorted.end(); ++corner) {
                if (!corner->isSharp)
                    continue;
                for (int side : {0, 1}) {
                    const bool entry = side == 0;
                    const double scale = form.value("radius").toDouble(1.7);
                    const double x = corner->point.x(), y = corner->point.y();
                    if (entry ? (x < box.right() - .1 * h || x > nose->point.x() + .6 * h ||
                                 y < box.top() - .5 * h || y > box.bottom() + .6 * h)
                              : (x > box.left() + .3 * h || x < box.left() - scale * h || y < box.top() - .5 * h ||
                                 y > box.bottom() + .8 * h))
                        continue;
                    for (auto cut : cornerSections(ctx, *corner)) {
                        const QLineF line(cut.topBaseLine.point, cut.bottomBaseLine.point);
                        if (line.length() > 1.5 * h)
                            continue;
                        orientSection(ctx, cut, nose->time, entry);
                        const auto pieces = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
                        if (pieces.size() != 2 || !pieces[entry ? 1 : 0]
                                                       .path.boundingRect()
                                                       .adjusted(-1e-7, -1e-7, 1e-7, 1e-7)
                                                       .contains(box))
                            continue;
                        const auto aim =
                            entry ? QPointF(nose->point.x() - .45 * h, box.center().y()) : box.bottomLeft();
                        sides[side].emplace_back(QLineF(corner->point, aim).length() + .35 * line.length(), cut);
                    }
                }
            }
            for (auto& side : sides)
                std::sort(side.begin(), side.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [sr, right] : sides[0])
                for (const auto& [sl, left] : sides[1]) {
                    const auto boundary = getJoinTime(right, ctx.orientation);
                    if (!boundary.isStrictLeft(left.topBaseLine.time) ||
                        !boundary.isStrictLeft(left.bottomBaseLine.time))
                        continue;
                    const auto parts = cutRegions(ctx.transformedPath, {}, {right, left}, ctx.orientation, true);
                    if (parts.size() != 3)
                        continue;
                    const auto paths = toSubpaths(parts[1].path);
                    if (paths.size() != 2 || paths[1].boundingRect() != box)
                        continue;
                    const auto body = paths[0].boundingRect();
                    if (body.height() > 3.6 * h || body.width() > box.width() + 4 * h)
                        continue;
                    result.emplace_back(right, left);
                }
        }
    }
    return result;
}
// Locate the cap of a tall narrow shaft and follow both flanks from the
// cap. A change from descending to lateral motion gives one attachment per
// flank. Both sections must enclose the same cap and keep the shaft intact.
struct TurningShaftBody {
    PathExtrema crest;
    double stroke;
    std::array<std::optional<PathExtrema>, 2> turns;
};

static std::vector<TurningShaftBody> recognizeTurningShafts(const InkOutline& outline, const QJsonObject& body) {
    WordResultInfo::SubWordInfo empty;
    auto ctx = outline.geometry(empty);
    std::vector<TurningShaftBody> result;
    const auto& ink = ctx.transformedPath;
    const auto curves = baseContourCurves(ink);
    const double rise = body.value("minRise").toDouble(3);
    for (auto cap = ctx.timeSorted.begin(); cap != ctx.timeSorted.end(); ++cap) {
        if (ctx.prev(cap)->point.y() < cap->point.y() - 1e-7 || ctx.next(cap)->point.y() < cap->point.y() - 1e-7)
            continue;
        const double y = cap->point.y() + rise * .5;
        const auto hits = FindInterections(ink, QLineF(ctx.bbox.left() - 1, y, ctx.bbox.right() + 1, y), true);
        double width = 0;
        for (size_t j = 1; j < hits.size(); ++j) {
            if (hits[j - 1].pathIndex || hits[j].pathIndex) continue;
            const QLineF l(hits[j - 1].point, hits[j].point);
            if (l.length() > .06 && l.length() < rise * .6 &&
                std::abs(l.center().x() - cap->point.x()) < rise * .25 && ink.contains(l.center())) {
                width = l.length();
                break;
            }
        }
        if (!width) continue;
        TurningShaftBody shaft{*cap, width, {}};
        int k = 0;
        for (bool forward : {true, false}) {
            PathExtrema previous = *cap;
            double run = 0;
            const int sign = forward ? 1 : -1;
            for (int j = 1; j <= int(curves.size() * 16); ++j) {
                double t = std::fmod(cap->time + sign * j / 16. + curves.size(), double(curves.size()));
                auto point = curves[int(t)].pointAt(t - int(t));
                const auto delta = point - previous.point;
                const double fall = point.y() - cap->point.y();
                run += QLineF(point, previous.point).length();
                if (run > rise * 7 || (fall < rise * .65 && run > rise * 1.7)) break;
                if (fall >= rise && (std::abs(delta.x()) >= std::abs(delta.y()) * .85 || delta.y() < 0)) {
                    shaft.turns[k] = PathExtrema{0, t, point, false, false, false};
                    break;
                }
                previous.point = point;
            }
            ++k;
        }
        result.push_back(std::move(shaft));
    }
    return result;
}

static std::vector<std::pair<Stretching, Stretching>> cutTurningShafts(SearchContext& ctx,
        const std::vector<TurningShaftBody>& bodies, const QJsonObject& shape) {
    std::vector<std::pair<Stretching, Stretching>> result;
    const auto& ink = ctx.transformedPath;
    const auto count = static_cast<int>(baseContourCurves(ink).size());
    for (const auto& body : bodies) {
        std::vector<Stretching> flank[2];
        for (int k = 0; k < 2; ++k) {
            if (!body.turns[k]) continue;
            const bool forward = k == 0;
            const auto start = *body.turns[k];
            auto end = start;
            end.time += (forward ? 1 : -1) * count;
            for (double offset : {.5, 0.}) {
                PathExtrema at = start;
                QPointF tangent;
                if (offset && !advanceOnContour(ink, start, end, forward, body.stroke * offset, at, tangent)) continue;
                for (auto section : cornerSections(ctx, at)) {
                    const QLineF l(section.topBaseLine.point, section.bottomBaseLine.point);
                    if (l.length() >= body.stroke * .4 && l.length() <= body.stroke * 3.2) flank[k].push_back(section);
                }
            }
        }
        for (int swap : {0, 1}) for (auto right : flank[swap]) for (auto left : flank[1 - swap]) {
            orientSection(ctx, right, body.crest.time, true);
            orientSection(ctx, left, body.crest.time, false);
            const auto side = getJoinTime(right, ctx.orientation);
            if (!side.isStrictLeft(left.topBaseLine.time) || !side.isStrictLeft(left.bottomBaseLine.time)) continue;
            const auto pieces = cutRegions(ink, {}, {right, left}, ctx.orientation, true);
            if (pieces.size() != 3) continue;
            const auto& mask = pieces[1].path;
            const auto box = mask.boundingRect();
            if (toSubpaths(mask).size() != 1 ||
                box.width() > box.height() * shape.value("maxWidthToHeight").toDouble(.8) ||
                std::abs(box.top() - body.crest.point.y()) > 1e-6) continue;
            result.emplace_back(right, left);
        }
    }
    return result;
}

// Adapter for other forms still using the existing shaft description.
static std::vector<std::pair<Stretching, Stretching>> shaftSections(SearchContext& ctx, const QJsonObject& form) {
    return cutTurningShafts(ctx, recognizeTurningShafts(inkOutline(ctx), form), form);
}

// The free arm of a folded lam-alef is broad at the nib and tapers down
// and right into the returning bowl. The two inward corners delimit the
// contact when both are present; a shorter cross-section can clip its tip.
static bool foldedArmRoot(SearchContext& ctx, const QJsonObject& form, Stretching& result, bool* cornerPair) {
    if (cornerPair) *cornerPair = false;
    const auto& ink = ctx.transformedPath;
    const double minRise = form.value("minRise").toDouble(3.);
    const double maxNeck = form.value("maxNeck").toDouble(1.6);
    const auto curves = baseContourCurves(ink);
    if (curves.empty()) return false;
    std::set<double> attachmentCorners;
    for (const auto& point : ctx.timeSorted) {
        if (!point.isSharp || std::abs(point.time - std::round(point.time)) > 1e-8) continue;
        const int index = int(std::round(point.time)) % int(curves.size());
        const auto& before = curves[(index + curves.size() - 1) % curves.size()];
        const auto& after = curves[index];
        auto back = before.pt3() - before.pt4(), forward = after.pt2() - after.pt1();
        if (std::hypot(back.x(), back.y()) < 1e-9) back = before.pointAt(.9999) - before.pt4();
        if (std::hypot(forward.x(), forward.y()) < 1e-9) forward = after.pointAt(.0001) - after.pt1();
        const double turn = forward.x() * back.y() - forward.y() * back.x();
        if (std::abs(turn) > 1e-10 && (turn > 0) == (ctx.orientation == Orientation::CounterClockwise))
            attachmentCorners.insert(point.time);
    }
    std::pair<int, double> best{1, std::numeric_limits<double>::infinity()};
    for (auto it = ctx.timeSorted.begin(); it != ctx.timeSorted.end(); ++it) {
        if (!it->isSharp || it->point.x() > ctx.bbox.left() + 1.35 * ctx.bbox.height())
            continue;
        if (it->point.y() - ctx.bbox.top() < minRise)
            continue;
        auto sections = cornerSections(ctx, *it);
        for (const auto& other : ctx.timeSorted) {
            if (!other.isSharp || other.time <= it->time || QLineF(other.point, it->point).length() > maxNeck ||
                !interiorChord(ink, *it, other))
                continue;
            Stretching cut{};
            cut.topBaseLine = *it;
            cut.bottomBaseLine = other;
            sections.push_back(cut);
        }
        for (auto cut : sections) {
            const QLineF neck(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (neck.length() > maxNeck)
                continue;
            for (int orientation = 0; orientation < 2; ++orientation) {
                std::swap(cut.topBaseLine, cut.bottomBaseLine);
                const auto pieces = cutRegions(ink, {}, {cut}, ctx.orientation, true);
                if (pieces.size() != 2)
                    continue;
                const auto& arm = pieces[1].path;
                const auto& rest = pieces[0].path;
                const auto box = arm.boundingRect();
                const auto root = neck.center();
                if (toSubpaths(arm).size() != 1 || box.height() < minRise || box.width() < .38 * box.height() ||
                    box.width() > 1.45 * box.height() || box.right() - root.x() > .65 ||
                    box.bottom() - root.y() > .65 || box.left() > ctx.bbox.left() + .35 * box.height() ||
                    rest.boundingRect().top() > box.top() + .25 * box.height() ||
                    rest.boundingRect().bottom() < root.y() + .18 * box.height())
                    continue;
                std::array<QLineF, 3> cross;
                bool valid = true;
                for (int j = 0; j < 3; ++j) {
                    const auto y = box.top() + box.height() * (.22 + .32 * j);
                    auto hits = FindInterections(arm, QLineF(box.left() - 1, y, box.right() + 1, y), true);
                    if (hits.size() != 2) {
                        valid = false;
                        break;
                    }
                    cross[j] = QLineF(hits.front().point, hits.back().point);
                }
                if (!valid || cross[0].center().x() + .15 * box.width() > cross[1].center().x() ||
                    cross[1].center().x() + .15 * box.width() > cross[2].center().x() ||
                    cross[0].length() < 1.2 * cross[2].length() || cross[2].length() > .3 * box.width())
                    continue;
                // Prefer the actual junction only after validating the complete
                // tapered arm above. Smooth roots retain the cross-section search.
                const bool junction = attachmentCorners.count(cut.topBaseLine.time) &&
                    attachmentCorners.count(cut.bottomBaseLine.time);
                const auto score = std::pair{junction ? 0 : 1,
                    neck.length() + 1.5 * (box.bottom() - root.y()) +
                    1.5 * (box.right() - root.x()) - .12 * box.height()};
                if (score >= best)
                    continue;
                best = score;
                if (cornerPair) *cornerPair = junction;
                result = cut;
                result.topLeft = result.topRight = result.topBaseLine;
                result.bottomLeft = result.bottomRight = result.bottomBaseLine;
                result.referenceWidth = neck.length();
                result.fixedRightBoundary = true;
                result.kashidaOverrides.insert("enabled", false);
            }
        }
    }
    return std::isfinite(best.second);
}

// The incoming branch leaves a notch between the upstroke and the bowl.
// A horizontal slice above that notch must cross only the slim upright.
static bool terminalStemFork(SearchContext &ctx, const QJsonObject &form, Stretching &best) {
    const auto &ink = ctx.transformedPath;
    const auto tail = std::prev(ctx.xSorted.end());
    const double pen = form.value("penWidth").toDouble(.5);
    const double inset = form.value("walkWidths").toDouble(.5) * pen;
    const int count = int(baseContourCurves(ink).size());
    double score = std::numeric_limits<double>::infinity();
    for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p) {
        if (p->point.x() < tail->point.x() + 2 * pen)
            continue;
        const auto flank = [&](auto advance) {
            auto step = p;
            do {
                step = (ctx.*advance)(step);
            } while (step != p && QLineF(p->point, step->point).length() < .25 * pen);
            return step->point - p->point;
        };
        const auto a = flank(&SearchContext::prev), b = flank(&SearchContext::next);
        if (std::min(a.y(), b.y()) < -.15 * pen || std::max(a.x(), b.x()) < pen)
            continue;
        const bool forward = a.x() > b.x();
        PathExtrema limit = *p, anchor{};
        limit.time += (forward ? 1 : -1) * count;
        QPointF tangent;
        std::vector<Stretching> proposals;
        Stretching normal{};
        if (advanceOnContour(ink, *p, limit, forward, inset, anchor, tangent) && anchor.point.x() > p->point.x() &&
            contourNormalCut(ctx, anchor, forward, false, normal, true))
            proposals.push_back(normal);
        // A head touching the upright creates a second notch on the other
        // edge of the attachment. Both notches stay on the shared cut.
        for (const auto &q : ctx.timeSorted) {
            const auto d = q.point - p->point;
            if (d.y() > -.4 * pen || d.y() < -4.5 * pen || d.x() < -.4 * pen || d.x() > 3.5 * pen ||
                !interiorChord(ink, q, *p))
                continue;
            Stretching bridge{};
            bridge.topBaseLine = q;
            bridge.bottomBaseLine = *p;
            proposals.push_back(bridge);
        }
        for (auto cut : proposals) {
            orientSection(ctx, cut, tail->time, true);
            const QLineF chord(cut.topBaseLine.point, cut.bottomBaseLine.point);
            if (chord.length() > 5 * pen || QLineF(chord.center(), p->point).length() > 4 * pen)
                continue;
            const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
            if (parts.size() != 2 || toSubpaths(parts[1].path).size() != 1)
                continue;
            const auto &body = parts[1].path;
            const auto box = body.boundingRect();
            const double rise = p->point.y() - box.top();
            if (rise < form.value("minRise").toDouble(3) || box.bottom() - p->point.y() < pen ||
                std::abs(box.left() - tail->point.x()) > pen || box.right() > chord.center().x() + 3 * pen)
                continue;
            bool slender = true;
            for (double fraction : {.3, .65}) {
                const double y = box.top() + fraction * rise;
                const auto hits = FindInterections(body, QLineF(box.left() - 1, y, box.right() + 1, y));
                if (hits.size() != 2) {
                    slender = false;
                    break;
                }
                const QLineF band(hits.front().point, hits.back().point);
                if (band.length() > .4 * rise || std::abs(band.center().x() - p->point.x()) > .5 * rise) {
                    slender = false;
                    break;
                }
            }
            if (!slender)
                continue;
            const double fit = chord.length() + .45 * QLineF(chord.center(), p->point).length();
            if (fit < score) {
                score = fit;
                best = cut;
            }
        }
    }
    return std::isfinite(score);
}

// Initial/final lām declarations are evaluated here, before the legacy
// dispatcher. A failed declaration is a miss, never a legacy matcher fallback.
static bool declaredLamBoundary(SearchContext& context, const QJsonObject& config, const QJsonObject& form) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto body = formDescription(config, form, "body");
    const auto cuts = formDescription(config, form, "cuts");
    const auto shape = body.value("shape").toString();
    bool found = false;
    if (shape == "contour-stem")
        found = lamRouteCut(context, body.value("route").toObject(), cuts.value("route").toObject());
    else if (shape == "descending-stem")
        found = descendingStemExit(context, body.value("stem").toObject(), context.res);
    else if (shape == "terminal-bowl") {
        const auto evidence = recognizeTerminalStemBodies(context, body.value("stem").toObject());
        if (inspectingForm) for (const auto& item : evidence) {
            inspectLandmark("Stem crest", *item.crest);
            inspectLandmark("Stem foot", *item.limit);
            inspectLandmark("Bowl edge", *item.bowlEdge);
        }
        if (evidence.empty()) inspectReason("Body", "No terminal shaft passed the crest, rise and local width constraints.");
        found = cutTerminalStemBodies(context, evidence, body.value("stem").toObject(), cuts.value("section").toObject());
        if (!found && !evidence.empty()) inspectReason("Cuts", "Terminal shaft recognized; no entry section retained the bowl and stem.");
    } else if (shape == "terminal-fork")
        found = terminalStemFork(context, withOverrides(body.value("stem").toObject(), cuts.value("section").toObject()), context.res);
    else if (shape == "terminal-return")
        found = terminalTailEntry(context, withOverrides(body.value("tail").toObject(), cuts.value("section").toObject()), context.res);
    if (found) inspectCandidate("Boundary proposal", context.res);
    else if (shape != "contour-stem" && shape != "terminal-bowl")
        inspectReason("Body / cuts", "No candidate passed this shape's combined attachment checks.");
    return found;
}

// One intrinsic stem occurrence, including both contour routes even when a
// neighbor will later make one of them unusable. No boundary is an input.
struct LamStemBody {
    PathExtrema crest;
    std::array<std::optional<PathExtrema>, 2> descendingFlanks;
    std::array<std::optional<SavedContourRoute>, 2> entryRoutes;
    std::vector<std::array<std::optional<SavedContourRoute>, 2>> exitRoutes;
};

struct LamMedialBodyEvidence {
    QPainterPath source;
    LamBodyEvidence features;
    std::vector<LamStemBody> stems;
    std::vector<TurningShaftBody> turningShafts;
};

static LamMedialBodyEvidence recognizeMedialLamBodies(const InkOutline& outline, const QJsonObject& body) {
    LamMedialBodyEvidence result;
    result.source = outline.ink;
    if (body.value("shape") == "turning-shaft") {
        result.turningShafts = recognizeTurningShafts(outline, body.value("shaft").toObject());
        return result;
    }
    if (body.value("shape") != "forked-stem") return result;
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    result.features = recognizeLamBodies(geometry, body.value("minFlankSpan").toDouble());
    const auto routes = body.value("exitRoutes").toArray();
    for (const auto& crest : result.features.crests) {
        LamStemBody stem{crest, {}, {}, {}};
        const auto at = geometry.timeSorted.find(crest);
        int flank = 0;
        for (const auto advance : {&SearchContext::next, &SearchContext::prev}) {
            auto limit = at;
            for (size_t step = 0; step < geometry.timeSorted.size(); ++step) {
                const auto next = (geometry.*advance)(limit);
                if (next->point.y() <= limit->point.y()) break;
                limit = next;
            }
            if (limit != at) stem.descendingFlanks[flank] = *limit;
            ++flank;
        }
        if (body.value("entryRoute").isObject())
            for (int reverse = 0; reverse < 2; ++reverse)
                stem.entryRoutes[reverse] = recognizeSavedRoute(geometry, body.value("entryRoute").toObject(), &crest, reverse);
        for (const auto& route : routes) {
            std::array<std::optional<SavedContourRoute>, 2> alternatives;
            for (int reverse = 0; reverse < 2; ++reverse)
                alternatives[reverse] = recognizeSavedRoute(geometry, route.toObject(), &crest, reverse);
            stem.exitRoutes.push_back(std::move(alternatives));
        }
        result.stems.push_back(std::move(stem));
    }
    return result;
}

static std::shared_ptr<const LamMedialBodyEvidence> recognizedMedialLamBodies(
        SearchContext& context, const QJsonObject& body) {
    const auto generate = [&] {
        return std::make_shared<const LamMedialBodyEvidence>(recognizeMedialLamBodies(inkOutline(context), body));
    };
    if (!context.outlineCache) return generate();
    const auto key = std::make_pair(bodyDescriptionKey(body), static_cast<int>(context.orientation));
    auto& cached = context.outlineCache->medialLamBodies[key];
    if (!cached || cached->source != context.transformedPath) cached = generate();
    return cached;
}

static void inspectLamBodyEvidence(const LamMedialBodyEvidence& evidence) {
    if (!inspectingForm) return;
    for (const auto& crest : evidence.features.crests) inspectLandmark("Stem crest", crest);
    for (const auto& fork : evidence.features.forks) inspectLandmark("Fork candidate", fork.tip);
    for (const auto& stem : evidence.stems) {
        for (const auto& foot : stem.descendingFlanks) if (foot) inspectLandmark("Flank foot", *foot);
        const auto route = [](const auto& r) {
            if (r) inspectLandmark("Route anchor", r->anchor);
        };
        for (const auto& r : stem.entryRoutes) route(r);
        for (const auto& alternatives : stem.exitRoutes) for (const auto& r : alternatives) route(r);
    }
    for (const auto& shaft : evidence.turningShafts) {
        inspectLandmark("Shaft crest", shaft.crest);
        for (const auto& turn : shaft.turns) if (turn) inspectLandmark("Flank turn", *turn);
    }
}

struct MedialCandidate {
    Stretching right;
    Stretching left;
    bool sharedJoins = false;
    std::optional<std::pair<Stretching, Stretching>> hollowCuts;
    std::optional<PathExtrema> lamCrest;
    std::optional<PathExtrema> lamFork;
};

// Assembly owns all neighbor compatibility, contextual scale, priority and
// shared-attachment checks. Its input inventory is already recognized and
// remains unchanged when incoming/outgoing joins or cut policies change.
static std::vector<MedialCandidate> assembleMedialLamCuts(SearchContext& context,
        const QJsonObject& config, const QJsonObject& form, const LamMedialBodyEvidence& evidence,
        const Stretching& incoming,
        const QPointF& entry, const QPointF& exit, double stroke, bool open, bool recoverTeeth,
        bool allowFork, const std::optional<Stretching>& sharedExit) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto lamBody = formDescription(config, form, "body");
    const auto lam = formDescription(config, form, "cuts");
    std::vector<MedialCandidate> candidates;
    if (lamBody.value("shape") == "turning-shaft") {
        for (const auto& [right, left] : cutTurningShafts(context, evidence.turningShafts, lamBody.value("shaft").toObject()))
            candidates.emplace_back(right, left);
        if (inspectingForm) for (const auto& item : candidates) inspectCandidate("Turning shaft", item.left, item.right);
        if (candidates.empty()) inspectReason("Body / cuts", "No turning shaft passed the rise, width and section checks.");
        return candidates;
    }
    if (lamBody.value("shape") != "forked-stem") return candidates;
    const auto placement = lam.value("placement").toObject();
    const auto before = [&](const Stretching& a, const Stretching& b) {
        return cutPrecedes(a, b, context.orientation);
    };
    const auto center = [](const Stretching& cut) {
        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).center();
    };
    const auto shortLamExit = medialLamShortExit(config);
    const double minExitRun = shortLamExit.value("minHorizontalRun").toDouble();
    const double maxExitEntryGap = shortLamExit.value("maxEntryGap").toDouble();
    const double minExitLengthRatio = shortLamExit.value("minLengthRatio").toDouble();
    const double exitAnchorTolerance = shortLamExit.value("anchorTolerance").toDouble();
    const bool cutsAroundHollow = shortLamExit.value("cutsAroundHollow").toBool();
    const double maxEntrySlope = std::tan(qDegreesToRadians(shortLamExit.value("entryMaxTilt").toDouble(12)));
    const auto originalForm = context.form;
    const auto originalResult = context.res;
    const auto originalNext = context.nextIter;
    const auto routes = lamBody.value("exitRoutes").toArray();
    const auto routeCuts = lam.value("exitRoutes").toArray();
    const auto& bodyEvidence = evidence.features;
    // This is a placement gate, not the body recognizer: all stem candidates
    // above exist even when this join rejects their position or height.
    const auto fitsSlot = [&](const LamStemBody& stem) {
        const auto& crest = stem.crest;
        const bool fits = crest.point.x() < entry.x() &&
            (open ? entry.y() : std::min(entry.y(), exit.y())) - crest.point.y() >=
                lamBody.value("minRise").toDouble() * stroke &&
            (sharedExit || getJoinTime(incoming, context.orientation).isStrictLeft(crest.time));
        if (!fits) inspectReason("Assembly", "Recognized stem is outside this slot's position, height or contour-order limits.");
        return fits;
    };
    double firstStemX = -std::numeric_limits<double>::infinity();
    for (const auto& stem : evidence.stems)
        if (fitsSlot(stem)) firstStemX = std::max(firstStemX, stem.crest.point.x());
    std::vector<MedialCandidate> forkCandidates;
    const double forkOffset = lam.value("forkOffset").toDouble();
    if (allowFork && forkOffset > 0 && !sharedExit && incoming.counterEntry.pathIndex <= 0) {
        const auto entrySide = getJoinTime(incoming, context.orientation);
        const auto& recognizedBodies = bodyEvidence.forks;
        for (const auto& stem : evidence.stems) {
            const auto* crest = &stem.crest;
            if (!fitsSlot(stem) || crest->point.x() < firstStemX - 1e-6) continue;
            for (const auto& body : recognizedBodies) {
                if (!body.isConcave(context.orientation) || (!body.fork && !body.folded())) continue;
                const auto* tip = &body.tip;
                if (tip->point.y() - crest->point.y() < lamBody.value("minRise").toDouble() * stroke ||
                    std::abs(tip->point.x() - crest->point.x()) > 3 * stroke ||
                    std::abs(tip->point.y() - entry.y()) > 4 * stroke ||
                    !entrySide.isStrictLeft(tip->time)) continue;
                const bool rightForward = body.rightForward;
                // Small extrema can subdivide one flank. Walk through
                // them and let the body and join checks bound the result.
                for (double distance : {forkOffset, 0.}) {
                    const auto right = cutLamForkFlank(context, *tip,
                        std::nullopt, rightForward, 1, distance);
                    const auto left = cutLamForkFlank(context, *tip, std::nullopt, !rightForward, -1, distance);
                    if (!right || !left || !before(incoming, *right) || !before(*right, *left) ||
                        std::min(right->referenceWidth, left->referenceWidth) <= 1e-6 ||
                        (distance == 0 && std::max(right->referenceWidth, left->referenceWidth) > 2.5 * stroke)) continue;
                    const auto parts = cutRegions(context.transformedPath, {}, {*right, *left}, context.orientation, true);
                    if (parts.size() != 3) continue;
                    const auto& body = parts[1].path;
                    const auto bounds = body.boundingRect();
                    if (std::abs(bounds.top() - crest->point.y()) > .01 ||
                        bounds.width() > lamBody.value("maxWidthToHeight").toDouble() * bounds.height() ||
                        (form.contains("bodyCounters") &&
                         static_cast<int>(toSubpaths(body).size()) - 1 != form.value("bodyCounters").toInt())) continue;
                    forkCandidates.push_back({*right, *left, false, {}, *crest, *tip});
                }
            }
        }
    }
    for (qsizetype routeIndex = 0; routeIndex < routes.size(); ++routeIndex) {
        const auto route = routes[routeIndex];
        const auto routePolicy = routeCuts[routeIndex].toObject();
        const size_t routeStart = candidates.size();

        for (const auto& stem : evidence.stems) {
            const auto* crest = &stem.crest;
            // Reading-order selection belongs to assembly. Recognition keeps
            // the following stem available for its own later letter slot.
            if (!fitsSlot(stem) || (!sharedExit && crest->point.x() < firstStemX - 1e-6)) continue;
            std::vector<Stretching> rightOptions;
            if (incoming.counterEntry.pathIndex > 0) rightOptions.push_back(incoming);
            for (int flank = 0; flank < 2; ++flank) {
                if (!stem.descendingFlanks[flank]) continue;
                Stretching right{};
                const int traversal = flank == 1 ? 1 : -1;
                if (alefTurnCut(context, *stem.descendingFlanks[flank], placement.value("turnAngle").toDouble(),
                                stroke, right, crest, traversal)) rightOptions.push_back(right);
            }
            // A bowl-shaped foot can turn after the stem's 45-degree
            // normal. Its optional route stays on this stem's flank.
            const auto turnCount = rightOptions.size();
            if (lamBody.value("entryRoute").isObject()) {

                for (bool reverse : {false, true}) {
                    context.res = {};
                    if (projectSavedRoute(context, stem.entryRoutes[reverse], lamBody.value("entryRoute").toObject(), lam.value("entryRoute").toObject())) rightOptions.push_back(context.res);
                }

            }
            for (auto right : rightOptions) {
                const auto incomingChord = QLineF(incoming.topBaseLine.point, incoming.bottomBaseLine.point);
                const auto rightChord = QLineF(right.topBaseLine.point, right.bottomBaseLine.point);
                bool shared = right.counterEntry.pathIndex > 0 ||
                    incomingChord.intersects(rightChord, nullptr) == QLineF::BoundedIntersection;
                if (recoverTeeth && !shared && !before(incoming,right) &&
                    getJoinTime(incoming,context.orientation).isStrictLeft(crest->time) &&
                    QLineF(entry,center(right)).length() <= placement.value("entryOverlap").toDouble()*stroke) {
                    right = incoming;
                    shared = true;
                }
                if ((!shared && !before(incoming, right)) ||
                    (shared && QLineF(entry, center(right)).length() > stroke)) continue;
                bool accepted = false;
                for (const bool reverse : {false, true}) {
                    context.res = {};
                    // In a lam-alef, the outgoing boundary is intrinsic to
                    // the ligature. Containment below rejects the diagonal
                    // alef crest and keeps the upright lam and its return.
                    std::optional<ForkRefinement> fork;
                    if (sharedExit) context.res = *sharedExit;
                    else if (!projectSavedRoute(context, stem.exitRoutes[routeIndex][reverse], route.toObject(), routePolicy, &fork)) continue;
                    // Moving only the exit onto its outgoing branch must
                    // not make a previously crossing entrance eligible.
                    if (fork && !before(right, fork->originalExit)) continue;
                    if (!before(right, context.res)) continue;
                    const auto pieces = cutRegions(context.transformedPath, {}, {right, context.res}, context.orientation, true);
                    const auto sample = crest->point + QPointF(0, .01 * stroke);
                    if (pieces.size()!=3) continue;
                    if (!pieces[1].path.contains(sample)) {
                        if (!recoverTeeth) continue;
                        // An inclined pen cap can leave the point directly
                        // below its crest outside the ink. Retain the
                        // center of the narrow shaft immediately below it.
                        const double y = crest->point.y() + .65 * stroke;
                        auto section = FindInterections(context.transformedPath,
                            QLineF(crest->point.x()-2*stroke,y,crest->point.x()+2*stroke,y), true);
                        std::sort(section.begin(),section.end(),[](const auto& a,const auto& b) {
                            return a.point.x()<b.point.x();
                        });
                        bool retainsShaft = false;
                        for (size_t j=1;j<section.size();++j) {
                            const QLineF cross(section[j-1].point,section[j].point);
                            if (cross.length()>2*stroke || !interiorChord(context.transformedPath,section[j-1],section[j])) continue;
                            if (pieces[1].path.contains(cross.center()) &&
                                pieces[1].path.boundingRect().adjusted(-.01,-.01,.01,.01).contains(crest->point)) retainsShaft=true;
                        }
                        if (!retainsShaft) continue;
                    }
                    // Apply body ownership before accepting a route;
                    // otherwise a route through the next loop can
                    // suppress a valid later route for this stem.
                    if (form.contains("bodyCounters") &&
                        static_cast<int>(toSubpaths(pieces[1].path).size()) - 1 !=
                            form.value("bodyCounters").toInt()) continue;
                    const auto bodyBounds = pieces[1].path.boundingRect();
                    if (!sharedExit && lamBody.contains("maxWidthToHeight") && bodyBounds.width() >
                        lamBody.value("maxWidthToHeight").toDouble() * bodyBounds.height()) continue;
                    auto bodyEntry = right;
                    if (fork && !shared && placement.value("levelForkEntry").toBool()) {
                        const auto near = levelForkEntry(context, fork->corner, context.res);
                        if (near && before(right, *near) && before(*near, context.res)) {
                            const auto local = cutRegions(context.transformedPath, {}, {*near, context.res},
                                                          context.orientation, true);
                            if (local.size() == 3 && local[1].path.contains(sample)) bodyEntry = *near;
                        }
                    }
                    std::optional<std::pair<Stretching, Stretching>> hollowCuts;
                    if (!sharedExit && !shortLamExit.isEmpty()) {
                        const auto top = context.res.topBaseLine;
                        const auto bottom = context.res.bottomBaseLine;
                        const double entryBottomX = bodyEntry.bottomBaseLine.point.x();
                        // On a folded lam foot a normal can cross the whole bend
                        // and land near the entry. Keep the same exit landmark
                        // and use the short section of ink directly beneath it.
                        if (bottom.point.x() - top.point.x() > minExitRun * stroke &&
                            entryBottomX - bottom.point.x() <= maxExitEntryGap * stroke) {
                            auto local = context.res;
                            if (medialChordAt(context, top.point.x(), top.point.y(), stroke,
                                              local, false, top.point.y() - exitAnchorTolerance) &&
                                std::abs(local.topBaseLine.point.y() - top.point.y()) < exitAnchorTolerance &&
                                QLineF(top.point, bottom.point).length() >
                                    minExitLengthRatio * QLineF(local.topBaseLine.point, local.bottomBaseLine.point).length() &&
                                before(bodyEntry, local)) {
                                local.topBaseLine = top;
                                local.topLeft = local.topRight = top;
                                local.bottomLeft = local.bottomRight = local.bottomBaseLine;
                                local.referenceWidth = QLineF(top.point, local.bottomBaseLine.point).length();
                                local.length = 0;
                                const auto entryChord = bodyEntry.bottomBaseLine.point - bodyEntry.topBaseLine.point;
                                if (!shared && cutsAroundHollow &&
                                    std::abs(entryChord.x()) <= maxEntrySlope * std::abs(entryChord.y())) {
                                    // A turn at the hollow can cross the original long exit.
                                    // Recheck it against the shortened exit of the same foot.
                                    for (size_t turn = 0; turn < turnCount; ++turn) {
                                        const auto& hollow = rightOptions[turn];
                                        if (!before(bodyEntry, hollow) || !before(hollow, local)) continue;
                                        const auto axis = center(hollow);
                                        const double width = QLineF(hollow.topBaseLine.point, hollow.bottomBaseLine.point).length();
                                        if (width <= 0) continue;
                                        auto hollowRight = bodyEntry, hollowLeft = local;
                                        const auto sectionAt = [&](Stretching& boundary, double x, double y, bool normal) {
                                            if (!medialChordAt(context, x, y, width, boundary) ||
                                                (normal && !strokeNormalCut(context, boundary))) return false;
                                            boundary.referenceWidth = QLineF(boundary.topBaseLine.point, boundary.bottomBaseLine.point).length();
                                            return boundary.referenceWidth <= 2 * width;
                                        };
                                        // The existing cuts bound the space available on each side.
                                        const double rightX = axis.x() + width, leftX = axis.x() - width;
                                        if (rightX < center(bodyEntry).x() &&
                                            (!sectionAt(hollowRight, rightX, center(bodyEntry).y(), true) ||
                                             !before(bodyEntry, hollowRight) || !before(hollowRight, hollow)))
                                            continue;
                                        if (leftX > center(local).x() &&
                                            (!sectionAt(hollowLeft, leftX, center(local).y(), false) ||
                                             !before(hollow, hollowLeft) || !before(hollowLeft, local)))
                                            continue;
                                        const auto hollowPieces = cutRegions(context.transformedPath, {}, {hollowRight, hollowLeft},
                                                                             context.orientation, true);
                                        if (hollowPieces.size() != 3 || !hollowPieces[1].path.contains(sample) ||
                                            toSubpaths(hollowPieces[1].path).size() != toSubpaths(pieces[1].path).size())
                                            continue;
                                        hollowCuts = std::pair{hollowRight, hollowLeft};
                                        break;
                                    }
                                }
                                context.res = local;
                            }
                        }
                    }
                    candidates.push_back({bodyEntry, context.res, shared, hollowCuts, *crest}); accepted = true;
                    if (sharedExit) break;
                }
                if (accepted) break;
            }
        }
        // Preserve route priority, but let the assembler try the
        // next route when this one conflicts with the final body.
        std::stable_sort(candidates.begin() + routeStart, candidates.end(), [](const auto& a, const auto& b) {
            return Stretching::cmpbyx{}(a.right, b.right);
        });
    }
    // Rank occurrences before route alternatives. Otherwise an
    // early route through the following stem can hide a later
    // valid route around this lam and put its shaft in a join.
    // A folded ligature already fixes the outgoing seam.
    // Preserve its established route priority rather than
    // moving the incoming seam to a different flank.
    if (!sharedExit)
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return Stretching::cmpbyx{}(a.right, b.right);
        });
    candidates.insert(candidates.begin(), forkCandidates.begin(), forkCandidates.end());
    if (inspectingForm) for (const auto& item : candidates) inspectCandidate(item.lamFork ? "Fork flanks" : "Stem route", item.left, item.right);
    if (candidates.empty()) inspectReason("Assembly", "No recognized shaft produced compatible cuts after rise, proportions, ordering and attachment checks.");
    context.form = originalForm;
    context.res = originalResult;
    context.nextIter = originalNext;
    return candidates;
}

// At a tight loop-to-stroke junction, both edges of the bridge can end at
// corners. Join those corners while retaining the counter and the complete tail.
struct LoopBridgeBody {
    QRectF hole;
    PathExtrema tip;
    std::vector<std::pair<PathExtrema, PathExtrema>> pairs;
};

static std::optional<LoopBridgeBody> recognizeLoopBridge(const InkOutline& outline) {
    if (outline.contours.size() < 2) return {};
    auto hole = outline.contours[1].boundingRect();
    for (size_t i = 2; i < outline.contours.size(); ++i)
        if (outline.contours[i].boundingRect().center().x() < hole.center().x()) hole = outline.contours[i].boundingRect();
    const auto tip = *std::prev(outline.x.end());
    const double h = hole.height();
    if (h <= 0 || hole.left() - tip.point.x() < h) return {};
    LoopBridgeBody result{hole, tip, {}};
    const QPointF target(hole.right(), hole.bottom());
    for (const auto& p : outline.time) {
        if (!p.isSharp || p.point.x() < hole.right() - .15 * h || QLineF(p.point, target).length() > 2 * h) continue;
        for (const auto& q : outline.time) {
            const auto delta = q.point - p.point;
            if (q.isSharp && delta.y() >= .3 * h && std::abs(delta.x()) <= delta.y() &&
                q.point.x() >= hole.right() - .15 * h && interiorChord(outline.ink, p, q))
                result.pairs.emplace_back(p, q);
        }
    }
    return result;
}

static bool cutLoopBridge(SearchContext& ctx, const LoopBridgeBody& body, const QJsonObject& settings, Stretching& best) {
    const auto* tip = &body.tip;
    const auto& hole = body.hole;
    const auto &ink = ctx.transformedPath;
    const double h = hole.height();
    if (h <= 0 || hole.left() - tip->point.x() < h)
        return false;
    const QPointF target(hole.right(), hole.bottom());
    double bestFit = std::numeric_limits<double>::infinity();
    for (const auto& pair : body.pairs) {
        const auto* p = &pair.first;
        const auto* q = &pair.second;
        const QLineF edge(p->point, q->point);
        if (edge.length() > settings.value("maxWidth").toDouble(3) * h) continue;
            Stretching cut{};
            cut.topBaseLine = *p;
            cut.bottomBaseLine = *q;
            orientSection(ctx, cut, tip->time, true);
            const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
            if (parts.size() != 2)
                continue;
            const auto outlines = toSubpaths(parts[1].path);
            const auto box = parts[1].path.boundingRect();
            if (outlines.size() != 2 || outlines[1].boundingRect() != hole ||
                box.right() > edge.center().x() + .7 * h || box.bottom() - hole.bottom() < 1.2 * h ||
                box.top() < hole.top() - 2.5 * h)
                continue;
            const double fit = QLineF(edge.center(), target).length() + .2 * edge.length();
            if (fit < bestFit) {
                bestFit = fit;
                best = cut;
            }
    }
    return std::isfinite(bestFit);
}

static bool terminalLoopBridge(SearchContext& ctx, const QJsonObject& settings, Stretching& best) {
    const auto body = recognizeLoopBridge(inkOutline(ctx));
    return body && cutLoopBridge(ctx, *body, settings, best);
}

struct CounterOutlineBody {
    std::vector<CounterHole> counters;
    QRectF eye;
};

static std::vector<CounterOutlineBody> recognizeCounterOutlines(const InkOutline& outline, const QJsonObject& body) {
    std::vector<CounterOutlineBody> result;
    const auto holes = recognizeCounterHoles(outline);
    const int count = body.value("counters").toInt();
    const auto aspect = body.value("outline").toObject().value("aspect").toArray();
    if (aspect.size() != 2) return result;
    const auto add = [&](std::vector<CounterHole> group) {
        QRectF eye;
        for (const auto& hole : group) {
            if (!outline.contours[0].contains(outline.contours[hole.index])) return;
            eye = eye.united(hole.box);
        }
        if (eye.height() <= .1 || eye.width() <= .1) return;
        const double ratio = eye.width() / eye.height();
        if (ratio >= aspect[0].toDouble() && ratio <= aspect[1].toDouble()) result.push_back({std::move(group), eye});
    };
    for (size_t i = 0; i < holes.size(); ++i) {
        if (count == 1) add({holes[i]});
        else if (count == 2) for (size_t j = i + 1; j < holes.size(); ++j) add({holes[i], holes[j]});
    }
    return result;
}

struct SharedShapeEvidence {
    QPainterPath source;
    std::vector<CounterHole> holes;
    std::optional<InitialCounterBody> initialNeck;
    std::optional<CounterHole> terminalCounter, terminalContour;
    CounterCornerEvidence terminalNeck;
    std::vector<PathExtrema> shoulders;
    std::optional<LoopBridgeBody> bridge;
    std::vector<LoopBodyEvidence> loops;
    std::vector<RimBodyEvidence> rims;
    std::optional<SavedContourRoute> route;
    std::optional<LoopHeadBody> head;
    std::vector<CrestedLoopBody> crestedHeads;
    std::optional<BowlTurnBody> bowlTurn;
    std::vector<CounterOutlineBody> outlines;
};

static SharedShapeEvidence recognizeSharedShape(const InkOutline& outline, const QJsonObject& body) {
    SharedShapeEvidence result;
    result.source = outline.ink;
    const auto shape = body.value("shape");
    if (shape == "contour-route") {
        WordResultInfo::SubWordInfo empty;
        auto geometry = outline.geometry(empty);
        const auto route = recognizeContourRoute(geometry, body.value("route").toObject());
        if (route) result.route = SavedContourRoute{*route->start, *route->origin, route->anchor, route->topBefore, route->topEnd,
            route->forward == &SearchContext::prev, route->topDirection == &SearchContext::prev, route->endpointTolerance};
    } else if (shape == "suspended-loop") result.head = recognizeLoopHead(outline, body.value("head").toObject());
    else if (shape == "crested-loop") result.crestedHeads = recognizeCrestedLoops(outline, body.value("head").toObject());
    else if (shape == "bowl-turn") result.bowlTurn = recognizeBowlTurn(outline);
    else if (shape == "counter-loop") result.loops = recognizeLoopBodies(outline, body.value("loop").toObject());
    else if (shape == "counter-rim") result.rims = recognizeRimBodies(outline);
    else if (shape == "counter-outline") result.outlines = recognizeCounterOutlines(outline, body);
    else if (shape == "counter-neck") {
        const auto spec = body.value("counter").toObject();
        result.holes = recognizeCounterHoles(outline);
        result.initialNeck = recognizeInitialCounterBody(outline, spec);
        for (const auto& hole : result.holes)
            if (!result.terminalCounter || hole.box.center().x() < result.terminalCounter->box.center().x()) result.terminalCounter = hole;
        if (result.terminalCounter)
            result.terminalNeck = recognizeCounterCorners(outline, result.terminalCounter->box, spec.value("entryCorner").toObject());
    } else if (shape == "counter-shoulder") {
        for (size_t i = 1; i < outline.contours.size(); ++i) {
            CounterHole hole{i, outline.contours[i].boundingRect()};
            if (!result.terminalContour || hole.box.center().x() < result.terminalContour->box.center().x()) result.terminalContour = hole;
        }
        if (result.terminalContour)
            result.shoulders = recognizeLoopShoulders(outline, result.terminalContour->box, body.value("shoulder").toObject());
    } else if (shape == "counter-bridge") result.bridge = recognizeLoopBridge(outline);
    return result;
}

static std::shared_ptr<const SharedShapeEvidence> recognizedSharedShape(SearchContext& context, const QJsonObject& body) {
    const auto generate = [&] { return std::make_shared<const SharedShapeEvidence>(recognizeSharedShape(inkOutline(context), body)); };
    if (!context.outlineCache) return generate();
    const auto key = std::make_pair(bodyDescriptionKey(body), static_cast<int>(context.orientation));
    auto& evidence = context.outlineCache->sharedBodies[key];
    if (!evidence || evidence->source != context.transformedPath) evidence = generate();
    return evidence;
}

static void inspectSharedShape(const SharedShapeEvidence& evidence) {
    if (!inspectingForm) return;
    const auto corner = [&](const auto& c) { for (const auto& p : c.corners) inspectLandmark("Neck corner", p.anchor); };
    if (evidence.route) {
        inspectLandmark("Route origin", evidence.route->origin);
        inspectLandmark("Route anchor", evidence.route->anchor);
    }
    if (evidence.initialNeck) {
        for (const auto& p : evidence.initialNeck->valleys) inspectLandmark("Loop valley", p);
        for (const auto& p : evidence.initialNeck->normalAnchors) inspectLandmark("Loop corner", p);
    }
    if (evidence.head) {
        if (evidence.head->edge) inspectLandmark("Head edge", *evidence.head->edge);
        for (const auto& neck : evidence.head->necks) inspectLandmark("Head crest", neck.crest);
    }
    for (const auto& head : evidence.crestedHeads) {
        inspectLandmark("Head crest", head.crest);
        inspectLandmark("Right edge", head.rightEdge); inspectLandmark("Left edge", head.leftEdge);
    }
    if (evidence.bowlTurn) inspectLandmark("Bowl turn", evidence.bowlTurn->turn);
    corner(evidence.terminalNeck);
    for (const auto& loop : evidence.loops) {
        corner(loop.entry);
        for (const auto& p : loop.exit.corners) {
            auto anchor = p.anchor; anchor.point.setX(-anchor.point.x()); inspectLandmark("Exit corner", anchor);
        }
    }
    for (const auto& rim : evidence.rims) for (const auto& p : rim.anchors) inspectLandmark("Rim landmark", p);
    for (const auto& p : evidence.shoulders) inspectLandmark("Shoulder", p);
    if (evidence.bridge) for (const auto& pair : evidence.bridge->pairs) {
        inspectLandmark("Bridge corner", pair.first); inspectLandmark("Bridge corner", pair.second);
    }
}

static bool declaredSharedBoundary(SearchContext& context, const QJsonObject& form) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto evidence = recognizedSharedShape(context, body);
    const auto shape = body.value("shape");
    bool found = false;
    if (shape == "contour-route")
        found = projectSavedRoute(context, evidence->route, body.value("route").toObject(), cuts.value("route").toObject());
    else if (shape == "suspended-loop")
        found = evidence->head && cutLoopHead(context, *evidence->head, body.value("head").toObject(), cuts.value("head").toObject());
    else if (shape == "bowl-turn")
        found = evidence->bowlTurn && cutBowlTurn(context, *evidence->bowlTurn, cuts.value("probe").toObject());
    else if (shape == "counter-neck") {
        const auto settings = shapeSettings(form, "counter");
        if (cuts.value("boundaries").toObject().value("right") == "free")
            found = evidence->initialNeck && cutInitialCounterBody(context, *evidence->initialNeck, settings);
        else found = evidence->terminalCounter && cutFinalCounterBody(context, evidence->terminalCounter->box,
            evidence->terminalNeck, settings);
    } else if (shape == "counter-shoulder")
        found = evidence->terminalContour && cutLoopShoulder(context, evidence->terminalContour->box, evidence->shoulders,
            shapeSettings(form, "shoulder"), std::prev(context.xSorted.end())->time, true, context.res);
    else if (shape == "counter-bridge")
        found = evidence->bridge && cutLoopBridge(context, *evidence->bridge, cuts.value("bridge").toObject(), context.res);
    else if (shape == "counter-loop") {
        const auto candidate = selectLoopBody(context, evidence->loops, context.bbox.left(), context.bbox.right(), nullptr);
        const auto pair = candidate ? cutLoopBody(context, *candidate, shapeSettings(form, "loop"), false) : std::nullopt;
        found = pair.has_value();
        if (found) { context.res = pair->second; context.res.fixedRightBoundary = true; }
    }
    if (found) inspectCandidate("Boundary proposal", context.res);
    else inspectReason("Cuts", "Recognized body features did not produce a section retaining this body.");
    return found;
}

static bool terminalFork(SearchContext& ctx, const QJsonObject& form, Stretching& result) {
    const auto& ink = ctx.transformedPath;
    const auto tail = std::prev(ctx.xSorted.end());
    double best = std::numeric_limits<double>::max();
    for (auto p = ctx.timeSorted.begin(); p != ctx.timeSorted.end(); ++p) {
        if (!p->isSharp || p->point.x() <= tail->point.x() + 1.5)
            continue;
        for (auto q = ctx.timeSorted.begin(); q != ctx.timeSorted.end(); ++q) {
            const auto d = q->point - p->point;
            if (!q->isSharp || d.y() < .15 || d.y() > 2 || std::abs(d.x()) > 1.6 * d.y() ||
                !interiorChord(ink, *p, *q))
                continue;
            const double width = std::hypot(d.x(), d.y());
            for (bool forward : {true, false}) {
                auto next = (forward ? ctx.prev(q) : ctx.next(q));
                const auto drift = next->point - q->point;
                if (drift.x() <= 0 || drift.y() <= 0)
                    continue;
                auto limit = *q;
                limit.time += (forward ? 1 : -1) * int(baseContourCurves(ink).size());
                PathExtrema entry{};
                QPointF tangent;
                if (!advanceOnContour(ink, *q, limit, forward, form.value("walkWidths").toDouble(.5) * width, entry,
                                      tangent))
                    continue;
                for (auto cut : cornerSections(ctx, entry)) {
                    orientSection(ctx, cut, tail->time, true);
                    const auto side = getJoinTime(cut, ctx.orientation);
                    if (!side.isStrictLeft(p->time) || !side.isStrictLeft(q->time))
                        continue;
                    const auto parts = cutRegions(ink, {}, {cut}, ctx.orientation, true);
                    if (parts.size() != 2 || toSubpaths(parts[1].path).size() != 1)
                        continue;
                    auto box = parts[1].path.boundingRect();
                    if (p->point.y() - box.top() < form.value("minRise").toDouble(3) ||
                        box.right() > q->point.x() + 2.6 * width)
                        continue;
                    bool cap = false;
                    for (const auto& peak : ctx.timeSorted)
                        if (side.isStrictLeft(peak.time) && peak.point.y() < p->point.y() - 3 &&
                            std::abs(peak.point.x() - q->point.x()) < 3 * width)
                            cap = true;
                    const double score =
                        QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length() + .2 * width;
                    if (cap && score < best) {
                        best = score;
                        result = cut;
                    }
                }
            }
        }
    }
    return std::isfinite(best) && best < std::numeric_limits<double>::max();
}

static std::vector<std::pair<Stretching, Stretching>> parallelKafSections(SearchContext& ctx,
                                                                          const QJsonObject& settings) {
    using Band = QLineF;
    const auto& ink = ctx.transformedPath;
    auto bandsAt = [&](double x) {
        const auto hits = FindInterections(ink, QLineF(x, ctx.bbox.top() - 1, x, ctx.bbox.bottom() + 1), true);
        std::vector<Band> bands;
        for (size_t i = 1; i < hits.size(); ++i) {
            const Band b(hits[i - 1].point, hits[i].point);
            if (!hits[i - 1].pathIndex && !hits[i].pathIndex && b.length() >= .18 && b.length() <= 2.8 &&
                ink.contains(b.center()))
                bands.push_back(b);
        }
        return bands;
    };
    std::vector<std::pair<Stretching, Stretching>> pairs;
    for (auto elbow = ctx.timeSorted.begin(); elbow != ctx.timeSorted.end(); ++elbow) {
        if (ctx.prev(elbow)->point.x() < elbow->point.x() - .02 ||
            ctx.next(elbow)->point.x() < elbow->point.x() - .02)
            continue;
        for (auto across : {&SearchContext::prev, &SearchContext::next}) {
            const auto up = across == &SearchContext::next ? &SearchContext::prev : &SearchContext::next;
            auto edge = elbow, tip = elbow;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                const auto next = (ctx.*across)(edge);
                if (next->point.x() < edge->point.x() - .04 || std::abs(next->point.y() - elbow->point.y()) > 2.8)
                    break;
                edge = next;
            }
            const auto run = edge->point.x() - elbow->point.x();
            if (run < settings.value("minRun").toDouble(4))
                continue;
            for (size_t i = 0; i < ctx.timeSorted.size(); ++i) {
                const auto next = (ctx.*up)(tip);
                if (next->point.x() < tip->point.x() - .16 || next->point.y() > tip->point.y() + .12)
                    break;
                tip = next;
            }
            if (tip->point.x() - elbow->point.x() < 2 || elbow->point.y() - tip->point.y() < 1.8)
                continue;
            auto parallelBands = [&](double x) -> std::optional<std::pair<Band, Band>> {
                auto bands = bandsAt(x);
                for (size_t i = 1; i < bands.size(); ++i) {
                    const Band a = bands[i - 1], b = bands[i];
                    if (std::abs(a.center().y() - elbow->point.y()) > 2.6 || b.center().y() <= a.center().y() ||
                        b.center().y() - a.center().y() > 3.4 * std::max(a.length(), b.length()) ||
                        std::max(a.length(), b.length()) > 2.3 * std::min(a.length(), b.length()))
                        continue;
                    return std::pair(a, b);
                }
                return {};
            };
            const auto a = parallelBands(elbow->point.x() + .32 * run),
                       b = parallelBands(elbow->point.x() + .72 * run);
            if (!a || !b || std::abs(a->second.center().y() - b->second.center().y()) > 2.6)
                continue;
            const double stroke = (a->second.length() + b->second.length()) / 2;
            const double slope = (b->second.center().y() - a->second.center().y()) / (.4 * run);
            auto baseline = [&](double x) { return a->second.center().y() + slope * (x - a->second.center().x()); };
            const double leftX = elbow->point.x() - settings.value("margin").toDouble(.5) * stroke;
            Stretching left{};
            if (!medialChordAt(ctx, leftX, baseline(leftX), 2.8 * stroke, left))
                continue;
            orientSection(ctx, left, tip->time, false);
            std::vector<Stretching> rights;
            // The tight fold can lean over its incoming stroke. Its lower
            // inward corner gives a more precise reference than the cap's projection.
            for (auto corner = ctx.timeSorted.begin(); corner != ctx.timeSorted.end(); ++corner) {
                if (!corner->isSharp || std::abs(corner->point.x() - edge->point.x()) > 1.7 * stroke ||
                    std::abs(corner->point.y() - baseline(edge->point.x())) > 2.1 * stroke)
                    continue;
                const double x = corner->point.x() + settings.value("margin").toDouble(.5) * stroke;
                Stretching cut{};
                if (medialChordAt(ctx, x, corner->point.y() + .5 * stroke, 1.8 * stroke, cut))
                    rights.push_back(cut);
            }
            const double x = edge->point.x() + .5 * stroke;
            Stretching projected{};
            if (medialChordAt(ctx, x, baseline(x) + .5 * stroke, 2.8 * stroke, projected))
                rights.push_back(projected);
            for (auto right : rights) {
                orientSection(ctx, right, tip->time, true);
                const auto side = getJoinTime(right, ctx.orientation);
                if (!side.isStrictLeft(left.topBaseLine.time) || !side.isStrictLeft(left.bottomBaseLine.time))
                    continue;
                const auto parts = cutRegions(ink, {}, {right, left}, ctx.orientation, true);
                if (parts.size() != 3 || toSubpaths(parts[1].path).size() != 1)
                    continue;
                const auto& body = parts[1].path;
                if (!body.contains(a->first.center()) || !body.contains(a->second.center()) ||
                    !body.contains(b->first.center()) || !body.contains(b->second.center()) ||
                    body.boundingRect().height() > elbow->point.y() - tip->point.y() + 6 * stroke)
                    continue;
                pairs.emplace_back(right, left);
            }
        }
    }
    return pairs;
}
static bool initialEnclosedHead(SearchContext& ctx, const QJsonObject& settings, Stretching& result) {
    const auto nose = ctx.xSorted.begin();
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < ctx.subPaths.size(); ++i) {
        const auto& hole = ctx.subPaths[i];
        const auto box = hole.boundingRect();
        const double h = box.height();
        if (h <= 0 || box.width() < settings.value("minAspect").toDouble(1.25) * h ||
            nose->point.x() < box.right() + .3 * h || nose->point.x() > box.right() + 4 * h ||
            nose->point.y() < box.top() - h || nose->point.y() > box.bottom() + h ||
            !ctx.subPaths[0].contains(hole))
            continue;
        for (auto corner = ctx.timeSorted.begin(); corner != ctx.timeSorted.end(); ++corner) {
            const auto p = corner->point;
            if (!corner->isSharp || p.x() > box.left() + .35 * h || QLineF(p, box.bottomLeft()).length() > 2 * h)
                continue;
            for (auto cut : cornerSections(ctx, *corner)) {
                orientSection(ctx, cut, nose->time, false);
                const auto parts = cutRegions(ctx.transformedPath, {}, {cut}, ctx.orientation, true);
                if (parts.size() != 2)
                    continue;
                const auto paths = toSubpaths(parts[0].path);
                if (paths.size() != 2 || paths[1].boundingRect() != box)
                    continue;
                if (paths[0].boundingRect().height() > 3.4 * h)
                    continue;
                const double score = QLineF(p, box.bottomLeft()).length() +
                                     .35 * QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
                if (score < best) {
                    best = score;
                    result = cut;
                }
            }
        }
    }
    return std::isfinite(best);
}

struct ToothSearchPlan {
    bool enumerate = false;
    int stage = 1;
    QMap<QString, std::vector<ToothBody>> teeth;
    QMap<QString, std::vector<std::pair<Stretching,Stretching>>> intrinsic;
    QVector<int> skip, choices;
    QVector<std::set<std::array<int, 4>>> encountered;
    explicit ToothSearchPlan(int size) : skip(size), choices(size), encountered(size) {}
    bool accepts(int letter, const Stretching& entry, const Stretching& exit) {
        const auto key = [](double time) { return static_cast<int>(std::lround(time * 100)); };
        if (!encountered[letter]
                 .insert({key(entry.topBaseLine.time), key(entry.bottomBaseLine.time), key(exit.topBaseLine.time),
                          key(exit.bottomBaseLine.time)})
                 .second)
            return false;
        return choices[letter]++ == skip[letter];
    }
    bool advance(int failed) {
        for (int i = std::min(failed, static_cast<int>(skip.size()) - 2); i >= 0; --i) {
            if (choices[i] <= skip[i]) continue;
            ++skip[i];
            std::fill(skip.begin() + i + 1, skip.end(), 0);
            choices.fill(0);
            for (auto& seen : encountered)
                seen.clear();
            return true;
        }
        return false;
    }
};

// The recovery search can find several ordered chords on a folded shape.
// Check their actual masks, including counters and connecting ink, before
// accepting the replacement for a partial result.
static bool coversOriginalInk(const Word& word, const WordResultInfo::SubWordInfo& sub) {
    if (sub.paths.empty())
        return false;
    const auto shape = word.bodyShape(sub);
    auto world = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path);
        p.translate(shape.pos);
        return p;
    };
    auto ink = world(shape.path);
    ink.setFillRule(Qt::WindingFill);
    const auto box = ink.boundingRect();
    constexpr double scale = 12;
    const QSize size(int(std::ceil(box.width() * scale)) + 4, int(std::ceil(box.height() * scale)) + 4);
    if (size.isEmpty())
        return false;
    const QTransform transform(scale, 0, 0, scale, 2 - box.left() * scale, 2 - box.top() * scale);
    const auto inverse = transform.inverted();
    auto render = [&](const QPainterPath& path) {
        QImage image(size, QImage::Format_Grayscale8);
        image.fill(0);
        QPainter painter(&image);
        painter.setTransform(transform);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPath(path);
        painter.end();
        return image;
    };
    const auto source = render(ink);
    std::vector<QPainterPath> shapes;
    for (const auto& body : sub.baseGlyphs)
        shapes.push_back(world(body.path));
    for (const auto& join : sub.kashidaPaths)
        if (!join.isEmpty())
            shapes.push_back(world(join));
    std::vector<QImage> images;
    for (const auto& path : shapes)
        images.push_back(render(path));
    QPainterPathStroker stroke;
    stroke.setWidth(.002);
    stroke.setCurveThreshold(1e-5);
    const auto boundary = stroke.createStroke(ink);
    std::vector<const uchar*> scan(images.size());
    for (int y = 0; y < size.height(); ++y) {
        for (size_t i = 0; i < images.size(); ++i)
            scan[i] = images[i].constScanLine(y);
        const auto line = source.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            int owners = 0;
            for (auto row : scan)
                owners += row[x] != 0;
            if (owners == int(line[x] != 0))
                continue;
            const auto probe = inverse.map(QPointF(x + .5, y + .5));
            int actual = 0;
            for (const auto& path : shapes)
                actual += path.contains(probe);
            if (actual != int(ink.contains(probe)) && !(actual <= 1 && boundary.contains(probe)))
                return false;
        }
    }
    return true;
}

// Compare the two sides by arc length. A short bent connector can lie well
// outside its straight endpoint chord while both sides still follow the same
// thin stroke. A tooth or head produces a long unmatched excursion instead.
static bool followsOneStroke(const QPainterPath& ribbon, const QLineF& right, const QLineF& left) {
    auto curves = baseContourCurves(ribbon);
    if (curves.size() < 3)
        return false;
    const auto first = curves.front().pt1(), last = curves.back().pt4();
    if (QLineF(last, first).length() > 1e-6) {
        const QLineF closing(last, first);
        curves.push_back(QBezier::fromPoints(last, closing.pointAt(1. / 3), closing.pointAt(2. / 3), first));
    }
    const int n = int(curves.size());
    std::array<int, 4> role{-1, -1, -1, -1};
    const std::array<QPointF, 4> end{right.p1(), right.p2(), left.p1(), left.p2()};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < n; ++i)
            if (QLineF(end[k], curves[i].pt1()).length() < .002) {
                role[k] = i;
                break;
            }
    if (std::any_of(role.begin(), role.end(), [](int r) { return r < 0; }))
        return false;
    std::array<std::vector<QPointF>, 2> points;
    std::array<std::vector<double>, 2> distances;
    for (int k = 0; k < 2; ++k) {
        int at = role[k];
        const int forward = (at + 1) % n == role[1 - k] ? -1 : 1;
        auto& p = points[k];
        auto& d = distances[k];
        p.push_back(curves[at].pt1());
        d.push_back(0);
        for (int i = 0; i < n; ++i) {
            const auto& c = curves[(at + (forward < 0 ? n - 1 : 0)) % n];
            for (int j = 1; j <= 12; ++j) {
                const auto next = c.pointAt(forward > 0 ? j / 12. : 1 - j / 12.);
                d.push_back(d.back() + QLineF(p.back(), next).length());
                p.push_back(next);
            }
            at = (at + forward + n) % n;
            if (at == role[2] || at == role[3])
                break;
            if (at == role[1 - k])
                return false;
        }
        if (d.back() <= 1e-5)
            return false;
    }
    const double width = std::max(right.length(), left.length());
    const double shortSide = std::min(distances[0].back(), distances[1].back());
    const double longSide = std::max(distances[0].back(), distances[1].back());
    if (longSide - shortSide > 3.5 * width + .18 * shortSide ||
        (shortSide + longSide) / 2 > 2.2 * QLineF(right.center(), left.center()).length() + 2.8 * width)
        return false;
    auto sample = [&](int side, double t) {
        const auto& ds = distances[side];
        const auto& ps = points[side];
        const auto target = t * ds.back();
        const auto i =
            std::clamp(int(std::upper_bound(ds.begin(), ds.end(), target) - ds.begin()), 1, int(ds.size()) - 1);
        return QLineF(ps[i - 1], ps[i]).pointAt((target - ds[i - 1]) / std::max(ds[i] - ds[i - 1], 1e-9));
    };
    for (double f : {.15, .3, .5, .7, .85})
        if (QLineF(sample(0, f), sample(1, f)).length() > 2.7 * width)
            return false;
    return true;
}

// An open or enclosed medial hah has two lips. Across part of the head,
// a page-vertical section must meet both with a visible gap between them.
static bool containsHeadLips(const QPainterPath& body, const QJsonObject& lips, QLineF* evidence) {
    const auto box = body.boundingRect();
    const double run = lips.value("minRun").toDouble(), gap = lips.value("minGap").toDouble();
    if (run <= 0 || gap <= 0 || box.width() < run || box.height() <= gap) return false;
    const int samples = std::max(1, int(std::ceil(box.width() / (.25 * run))));
    const double step = box.width() / samples;
    int consecutive = 0, longest = 0;
    std::vector<QLineF> sections;
    if (evidence) sections.resize(samples);
    for (int i = 0; i < samples; ++i) {
        const double x = box.left() + (i + .5) * step;
        auto hits = FindInterections(body, QLineF(x, box.top() - 1, x, box.bottom() + 1), true);
        std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.point.y() < b.point.y(); });
        bool divided = false, seenInk = false;
        double lastBottom = 0, lastMiddle = 0;
        for (size_t j = 1; j < hits.size(); ++j) {
            double top = hits[j - 1].point.y(), bottom = hits[j].point.y();
            if (bottom - top < .01 || !body.contains(QPointF(x, (top + bottom) / 2)))
                continue;
            if (seenInk && top - lastBottom >= gap) {
                divided = true;
                if (evidence) sections[i] = QLineF(x, lastMiddle, x, (top + bottom) / 2);
                break;
            }
            lastBottom = bottom;
            lastMiddle = (top + bottom) / 2;
            seenInk = true;
        }
        consecutive = divided ? consecutive + 1 : 0;
        if (consecutive * step >= run) {
            if (!evidence) return true;
            if (consecutive > longest) {
                longest = consecutive;
                *evidence = sections[i - consecutive / 2];
            }
        }
    }
    return longest > 0;
}

// Check the delivered partition. A normal connecting stroke can be very long.
// Only a fork with two ink bands separated by an opening is evidence of a head.
static std::map<int, QLineF> missingHeadInk(const Word& word, const WordResultInfo::SubWordInfo& sub,
                                            const QString& letters, const QJsonObject& settings) {
    std::map<int, QLineF> missing;
    if (settings.isEmpty() || int(sub.baseGlyphs.size()) != letters.size() ||
        sub.kashidaPaths.size() + 1 != sub.baseGlyphs.size() || sub.kashidaCuts.size() != sub.kashidaPaths.size())
        return missing;
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& p) { return shape.transform.map(p).translated(shape.pos); };
    const auto opening = settings.value("_initialHeadLips").toObject();
    if (!opening.isEmpty() && !containsHeadLips(mapped(sub.baseGlyphs.front().path), opening))
        missing.emplace(0, QLineF{});
    const auto protectedLetters = settings.value("_connectorHeadLips").toObject();
    const int last = int(letters.size()) - 1;
    for (int letter = 1; !protectedLetters.isEmpty() && letter < last; ++letter) {
        const auto lips = protectedLetters.value(QString(letters[letter])).toObject();
        if (lips.isEmpty()) continue;
        for (const int join : {letter - 1, letter}) {
            if (sub.kashidaPaths[join].isEmpty()) continue;
            const auto ribbon = mapped(sub.kashidaPaths[join]);
            const auto& [right, left] = sub.kashidaCuts[join];
            QLineF witness;
            if (followsOneStroke(ribbon, right, left) || !containsHeadLips(ribbon, lips, &witness)) continue;
            // A folded upright can be the arm of the adjacent bowl. If the
            // medial head already has both lips, keep those source points with
            // it while the adjoining form reclaims its upright.
            const auto bounds = ribbon.boundingRect();
            if (bounds.height() > bounds.width()) {
                QLineF existing;
                if (containsHeadLips(mapped(sub.baseGlyphs[letter].path), lips, &existing)) witness = existing;
            }
            // Between two eligible letters the fork alone cannot identify its owner.
            const int neighbor = join == letter ? letter + 1 : letter - 1;
            if (neighbor != 0 && neighbor != last && protectedLetters.contains(QString(letters[neighbor])))
                witness = {};
            missing.emplace(letter, witness);
            break;
        }
    }
    return missing;
}

// A shared medial cut may take an ascender away from a valid initial proposal.
// Check the delivered body against the exposed sides and cap of the original pen stroke.
static bool retainsInitialShaft(const QPainterPath& body, const QPainterPath& source, const QJsonObject& spec) {
    const auto bounds = body.boundingRect();
    if (bounds.height() < spec.value("minHeight").toDouble() ||
        bounds.width() > spec.value("maxWidthToHeight").toDouble() * bounds.height()) return false;
    const double tol = spec.value("edgeTolerance").toDouble();
    int fullSections = 0;
    for (double fraction : {.12, .32, .55}) {
        const double y = bounds.top() + fraction * bounds.height();
        const auto span = singleHorizontalStroke(body, bounds, y);
        if (!span || span->length() < 2*tol) continue;
        if (fraction == .12 && source.contains(QPointF(span->center().x(), bounds.top()-2*tol))) return false;
        if (!source.contains(QPointF(span->x1()-tol,y)) && !source.contains(QPointF(span->x2()+tol,y)))
            ++fullSections;
    }
    return fullSections >= 2;
}

static bool plausibleRecoveredBodies(const Word& word, const WordResultInfo::SubWordInfo& sub,
                                     const QJsonObject& settings) {
    const auto shape = word.bodyShape(sub);
    auto map = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path);
        p.translate(shape.pos);
        return p;
    };
    for (size_t i = 0; i < sub.kashidaPaths.size(); ++i) {
        if (sub.kashidaPaths[i].isEmpty())
            continue;
        const auto ribbon = map(sub.kashidaPaths[i]);
        const auto outlines = toSubpaths(ribbon);
        if (outlines.size() != 1)
            return false;
        const auto& [entry, exit] = sub.kashidaCuts[i];
        const auto start = entry.center(), end = exit.center();
        const double width = std::max(entry.length(), exit.length()), length = QLineF(start, end).length();
        if (width <= 1e-6 || length <= 1e-6)
            return false;
        const auto along = (end - start) / length;
        const QPointF across(-along.y(), along.x());
        const double margin = settings.value("connectorMargin").toDouble(.9) * width;
        const double bend = settings.value("connectorBend").toDouble(.25) * length + width;
        bool withinEnvelope = true;
        for (const auto& poly : ribbon.toSubpathPolygons(QTransform::fromScale(8, 8)))
            for (auto p : poly) {
                const auto d = p / 8. - start;
                const double projection = QPointF::dotProduct(d, along);
                if (projection < -margin || projection > length + margin ||
                    std::abs(QPointF::dotProduct(d, across)) > bend)
                    withinEnvelope = false;
            }
        const bool longJoin = length >= settings.value("longConnectorWidths").toDouble(4) * width;
        if ((longJoin || !withinEnvelope) && !followsOneStroke(ribbon, entry, exit))
            return false;
    }
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.isEmpty() || sub.baseGlyphs.empty())
        return false;
    const auto shaft = settings.value("initialShaft").toObject();
    if (shaft.value("letters").toString().contains(letters.front())) {
        auto source = map(shape.path);
        source.setFillRule(Qt::WindingFill);
        if (!retainsInitialShaft(map(sub.baseGlyphs.front().path), source, shaft)) return false;
    }
    const auto sizes = settings.value("bodyExtents").toArray();
    for (int i = 0; i < static_cast<int>(sub.baseGlyphs.size()); ++i) {
        const auto body = map(sub.baseGlyphs[i].path);
        const auto box = body.boundingRect();
        if (box.width() * box.height() < .12)
            return false;
        const auto position = i == 0
                                  ? QStringLiteral("initial")
                                  : (i + 1 == letters.size() ? QStringLiteral("final") : QStringLiteral("medial"));
        for (const auto& value : sizes) {
            const auto bounds = value.toObject();
            if (bounds.value("position") != position || !bounds.value("letters").toString().contains(letters[i]) ||
                (bounds.contains("template") &&
                 bounds.value("template") != sub.letterDiagnostics[i].toObject().value("template")))
                continue;
            if (box.width() < bounds.value("minWidth").toDouble() || box.height() < bounds.value("minHeight").toDouble() ||
                (bounds.contains("maxWidthToHeight") && box.width() > bounds.value("maxWidthToHeight").toDouble() * box.height()) ||
                (bounds.contains("lips") && !containsHeadLips(body, bounds.value("lips").toObject())))
                return false;
            // A descending bowl alone is insufficient: its raised head must stay above the incoming stroke.
            if (bounds.contains("minHeadRiseWidths")) {
                if (sub.joins.empty()) return false;
                const auto& join = *sub.joins.rbegin();
                QLineF entry(join.topBaseLine.point, join.bottomBaseLine.point);
                if (!sub.kashidaPaths.empty() && !sub.kashidaPaths.back().isEmpty()) {
                    if (sub.kashidaCuts.size() != sub.kashidaPaths.size()) return false;
                    entry = sub.kashidaCuts.back().second;
                }
                if (entry.isNull() || std::min(entry.y1(), entry.y2()) - box.top() <
                    bounds.value("minHeadRiseWidths").toDouble() * entry.length()) return false;
            }
        }
    }
    const auto bowl = settings.value("finalBowl").toObject();
    if (bowl.value("letters").toString().contains(letters.back())) {
        const auto full = map(shape.path).boundingRect(), final = map(sub.baseGlyphs.back().path).boundingRect();
        if (final.width() < bowl.value("minWidth").toDouble() ||
            final.height() < bowl.value("minHeight").toDouble() ||
            final.left() > full.left() + bowl.value("tipTolerance").toDouble(.2))
            return false;
    }
    return true;
}

// The prefix outline can start on a different curve after removing the arm.
// Return cut landmarks to their original contour and curve time for inspection.
static bool sourceLandmark(PathExtrema& mark, const std::vector<std::vector<QBezier>>& paths) {
    constexpr double tolerance = 1e-4;
    auto best = tolerance;
    int path = -1;
    double time = 0;
    for (int p = 0; p < int(paths.size()); ++p)
        for (int k = 0; k < int(paths[p].size()); ++k) {
            const auto& c = paths[p][k];
            const auto q = mark.point;
            if (q.x() < std::min({c.pt1().x(), c.pt2().x(), c.pt3().x(), c.pt4().x()}) - tolerance ||
                q.x() > std::max({c.pt1().x(), c.pt2().x(), c.pt3().x(), c.pt4().x()}) + tolerance ||
                q.y() < std::min({c.pt1().y(), c.pt2().y(), c.pt3().y(), c.pt4().y()}) - tolerance ||
                q.y() > std::max({c.pt1().y(), c.pt2().y(), c.pt3().y(), c.pt4().y()}) + tolerance)
                continue;
            const auto distance = [&](double t) { return QLineF(q, c.pointAt(t)).length(); };
            if (distance(0) < 1e-8) {
                mark.pathIndex = p;
                mark.time = k;
                return true;
            }
            if (distance(1) < 1e-8) {
                mark.pathIndex = p;
                mark.time = (k + 1) % paths[p].size();
                return true;
            }
            int closest = 0;
            for (int i = 1; i <= 16; ++i)
                if (distance(i / 16.) < distance(closest / 16.))
                    closest = i;
            double lo = std::max(0., (closest - 1) / 16.), hi = std::min(1., (closest + 1) / 16.);
            for (int i = 0; i < 35; ++i) {
                const auto t = (hi - lo) / 3.;
                if (distance(lo + t) < distance(hi - t))
                    hi -= t;
                else
                    lo += t;
            }
            const double t = (lo + hi) / 2, error = distance(t);
            if (error < best) {
                best = error;
                path = p;
                time = k + t;
            }
        }
    if (path < 0)
        return false;
    mark.pathIndex = path;
    mark.time = time;
    return true;
}

static bool restoreSourceCut(Stretching& cut, const std::vector<std::vector<QBezier>>& curves) {
    for (auto p : {&cut.topBaseLine, &cut.bottomBaseLine, &cut.topRight, &cut.bottomRight, &cut.topLeft, &cut.bottomLeft})
        if (!p->point.isNull() && !sourceLandmark(*p, curves))
            return false;
    return cut.counterEntry.pathIndex <= 0 ||
           (sourceLandmark(cut.counterEntry, curves) && sourceLandmark(cut.counterExit, curves));
}

static bool isSeparateFoldedArm(const QPainterPath& body, const QPainterPath& arm, bool allowPointContact) {
    const auto box = arm.boundingRect();
    if (!box.intersects(body.boundingRect()))
        return true;
    // Qt flattens Beziers for path intersections. Resolve the tip at a consistent
    // scale so a chord through the bend of the bowl cannot create a false bridge.
    const double scale = 1024.0 / std::max(box.width(), box.height());
    QTransform detail;
    detail.scale(scale, scale).translate(-box.center().x(), -box.center().y());
    const auto bowl = detail.map(body);
    const auto diagonal = detail.map(arm);
    return allowPointContact ? bowl.intersected(diagonal).isEmpty() : !bowl.intersects(diagonal);
}

// A folded alef meets the lam bowl either at a thin bridge or as a separate source outline.
bool PageAnalysisResult::recoverFoldedArm(Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config,
                                          bool attached) {
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 3 || sub.paths.empty())
        return false;
    const auto findRule = [&](const char* phase, const char* feature, QChar ch) {
        for (const auto& value : config.value(phase).toArray()) {
            const auto rule = value.toObject();
            if ((rule.contains(feature) || formDescription(config, rule, "body").contains(feature)) &&
                rule.value("current").toString().contains(ch))
                return rule;
        }
        return QJsonObject{};
    };
    auto lam = findRule("medial", attached ? "terminalArm" : "detachedExit", letters[letters.size() - 2]);
    if (lam.isEmpty())
        return false;
    const auto lamBody = formDescription(config, lam, "body");
    const auto lamCuts = formDescription(config, lam, "cuts");
    auto arm = findRule("final", "detachedArm", letters.back());
    if (attached) {
        arm = {};
        for (const auto& v : config.value("final").toArray()) {
            auto f = v.toObject();
            if (f.value("id") == lamBody.value("terminalArm") && f.value("method") == "folded-arm" &&
                f.value("current").toString().contains(letters.back())) {
                arm = f;
                break;
            }
        }
    } else if (!lamBody.value("detachedExit").toString().contains(letters.back()))
        return false;
    if (arm.isEmpty())
        return false;
    auto shape = word.bodyShape(sub);
    auto toPage = [&](const QPainterPath& path) {
        auto p = shape.transform.map(path);
        p.translate(shape.pos);
        return p;
    };
    QPainterPath connected, separate;
    Stretching seam{};
    std::vector<std::vector<QBezier>> curves;
    if (attached) {
        WordResultInfo::SubWordInfo dummy;
        auto ink = toPage(shape.path);
        ink.setFillRule(Qt::WindingFill);
        SearchContext context{.transformedPath = ink, .subword = dummy};
        context.bbox = ink.boundingRect();
        QVector<PathExtrema> extrema;
        ShapeItem::getExtrema(ink, context.subPaths, extrema);
        for (const auto& p : extrema)
            if (p.pathIndex == 0) {
                context.timeSorted.insert(p);
                context.xSorted.insert(p);
                context.ySorted.insert(p);
            }
        context.setOrientation();
        if (!foldedArmRoot(context, arm, seam))
            return false;
        auto halves = cutRegions(shape.path, {}, {seam}, context.orientation, true);
        if (halves.size() != 2)
            return false;
        connected = halves[0].path;
        separate = halves[1].path;
        for (const auto& path : context.subPaths)
            curves.push_back(baseContourCurves(path));
    } else {
        const auto paths = toSubpaths(shape.path);
        if (paths.size() < 2)
            return false;
        connected = paths[0];
        for (int i = 1; i < paths.size(); ++i) {
            if (paths[0].contains(paths[i]))
                connected.addPath(paths[i]);
            else if (separate.isEmpty() &&
                     isSeparateFoldedArm(paths[0], paths[i], config.contains("reconsiderFinalAttachments")))
                separate = paths[i];
            else
                return false;
        }
    }
    if (separate.isEmpty())
        return false;
    connected.setFillRule(shape.path.fillRule());
    const auto component = toPage(separate);
    const auto box = component.boundingRect();
    const auto settings = arm.value("detachedArm").toObject();
    if (box.height() < settings.value("minRise").toDouble(3) || box.width() < .25 * box.height() ||
        box.width() > 1.7 * box.height())
        return false;
    QVector<PathExtrema> extrema;
    std::vector<QPainterPath> outlines;
    ShapeItem::getExtrema(component, outlines, extrema);
    if (extrema.isEmpty())
        return false;
    const auto byY = [](const auto& a, const auto& b) { return a.point.y() < b.point.y(); };
    const auto [top, bottom] = std::minmax_element(extrema.begin(), extrema.end(), byY);
    const auto tip = attached ? QLineF(seam.topBaseLine.point, seam.bottomBaseLine.point).center() : bottom->point;
    if (!attached && bottom->point.x() - top->point.x() < .45 * box.width())
        return false;

    // Segment the attached prefix normally, with the lam's own stem and
    // returning bowl as its last body. Keep the original Beziers in both parts.
    PageAnalysisResult prefix;
    prefix.page.lines.resize(1);
    prefix.page.lines[0].words.resize(1);
    auto& part = prefix.page.lines[0].words[0];
    shape.path = connected;
    part.paths.append(shape);
    auto& result = part.wordResultInfo.subWords.emplace_back();
    result.paths = {0};
    result.text = letters.chopped(1);
    auto prefixForms = config;
    if (lamBody.value("shape") == "detached-return") {
        auto rules = prefixForms.value("final").toArray();
        auto body = lamBody;
        body.insert("shape", "terminal-return");
        body.insert("counters", false);
        auto cuts = lamCuts;
        cuts.insert("beforeFinal", false);
        cuts.insert("boundaries", QJsonObject{{"right", "fork-normal"}, {"left", "free"}});
        rules.append(QJsonObject{{"id", lam.value("id")}, {"current", lam.value("current")},
            {"ownsLeftTip", true}, {"body", body}, {"cuts", cuts}});
        prefixForms.insert("final", rules);
    }
    bool prefixComplete;
    if (attached) {
        auto rules = prefixForms.value("final").toArray();
        for (int i = 0; i < rules.size(); ++i) {
            auto form = rules[i].toObject();
            auto body = formDescription(prefixForms, form, "body");
            if (body.value("shape") == "terminal-return" &&
                form.value("current").toString().contains(result.text.back())) {
                auto cuts = formDescription(prefixForms, form, "cuts");
                auto section = cuts.value("section").toObject();
                section.insert("turnAngle", lamCuts.value("entryTurnAngle"));
                cuts.insert("section", section);
                form.insert("cuts", cuts);
                rules[i] = form;
            }

        }
        prefixForms.insert("final", rules);
        prefixForms["reconsiderForms"] =
            QJsonObject{{"initial", result.text}, {"medial", result.text}, {"final", result.text}};
        prefixComplete = prefix.segmentSubword(-2, 0, 0, 0, nullptr, true, &prefixForms);
    } else {
        prefixComplete = prefix.segmentWithLetterForms(-2, 0, 0, 0, nullptr, prefixForms, true);
    }
    if (!prefixComplete || result.provisional() || result.baseGlyphs.size() + 1 != size_t(letters.size()))
        return false;
    const auto bowl = toPage(result.baseGlyphs.back().path);
    QPainterPathStroker envelope;
    envelope.setWidth(2 * settings.value("maxTipGap").toDouble(.75));
    if (!envelope.createStroke(bowl).contains(tip) || bowl.boundingRect().top() >= box.center().y() ||
        bowl.boundingRect().bottom() <= tip.y())
        return false;
    if (attached) {
        if (result.joins.empty())
            return false;
        const auto& lastJoin = *result.joins.rbegin();
        const auto entry = !result.kashidaPaths.empty() && !result.kashidaPaths.back().isEmpty()
                               ? result.kashidaCuts.back().second
                               : QLineF(lastJoin.topBaseLine.point, lastJoin.bottomBaseLine.point);
        // A connector entering above the folded arm would take part of the upright stem.
        if (entry.isNull() || entry.center().y() + entry.length() < tip.y())
            return false;
        Joins original;
        for (auto cut : result.joins) {
            if (!restoreSourceCut(cut, curves))
                return false;
            cut.readingOrder = int(original.size());
            original.insert(cut);
        }
        seam.readingOrder = int(original.size());
        original.insert(seam);
        result.joins = std::move(original);
        result.kashidaPaths.emplace_back();
        result.kashidaCuts.emplace_back();
    }
    sub.baseGlyphs = std::move(result.baseGlyphs);
    sub.baseGlyphs.push_back({letters.right(1), separate});
    sub.joins = std::move(result.joins);
    sub.kashidaCuts = std::move(result.kashidaCuts);
    sub.kashidaPaths = std::move(result.kashidaPaths);
    sub.letterDiagnostics = std::move(result.letterDiagnostics);
    auto stem = sub.letterDiagnostics.last().toObject();
    stem.insert("position", "medial");
    stem.insert("prefixTemplate", stem.value("template"));
    stem.insert("template", lam.value("id"));
    stem.insert("candidates", QJsonArray{lam.value("id")});
    sub.letterDiagnostics[sub.letterDiagnostics.size() - 1] = stem;
    sub.letterDiagnostics.append(QJsonObject{{"letterIndex", letters.size()},
                                             {"position", "final"},
                                             {"character", letters.right(1)},
                                             {"status", "matched"},
                                             {"template", arm.value("id")},
                                             {"boundaryKind", attached ? "shared-folded-arm" : "separate-contours"},
                                             {"candidates", QJsonArray{arm.value("id")}}});
    sub.templateTrace = result.templateTrace;
    sub.templateTrace.append(lam.value("id").toString() + " · attached stem and returning bowl");
    sub.templateTrace.append(arm.value("id").toString() +
                             (attached ? " · sloping arm cut at its root" : " · separate sloping arm"));
    sub.matchedTemplates = result.matchedTemplates;
    sub.matchedTemplates << lam.value("id").toString() << arm.value("id").toString();
    sub.matchedTemplates.removeDuplicates();
    sub.correct = true;
    return true;
}

// Isolate an established entry seam, then recognize the exposed letter by its
// own outline. Accept the reconstruction only if every suffix letter fits.
static bool recoverExposedHead(const Word& word, WordResultInfo::SubWordInfo& sub,
                               const QJsonObject& config, const QJsonObject& checks) {
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 3 || sub.paths.empty() || sub.letterDiagnostics.size() != letters.size())
        return false;
    const auto initial = sub.letterDiagnostics.first().toObject();
    const bool knownEntry = initial.value("status") == "matched" && !sub.joins.empty() &&
                            sub.joins.begin()->cutSegments().size() == 1 &&
                            (sub.kashidaPaths.empty() || sub.kashidaPaths[0].isEmpty()) &&
                            sub.letterDiagnostics[1].toObject().value("status") != "matched";
    QJsonObject head, stem;
    for (const auto& value : config.value("medial").toArray()) {
        const auto rule = value.toObject();
        if (!rule.value("current").toString().contains(letters[1]))
            continue;
        if (knownEntry && rule.contains("exposedEntry")) {
            head = rule;
            break;
        }
        if (!rule.contains("exposedBy"))
            continue;
        for (const auto& item : config.value("initial").toArray()) {
            const auto form = item.toObject();
            if (form.value("id") == rule.value("exposedBy") && form.value("current").toString().contains(letters[0]) &&
                form.value("ligatureWith").toString().contains(letters[1]) &&
                descendingStemDescription(config, form).contains("headLip")) {
                stem = form;
                head = rule;
                break;
            }
        }
        if (!head.isEmpty()) break;
    }
    if (head.isEmpty()) return false;
    auto shape = word.bodyShape(sub);
    auto ink = shape.transform.map(shape.path).translated(shape.pos);
    ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy{};
    SearchContext ctx{.transformedPath = ink, .subword = dummy};
    ctx.bbox = ink.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(ink, ctx.subPaths, extrema);
    for (const auto& e : extrema) {
        if (e.pathIndex != 0) continue;
        ctx.timeSorted.insert(e);
        ctx.xSorted.insert(e);
        ctx.ySorted.insert(e);
    }
    if (ctx.timeSorted.size() < 3) return false;
    ctx.setOrientation();
    Stretching cut{};
    if (stem.isEmpty())
        cut = *sub.joins.begin();
    else {
        if (!declaredLamBoundary(ctx, config, stem)) return false;
        cut = ctx.res;
    }
    const auto parts = cutRegions(shape.path, {}, {cut}, ctx.orientation, true);
    if (parts.size() != 2) return false;

    PageAnalysisResult suffix;
    suffix.page.lines.resize(1); suffix.page.lines[0].words.resize(1);
    auto& tail = suffix.page.lines[0].words[0];
    shape.path = parts.back().path;
    tail.paths.append(shape);
    auto& result = tail.wordResultInfo.subWords.emplace_back();
    result.paths = {0}; result.text = letters.mid(1);
    if (!suffix.segmentSubword(-2, 0, 0, 0, nullptr, true, &config) || result.provisional() ||
        result.baseGlyphs.size() + 1 != size_t(letters.size()) ||
        !head.value("headForms").toArray().contains(result.letterDiagnostics.first().toObject().value("template")))
        return false;
    auto candidate = sub;
    candidate.baseGlyphs = std::move(result.baseGlyphs);
    candidate.baseGlyphs.insert(candidate.baseGlyphs.begin(), {letters.left(1), parts.front().path});
    candidate.kashidaPaths = std::move(result.kashidaPaths);
    candidate.kashidaPaths.emplace(candidate.kashidaPaths.begin());
    candidate.kashidaCuts = std::move(result.kashidaCuts);
    candidate.kashidaCuts.emplace(candidate.kashidaCuts.begin());
    candidate.joins.clear();
    cut.readingOrder = 0; cut.fixedLeftBoundary = cut.fixedRightBoundary = true;
    candidate.joins.insert(cut);
    std::vector<std::vector<QBezier>> curves;
    for (const auto& outline : ctx.subPaths) curves.push_back(baseContourCurves(outline));
    for (auto j : result.joins) {
        if (!restoreSourceCut(j, curves)) return false;
        j.readingOrder = int(candidate.joins.size());
        candidate.joins.insert(j);
    }
    const auto initialId = stem.isEmpty() ? initial.value("template").toString() : stem.value("id").toString();
    const auto headId = head.value("id").toString();
    candidate.letterDiagnostics = {};
    auto first = initial;
    first.insert("status", "matched");
    first.insert("template", initialId);
    auto candidates = first.value("candidates").toArray();
    if (!candidates.contains(initialId)) candidates.append(initialId);
    first.insert("candidates", candidates);
    candidate.letterDiagnostics.append(first);
    for (int i = 0; i < result.letterDiagnostics.size(); ++i) {
        auto d = result.letterDiagnostics[i].toObject();
        d.insert("letterIndex",i+2);
        if (i == 0) {
            d.insert("position","medial"); d.insert("exitTemplate",d.value("template"));
            d.insert("template",headId); d.insert("candidates",QJsonArray{headId});
        }
        candidate.letterDiagnostics.append(d);
    }
    candidate.matchedTemplates = result.matchedTemplates;
    candidate.matchedTemplates.prepend(headId); candidate.matchedTemplates.prepend(initialId);
    candidate.templateTrace = result.templateTrace;
    candidate.templateTrace.prepend(headId + " · exposed head at shared cut");
    candidate.templateTrace.prepend(initialId + (stem.isEmpty() ? " · existing exit retained" :
                                                                " · shaft ends at projecting head"));
    if (!plausibleRecoveredBodies(word,candidate,checks) || !coversOriginalInk(word,candidate)) return false;
    candidate.correct = true;
    sub = std::move(candidate);
    return true;
}

// In some overhanging heads the pen lifts between the incoming attachment
// and the head, even though the transcript remains joined. Recover the
// attachment from its own contour and the head from the other contour.
static bool detachedHeadEntry(Word& word, WordResultInfo::SubWordInfo& result, const QJsonObject& config) {
    const auto letters = PageAnalysisResult::bodyLetters(result.text);
    if (letters.size() < 4 || result.paths.empty())
        return false;
    QJsonObject form;
    QJsonArray lifted;
    for (const auto& value : config.value("medial").toArray()) {
        const auto rule = value.toObject();
        if (rule.contains("overhangingEntry") && form.isEmpty()) form = rule;
        if (rule.contains("penLift")) lifted.append(rule);
    }
    if (form.isEmpty() && lifted.isEmpty()) return false;
    const auto liftFor = [&](QChar letter, const char* side) {
        for (const auto& value : lifted) {
            const auto rule = value.toObject();
            if (rule.value("current").toString().contains(letter) &&
                rule.value("penLift").toObject().value("side") == side) return rule;
        }
        return QJsonObject{};
    };
    const auto shape = word.bodyShape(result);
    const auto paths = toSubpaths(shape.path);
    if (paths.size() < 2)
        return false;
    struct Component {
        QPainterPath ink;
        std::vector<int> indices;
    };
    std::vector<Component> pieces;
    for (int i = 0; i < paths.size(); ++i) {
        bool inside = false;
        for (int j = 0; j < paths.size() && !inside; ++j)
            inside = i != j && paths[j].contains(paths[i]);
        if (!inside)
            pieces.push_back({paths[i], {i}});
    }
    if (pieces.size() != 2 || pieces[0].ink.intersects(pieces[1].ink))
        return false;
    for (auto& piece : pieces) {
        const auto outer = piece.ink;
        for (int i = 0; i < paths.size(); ++i)
            if (i != piece.indices.front() && outer.contains(paths[i])) {
                piece.ink.addPath(paths[i]);
                piece.indices.push_back(i);
            }
        piece.ink.setFillRule(shape.path.fillRule());
    }
    auto world = [&](const QPainterPath& p) {
        auto q = shape.transform.map(p);
        q.translate(shape.pos);
        return q;
    };
    if (world(pieces[0].ink).boundingRect().center().x() < world(pieces[1].ink).boundingRect().center().x())
        std::swap(pieces[0], pieces[1]);
    const auto settings = form.value("overhangingEntry").toObject();
    QPainterPathStroker contact;
    contact.setWidth(2 * settings.value("maxGap").toDouble(.8));
    if (!contact.createStroke(world(pieces[0].ink)).intersects(world(pieces[1].ink)))
        return false;
    auto fit = [&](const Component& component, const QString& text) {
        PageAnalysisResult analysis;
        analysis.page.lines.resize(1);
        analysis.page.lines.front().words.resize(1);
        auto& localWord = analysis.page.lines.front().words.front();
        auto localShape = shape;
        localShape.path = component.ink;
        localWord.paths.append(localShape);
        auto& local = localWord.wordResultInfo.subWords.emplace_back();
        local.paths = {0};
        local.text = text;
        analysis.segmentSubword(-2, 0, 0, 0, nullptr, true, &config);
        return std::move(local);
    };
    std::optional<WordResultInfo::SubWordInfo> accepted;
    for (int slot = 2; slot + 1 < letters.size(); ++slot) {
        const auto exit = liftFor(letters[slot - 1], "exit"), entry = liftFor(letters[slot], "entry");
        const bool separate = !exit.isEmpty() && !entry.isEmpty();
        if (!separate && !form.value("current").toString().contains(letters[slot])) continue;
        const int prefixSize = separate ? slot : slot + 1;
        auto prefix = fit(pieces[0], letters.left(prefixSize)), suffix = fit(pieces[1], letters.mid(slot));
        if (!suffix.correct || prefix.provisional() || suffix.provisional() ||
            prefix.baseGlyphs.size() != size_t(prefixSize) || suffix.baseGlyphs.size() != size_t(letters.size() - slot) ||
            prefix.joins.size() != size_t(prefixSize - 1) || suffix.joins.size() + 1 != suffix.baseGlyphs.size() ||
            prefix.letterDiagnostics.size() != prefixSize || suffix.letterDiagnostics.size() != letters.size() - slot)
            continue;
        if (std::any_of(prefix.letterDiagnostics.constBegin(), prefix.letterDiagnostics.constEnd() - 1,
                        [](const auto& v) { return v.toObject().value("status") != "matched"; })) continue;
        const auto attachment = world(prefix.baseGlyphs.back().path), head = world(suffix.baseGlyphs.front().path);
        if (!contact.createStroke(attachment).intersects(head)) continue;
        if (separate) {
            const auto accepts = [](const QJsonObject& rule, const QJsonValue& letter) {
                return rule.value("penLift").toObject().value("forms").toArray().contains(letter.toObject().value("template"));
            };
            if (!prefix.correct || !accepts(exit, prefix.letterDiagnostics.last()) ||
                !accepts(entry, suffix.letterDiagnostics.first())) continue;
        } else {
            const auto box = attachment.boundingRect();
            if (toSubpaths(attachment).size() != 1 || box.width() <= box.height() ||
                box.width() > settings.value("maxEntryToHead").toDouble() * head.boundingRect().width()) continue;
        }
        auto assembled = result;
        assembled.baseGlyphs = prefix.baseGlyphs;
        assembled.letterDiagnostics = prefix.letterDiagnostics;
        if (separate) {
            auto d = assembled.letterDiagnostics.last().toObject();
            d.insert("componentTemplate", d.value("template"));
            d.insert("template", exit.value("id"));
            d.insert("candidates", QJsonArray{exit.value("id")});
            d.insert("position", "medial");
            assembled.letterDiagnostics[prefixSize - 1] = d;
            assembled.baseGlyphs.insert(assembled.baseGlyphs.end(), suffix.baseGlyphs.begin(), suffix.baseGlyphs.end());
        } else {
            assembled.baseGlyphs.back().path.addPath(suffix.baseGlyphs.front().path);
            assembled.baseGlyphs.back().path.setFillRule(shape.path.fillRule());
            assembled.baseGlyphs.insert(assembled.baseGlyphs.end(), suffix.baseGlyphs.begin() + 1, suffix.baseGlyphs.end());
            assembled.letterDiagnostics.removeLast();
        }
        for (int j = 0; j < suffix.letterDiagnostics.size(); ++j) {
            auto letter = suffix.letterDiagnostics[j].toObject();
            letter.insert("letterIndex", slot + j + 1);
            if (j == 0) {
                const auto id = (separate ? entry : form).value("id");
                letter.insert("headTemplate", letter.value("template"));
                letter.insert("template", id);
                letter.insert("position", "medial");
                letter.insert("candidates", QJsonArray{id});
                if (separate) letter.insert("boundaryKind", "separate-contours");
            }
            assembled.letterDiagnostics.append(letter);
        }
        assembled.joins.clear();
        assembled.kashidaPaths.clear();
        assembled.kashidaCuts.clear();
        const auto append = [&](const auto& run, const Component& piece) {
            for (auto cut : run.joins) {
                const auto mapIndex = [&](PathExtrema& p) {
                    if (p.pathIndex >= 0 && p.pathIndex < int(piece.indices.size()))
                        p.pathIndex = piece.indices[p.pathIndex];
                };
                for (auto p : {&cut.topBaseLine, &cut.topRight, &cut.topLeft, &cut.bottomBaseLine, &cut.bottomLeft,
                               &cut.bottomRight})
                    mapIndex(*p);
                if (cut.counterEntry.pathIndex > 0) {
                    mapIndex(cut.counterEntry);
                    mapIndex(cut.counterExit);
                }
                cut.readingOrder = int(assembled.joins.size());
                assembled.joins.insert(cut);
            }
            assembled.kashidaPaths.insert(assembled.kashidaPaths.end(), run.kashidaPaths.begin(),
                                          run.kashidaPaths.end());
            assembled.kashidaCuts.insert(assembled.kashidaCuts.end(), run.kashidaCuts.begin(),
                                         run.kashidaCuts.end());
        };
        append(prefix, pieces[0]);
        append(suffix, pieces[1]);
        assembled.matchedTemplates.clear();
        for (const auto& d : assembled.letterDiagnostics)
            assembled.matchedTemplates << d.toObject().value("template").toString();
        assembled.matchedTemplates.removeDuplicates();
        assembled.templateTrace.clear();
        for (const auto& d : assembled.letterDiagnostics) {
            const auto v = d.toObject();
            assembled.templateTrace << v.value("character").toString() + " · " + v.value("template").toString() +
                                           " · matched";
        }
        assembled.templateTrace << (separate ? QStringLiteral("Pen lift between two independently matched bodies") :
                                     form.value("id").toString() + " · incoming stroke and overhanging head share one letter");
        assembled.correct = true;
        if (!plausibleRecoveredBodies(word, assembled, config.value("recoveryChecks").toObject()) ||
            !coversOriginalInk(word, assembled))
            continue;
        if (accepted)
            return false;
        accepted = std::move(assembled);
    }
    if (!accepted)
        return false;
    result = std::move(*accepted);
    return true;
}

// Recover a contour match that is sensitive to pen scale only when the same templates
// find the same ink boundaries on both sides of the native scale.
static bool stableTemplateCuts(const Word& word, WordResultInfo::SubWordInfo& sub, QJsonObject config,
                               const QJsonArray& shapeFrames = {}) {
    const auto settings = config.value("reconsiderScale").toObject();
    if (settings.isEmpty())
        return false;
    config.remove("reconsiderScale"); // Recheck each scale once.
    const auto shape = word.bodyShape(sub);
    double width = 1, slant = 0, frameAngle = 0;
    auto trial = [&](double scale, double angle, WordResultInfo::SubWordInfo& candidate) {
        PageAnalysisResult page;
        page.page.lines.resize(1);
        page.page.lines.front().words.resize(1);
        auto& item = page.page.lines.front().words.front();
        auto scaled = shape;
        QTransform affine = QTransform::fromScale(scale * width, scale);
        affine.shear(slant, 0);
        affine.rotate(angle + frameAngle);
        scaled.transform *= affine;
        item.paths.append(scaled);
        auto& result = item.wordResultInfo.subWords.emplace_back();
        result.paths = {0};
        result.text = sub.text;
        if (!page.segmentSubword(-2, 0, 0, 0, nullptr, true, &config) || result.provisional())
            return false;
        // Masks are stored in the original path coordinates. Only inspection
        // landmarks, measured distances, and connector lines use page units.
        const auto inverse = affine.inverted();
        auto point = [&](QPointF p) {
            const auto delta = p - shape.pos;
            return (width == 1 && slant == 0 && angle + frameAngle == 0 ? delta / scale : inverse.map(delta)) + shape.pos;
        };
        auto line = [&](QLineF l) { return QLineF(point(l.p1()), point(l.p2())); };
        Joins restored;
        for (auto cut : result.joins) {
            const auto normal = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
            const double unit = normal.length() > 0 && (width != 1 || slant != 0) ?
                line(normal).length() / normal.length() : 1 / scale;
            for (auto* p : {&cut.topBaseLine, &cut.bottomBaseLine, &cut.topLeft, &cut.bottomLeft, &cut.topRight,
                            &cut.bottomRight}) {
                if (!p->point.isNull())
                    p->point = point(p->point);
            }
            if (cut.counterEntry.pathIndex > 0) {
                cut.counterEntry.point = point(cut.counterEntry.point);
                cut.counterExit.point = point(cut.counterExit.point);
            }
            cut.length *= unit;
            cut.referenceWidth *= unit;
            cut.topEndpointTolerance *= unit;
            restored.insert(cut);
        }
        result.joins = std::move(restored);
        for (auto& [a, b] : result.kashidaCuts) {
            if (!a.isNull())
                a = line(a);
            if (!b.isNull())
                b = line(b);
        }
        for (auto& mark : result.extrema)
            mark.point = point(mark.point);
        result.debugPath = {}; // Temporary search overlays use the trial coordinate system.
        candidate = std::move(result);
        return true;
    };
    const auto sameBoundaries = [&](const auto& lower, const auto& upper) {
        if (lower.matchedTemplates != upper.matchedTemplates || lower.joins.size() != upper.joins.size() ||
            lower.kashidaPaths.size() != upper.kashidaPaths.size() ||
            lower.baseGlyphs.size() != upper.baseGlyphs.size())
            return false;
        const double tol = settings.value("boundaryTolerance").toDouble();
        auto close = [&](QLineF a, QLineF b) {
            return QLineF(a.p1(), b.p1()).length() <= tol && QLineF(a.p2(), b.p2()).length() <= tol;
        };
        auto b = upper.joins.begin();
        for (const auto& a : lower.joins) {
            auto first = a.cutSegments(), second = b->cutSegments();
            if (first.size() != second.size())
                return false;
            for (size_t i = 0; i < first.size(); ++i)
                if (!close(first[i], second[i]))
                    return false;
            ++b;
        }
        for (size_t i = 0; i < lower.kashidaPaths.size(); ++i) {
            if (lower.kashidaPaths[i].isEmpty() != upper.kashidaPaths[i].isEmpty())
                return false;
            if (!lower.kashidaPaths[i].isEmpty() && (!close(lower.kashidaCuts[i].first, upper.kashidaCuts[i].first) ||
                                                     !close(lower.kashidaCuts[i].second, upper.kashidaCuts[i].second)))
                return false;
        }
        return true;
    };
    const auto stableAt = [&](double low, double high, double angleLow = 0, double angleHigh = 0) {
        WordResultInfo::SubWordInfo lower{}, upper{};
        if (!trial(low, angleLow, lower) || !trial(high, angleHigh, upper) || !sameBoundaries(lower, upper))
            return false;
        lower.paths = sub.paths;
        lower.secondaryBodyPath = sub.secondaryBodyPath;
        lower.bodyCounter = sub.bodyCounter;
        if (!plausibleRecoveredBodies(word, lower, config.value("recoveryChecks").toObject()) ||
            !coversOriginalInk(word, lower))
            return false;
        if (!shapeFrames.isEmpty())
            lower.templateTrace.append(QStringLiteral("Contour cuts agree at 99% and 101% scale with width %1, slant %2 and tilt %3°")
                                       .arg(width).arg(slant).arg(frameAngle));
        else if (angleLow != 0 || angleHigh != 0)
            lower.templateTrace.append(
                QStringLiteral("Same contour attachments at %1° and %2° tilt").arg(angleLow).arg(angleHigh));
        else
            lower.templateTrace.append(QStringLiteral("Same template boundaries at %1% and %2% scale")
                                           .arg(qRound(100 * low))
                                           .arg(qRound(100 * high)));
        sub = std::move(lower);
        return true;
    };
    if (!shapeFrames.isEmpty()) {
        for (const auto& frame : shapeFrames) {
            const auto pair = frame.toArray();
            width = pair[0].toDouble();
            slant = pair[1].toDouble();
            frameAngle = pair.size() == 3 ? pair[2].toDouble() : 0;
            if (stableAt(.99, 1.01)) return true;
        }
        return false;
    }
    const double step = settings.value("fraction").toDouble();
    if (stableAt(1 - step, 1 + step))
        return true;
    for (const auto& value : settings.value("alternateScales").toArray()) {
        const double center = value.toDouble();
        if (stableAt(center - step / 2, center + step / 2))
            return true;
    }
    for (const auto& value : settings.value("tilts").toArray()) {
        const double tilt = value.toDouble();
        if (stableAt(1, 1, tilt - .8, tilt + .8))
            return true;
    }
    return false;
}

// Confirm an existing boundary from the candidate body's own counter(s).
// Both attachments of a medial form must independently land on its rim.
static bool matchesCounterOutline(SearchContext& source, const QPainterPath& body, const QJsonObject& rule,
                                  const QLineF& entry, const QLineF& exit) {
    const auto contours = toSubpaths(body);
    const auto spec = counterOutlineSettings(rule);
    if (contours.size() != spec.value("counters").toInt() + 1 || contours.size() < 2)
        return false;
    QRectF eye;
    for (int i = 1; i < contours.size(); ++i) {
        if (!contours[0].contains(contours[i]))
            return false;
        eye = eye.united(contours[i].boundingRect());
    }
    const auto box = body.boundingRect();
    const double h = eye.height();
    if (h <= .1 || eye.width() <= .1)
        return false;
    const auto inside = [](double value, QJsonArray interval) {
        return interval.size() == 2 && value >= interval[0].toDouble() && value <= interval[1].toDouble();
    };
    if (!inside(eye.width() / h, spec.value("aspect").toArray()))
        return false;
    if (rule.value("body").toObject().value("shape") == "counter-outline") {
        // Recognize counter groups in the uncut source. Assembly may retain
        // or reject one of them, but cannot create evidence by clipping a hole.
        const auto evidence = recognizedSharedShape(source, rule.value("body").toObject());
        const bool retained = std::any_of(evidence->outlines.begin(), evidence->outlines.end(), [&](const auto& group) {
            if (group.counters.size() + 1 != contours.size()) return false;
            return std::all_of(group.counters.begin(), group.counters.end(), [&](const auto& hole) {
                return std::find(contours.begin() + 1, contours.end(), source.subPaths[hole.index]) != contours.end();
            });
        });
        if (!retained) return false;
    }
    const auto margins = spec.value("rim").toObject();
    if (!inside((eye.left() - box.left()) / h, margins.value("left").toArray()) ||
        !inside((eye.top() - box.top()) / h, margins.value("top").toArray()) ||
        !inside((box.right() - eye.right()) / h, margins.value("right").toArray()) ||
        !inside((box.bottom() - eye.bottom()) / h, margins.value("bottom").toArray()))
        return false;
    const auto attached = [&](const QLineF& cut, const QJsonObject& attachment) {
        if (attachment.isEmpty())
            return cut.isNull();
        if (cut.isNull())
            return false;
        const auto p = cut.center();
        return inside((p.x() - eye.left()) / eye.width(), attachment.value("x").toArray()) &&
               inside((p.y() - eye.top()) / h, attachment.value("y").toArray()) &&
               inside(cut.length() / h, attachment.value("width").toArray());
    };
    return attached(entry, spec.value("entry").toObject()) && attached(exit, spec.value("exit").toObject());
}

static void removeUnresolvedSlots(QStringList& trace, const QJsonArray& slotsBeforeRecovery) {
    for (const auto& value : slotsBeforeRecovery) {
        const auto slot = value.toObject();
        if (slot.value("status") != "matched")
            trace.removeAll(QString("%1 %2 · %3").arg(slot.value("position").toString(),
                            slot.value("character").toString(), slot.value("status").toString()));
    }
}

// Rebuild the last two masks together at the terminal letter's own attachment.
// The approach to the shared turn stays in the head stacked above it.
static bool recoverSharedTerminal(const Word& word, WordResultInfo::SubWordInfo& sub,
                              const QJsonObject& config, const QJsonObject& checks) {
    const auto text = PageAnalysisResult::bodyLetters(sub.text);
    const int count = text.size();
    if (count < 2 || sub.paths.empty() || sub.provisional() || sub.letterDiagnostics.size() != count ||
        sub.baseGlyphs.size() != sub.joins.size() + 1 ||
        (sub.joins.size() != size_t(count - 1) && sub.joins.size() != size_t(count - 2)))
        return false;
    for (int i = 0; i + 2 < count; ++i)
        if (sub.letterDiagnostics[i].toObject().value("status") != "matched")
            return false;
    QJsonObject receiver;
    for (const auto& value : config.value("final").toArray()) {
        const auto rule = value.toObject();
        if ((rule.value("method") == "folded-bowl" || rule.value("method") == "rising-tail") &&
            rule.value("sharedBoundary").toBool() &&
            rule.value("current").toString().contains(text.back())) {
            receiver = rule;
            break;
        }
    }
    if (receiver.isEmpty()) return false;
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    auto ink = mapped(shape.path);
    ink.setFillRule(Qt::WindingFill);
    WordResultInfo::SubWordInfo dummy{};
    SearchContext ctx{.transformedPath = ink, .subword = dummy};
    ctx.bbox = ink.boundingRect();
    QVector<PathExtrema> extrema;
    ShapeItem::getExtrema(ink, ctx.subPaths, extrema);
    for (const auto& e : extrema) if (e.pathIndex == 0) {
        ctx.timeSorted.insert(e); ctx.xSorted.insert(e); ctx.ySorted.insert(e);
    }
    if (ctx.timeSorted.size() < 3) return false;
    ctx.setOrientation();
    Stretching seam{};
    const bool folded = receiver.value("method") == "folded-bowl";
    if (!(folded ? foldedBowlEntry(ctx, receiver.value("fold").toObject(), seam) :
                   risingTailEntry(ctx, receiver.value("tail").toObject(), seam)))
        return false;
    const auto incoming = [&](int gap) -> std::optional<Stretching> {
        auto boundary = *std::next(sub.joins.begin(), gap);
        if (gap >= int(sub.kashidaPaths.size()) || sub.kashidaPaths[gap].isEmpty()) return boundary;
        if (gap >= int(sub.kashidaCuts.size())) return {};
        const auto edge = sub.kashidaCuts[gap].second;
        boundary = {};
        boundary.topBaseLine.point = edge.p1();
        boundary.bottomBaseLine.point = edge.p2();
        const std::vector<std::vector<QBezier>> outer{baseContourCurves(ctx.subPaths.front())};
        if (!sourceLandmark(boundary.topBaseLine, outer) || !sourceLandmark(boundary.bottomBaseLine, outer))
            return {};
        orientSection(ctx, boundary, seam.topBaseLine.time, true);
        return boundary;
    };
    // When the preceding mask enclosed the head's entire eye, its boundary
    // was on the wrong stroke. Recover the incoming edge from that eye as well.
    std::optional<Stretching> rim;
    if (count >= 3 && sub.letterDiagnostics[count - 2].toObject().value("status") != "matched") {
        const auto captured = toSubpaths(mapped(sub.baseGlyphs[count - 3].path));
        for (const auto& value : config.value("medial").toArray()) {
            const auto rule = value.toObject();
            if (!rule.contains("rimOffset") || rule.value("requiresExitTemplate") != receiver.value("id") ||
                !rule.value("current").toString().contains(text[count - 2])) continue;
            for (int i = 1; i < captured.size(); ++i) {
                const auto eye = captured[i].boundingRect();
                Stretching edge{};
                if (!medialChordAt(ctx, eye.right() + rule.value("rimOffset").toDouble() * eye.height(),
                                  eye.center().y(), 1.3 * eye.height(), edge)) continue;
                orientSection(ctx, edge, seam.topBaseLine.time, true);
                const auto parts = cutRegions(ink, {}, {edge, seam}, ctx.orientation, true);
                if (parts.size() != 3 || !matchesCounterOutline(ctx, parts[1].path, rule,
                        edge.cutSegments().front(), seam.cutSegments().front())) continue;
                if (rim) return false;
                rim = edge;
            }
        }
    }
    std::vector<Stretching> cuts;
    QLineF entry;
    if (rim && count > 3) {
        const auto preceding = incoming(count - 4);
        if (!preceding) return false;
        cuts.push_back(*preceding);
    }
    if (count > 2) {
        const auto prior = rim ? rim : incoming(count - 3);
        if (!prior) return false;
        if (prior->cutSegments().size() == 1) entry = prior->cutSegments().front();
        cuts.push_back(*prior);
    }
    cuts.push_back(seam);
    const auto pieces = cutRegions(shape.path, {}, cuts, ctx.orientation, true);
    if (pieces.size() != cuts.size() + 1) return false;
    auto candidate = sub;
    candidate.baseGlyphs.resize(count);
    const auto& head = pieces[pieces.size() - 2].path;
    auto sender = candidate.letterDiagnostics[count - 2].toObject();
    if (sender.value("status") != "matched") {
        bool matched = false;
        for (const auto& value : config.value(sender.value("position").toString()).toArray()) {
            const auto rule = value.toObject();
            if (!hasCounterOutline(rule) || !rule.value("current").toString().contains(text[count - 2]) ||
                (rule.contains("requiresExitTemplate") && rule.value("requiresExitTemplate") != receiver.value("id")) ||
                !matchesCounterOutline(ctx, mapped(head), rule, entry, seam.cutSegments().front()))
                continue;
            auto names = sender.value("candidates").toArray();
            if (!names.contains(rule.value("id"))) names.append(rule.value("id"));
            sender.insert("candidates", names);
            sender.insert("template", rule.value("id"));
            sender.insert("status", "matched");
            sender.insert("matchMethod", "counter-outline");
            candidate.matchedTemplates.append(rule.value("id").toString());
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    candidate.baseGlyphs[count - 2].path = head;
    candidate.baseGlyphs.back().path = pieces.back().path;
    candidate.baseGlyphs.back().text = text.right(1);
    candidate.kashidaPaths.resize(count - 1);
    candidate.kashidaCuts.resize(count - 1);
    candidate.kashidaPaths.back() = {};
    candidate.kashidaCuts.back() = {};
    if (rim) {
        candidate.baseGlyphs[count - 3].path = pieces[pieces.size() - 3].path;
        candidate.kashidaPaths[count - 3] = {};
        candidate.kashidaCuts[count - 3] = {};
    }
    candidate.joins.clear();
    for (auto it = sub.joins.begin(); candidate.joins.size() < size_t(count - (rim ? 3 : 2)); ++it) {
        auto kept = *it;
        kept.readingOrder = int(candidate.joins.size());
        candidate.joins.insert(kept);
    }
    if (rim) { rim->readingOrder = count - 3; candidate.joins.insert(*rim); }
    seam.readingOrder = count - 2;
    candidate.joins.insert(seam);
    const auto id = receiver.value("id").toString();
    sender.insert("sharedExitTemplate", id);
    candidate.letterDiagnostics[count - 2] = sender;
    auto terminal = candidate.letterDiagnostics.last().toObject();
    auto names = terminal.value("candidates").toArray();
    if (!names.contains(id)) names.append(id);
    terminal.insert("candidates", names);
    terminal.insert("template", id);
    terminal.insert("status", "matched");
    candidate.letterDiagnostics[count - 1] = terminal;
    candidate.matchedTemplates.append(id);
    removeUnresolvedSlots(candidate.templateTrace, sub.letterDiagnostics);
    if (sub.letterDiagnostics[count - 2].toObject().value("status") != "matched")
        candidate.templateTrace.append(sender.value("template").toString() + " · counter and body attachments matched");
    candidate.templateTrace.append(id + (folded ? " · shared cut at first turn of bowl" :
                                                 " · shared cut at root of rising tail"));
    if (!plausibleRecoveredBodies(word, candidate, checks) || !coversOriginalInk(word, candidate))
        return false;
    candidate.correct = true;
    sub = std::move(candidate);
    return true;
}

static bool confirmCounterOutlines(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config, const QJsonObject& recoveryChecks) {
    const auto text = PageAnalysisResult::bodyLetters(sub.text);
    if (sub.provisional() || text.size() < 2 || sub.baseGlyphs.size() != size_t(text.size()) ||
        sub.letterDiagnostics.size() != text.size() || sub.joins.size() + 1 != sub.baseGlyphs.size())
        return false;
    const auto shape = word.bodyShape(sub);
    WordResultInfo::SubWordInfo empty;
    OutlineSearchCache cache;
    SearchContext source{.transformedPath = shape.transform.map(shape.path).translated(shape.pos), .subword = empty};
    source.outlineCache = &cache;
    if (!readTerminalOutline(source)) return false;
    std::vector<QLineF> incoming(text.size()), outgoing(text.size());
    size_t j = 0;
    for (const auto& join : sub.joins) {
        if (join.cutSegments().size() == 1)
            outgoing[j] = incoming[j + 1] = QLineF(join.topBaseLine.point, join.bottomBaseLine.point);
        if (j < sub.kashidaPaths.size() && !sub.kashidaPaths[j].isEmpty()) {
            if (j >= sub.kashidaCuts.size())
                return false;
            outgoing[j] = sub.kashidaCuts[j].first;
            incoming[j + 1] = sub.kashidaCuts[j].second;
        }
        ++j;
    }
    auto diagnostics = sub.letterDiagnostics;
    QStringList matched, trace;
    for (int i = 0; i < text.size(); ++i) {
        auto slot = diagnostics[i].toObject();
        if (slot.value("status") == "matched")
            continue;
        const auto pos = slot.value("position").toString();
        const auto mask = shape.transform.map(sub.baseGlyphs[i].path).translated(shape.pos);
        bool certified = false;
        for (const auto& value : config.value(pos).toArray()) {
            const auto form = value.toObject();
            if (!hasCounterOutline(form) || form.contains("requiresExitTemplate") ||
                !form.value("current").toString().contains(text[i]) ||
                !matchesCounterOutline(source, mask, form, incoming[i], outgoing[i]))
                continue;
            const auto id = form.value("id").toString();
            auto candidates = slot.value("candidates").toArray();
            if (!candidates.contains(id))
                candidates.append(id);
            slot.insert("candidates", candidates);
            slot.insert("status", "matched");
            slot.insert("template", id);
            slot.insert("matchMethod", "counter-outline");
            diagnostics[i] = slot;
            matched.append(id);
            trace.append(id + " · counter and body attachments matched");
            certified = true;
            break;
        }
        if (!certified)
            return false;
    }
    if (matched.isEmpty() || !plausibleRecoveredBodies(word, sub, recoveryChecks) ||
        !coversOriginalInk(word, sub))
        return false;
    removeUnresolvedSlots(sub.templateTrace, sub.letterDiagnostics);
    sub.letterDiagnostics = std::move(diagnostics);
    for (const auto& id : matched)
        if (!sub.matchedTemplates.contains(id))
            sub.matchedTemplates.append(id);
    sub.templateTrace.append(trace);
    sub.correct = true;
    return true;
}

// At an upturned entry the two sides of the rising pen stroke meet the
// underside of the nose at two facing corners. A stacked descending arm
// instead meets the upper lip at a re-entrant corner, with ink above it.
static std::optional<Stretching> lipEntry(SearchContext& ctx, const MouthLips& lips,
                                          double maxThroat, bool stackedArm, const Stretching& preceding) {
    const auto pivot = lips.notch.point;
    const double span = pivot.x() - lips.upper.point.x();
    std::optional<Stretching> best;
    double shortest = std::numeric_limits<double>::infinity();
    const auto separates = [&](const Stretching& cut) {
        const auto side = getJoinTime(cut, ctx.orientation);
        return side.isStrictLeft(lips.upper.time) && side.isRight(preceding.topBaseLine.time) &&
               side.isRight(preceding.bottomBaseLine.time);
    };
    for (auto at = ctx.timeSorted.begin(); at != ctx.timeSorted.end(); ++at) {
        if (!at->isSharp) continue;
        const auto d = at->point - pivot;
        if (stackedArm) {
            if (d.x() >= 0 || d.x() <= -span || d.y() >= -.25 || d.y() < -span) continue;
            for (bool forward : {false, true}) {
                auto arm = (forward ? ctx.next(at) : ctx.prev(at))->point - at->point;
                auto head = (forward ? ctx.prev(at) : ctx.next(at))->point - at->point;
                if (arm.x() < .3 * span || arm.y() > -.25 || head.x() < .3 * span || head.y() <= 0) continue;
                Stretching cut{};
                if (!contourNormalCut(ctx, *at, forward, false, cut, true)) continue;
                const double width = QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point).length();
                if (width >= shortest || width > .6 * span) continue;
                orientSection(ctx, cut, lips.notch.time, true);
                if (!separates(cut)) continue;
                best = cut; shortest = width;
            }
        } else {
            if (d.x() <= 0 || d.x() > 1.5 * span || d.y() < -.05 || d.y() > .5 * span) continue;
            for (const auto& other : ctx.timeSorted) {
                const auto delta = other.point - at->point;
                if (!other.isSharp || delta.x() <= .04 || delta.x() > maxThroat ||
                    std::abs(delta.y()) > .35 * delta.x()) continue;
                const double width = std::hypot(delta.x(), delta.y());
                if (width >= shortest || !interiorChord(ctx.transformedPath, *at, other)) continue;
                Stretching cut{};
                cut.topBaseLine = *at; cut.bottomBaseLine = other;
                orientSection(ctx, cut, lips.notch.time, true);
                if (!separates(cut)) continue;
                best = cut; shortest = width;
            }
        }
    }
    return best;
}

// Match the sending head by its already established right cut. Another crest
// in this outline can satisfy the same shape template. Only an unambiguous
// left cut of this particular head can start its outgoing kashida.
static std::optional<Stretching> matchedOpenHeadExit(SearchContext& ctx, const QJsonArray& forms,
                                                     const QString& id, const Stretching& entry,
                                                     const Stretching& nextEntry) {
    const auto sameSeam = [](const Stretching& a, const Stretching& b) {
        return std::abs(a.topBaseLine.time - b.topBaseLine.time) < 1e-5 &&
               std::abs(a.bottomBaseLine.time - b.bottomBaseLine.time) < 1e-5;
    };
    for (const auto& value : forms) {
        const auto rule = value.toObject();
        if (rule.value("id") != id || !rule.contains("openHead")) continue;
        if (ctx.subPaths.empty()) {
            const auto paths = toSubpaths(ctx.transformedPath);
            ctx.subPaths.assign(paths.cbegin(), paths.cend());
        }
        std::optional<Stretching> exit;
        for (const auto& [right, left] : medialOpenHeadBoundaries(ctx, rule.value("openHead").toObject(),
                                                                 ctx.bbox.left(), ctx.bbox.right())) {
            if (!sameSeam(right, entry)) continue;
            const auto side = getJoinTime(left, ctx.orientation);
            if (!side.isStrictLeft(nextEntry.topBaseLine.time) || !side.isStrictLeft(nextEntry.bottomBaseLine.time))
                continue;
            if (exit && !sameSeam(*exit, left)) return {};
            exit = left;
        }
        return exit;
    }
    return {};
}

// Refine an already identified open mouth on the original contour. Work only
// between its actual neighbouring bodies; an ambiguous mouth keeps its cuts.
void PageAnalysisResult::refineLipCuts(int line, int wordIndex, int subIndex, const QJsonObject& config) {
    auto& word = page.lines[line].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subIndex];
    const auto letters = bodyLetters(sub.text);
    if (letters.size() < 3 || sub.letterDiagnostics.size() != letters.size() ||
        sub.kashidaPaths.size() != sub.joins.size() || sub.kashidaCuts.size() != sub.joins.size() ||
        sub.baseGlyphs.size() != size_t(letters.size()) || int(sub.joins.size()) + 1 != letters.size()) return;
    QJsonObject form;
    for (const auto& v : config.value("medial").toArray()) {
        const auto rule = v.toObject();
        if (rule.value("mouth").toObject().contains("exitMargin") &&
            std::any_of(letters.cbegin() + 1, letters.cend() - 1,
                        [&](QChar ch) { return rule.value("current").toString().contains(ch); })) {
            form = rule; break;
        }
    }
    if (form.isEmpty()) return;
    const auto mouth = form.value("mouth").toObject();
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    auto ink = mapped(shape.path); ink.setFillRule(Qt::WindingFill);
    SearchContext ctx{.transformedPath = ink, .bbox = ink.boundingRect(), .subword = sub};
    ctx.orientation = sub.basePathOrientation;
    for (const auto& mark : sub.extrema) if (!mark.pathIndex) ctx.timeSorted.insert(mark);
    const auto mouths = mouthLips(ctx, mouth);
    const auto before = [&](const Stretching& first, const Stretching& next) {
        const auto side = getJoinTime(first, ctx.orientation);
        return side.isStrictLeft(next.topBaseLine.time) && side.isStrictLeft(next.bottomBaseLine.time);
    };
    const auto chord = [](const Stretching& s) { return QLineF(s.topBaseLine.point, s.bottomBaseLine.point); };
    const auto receiver = [&](Stretching cut, int index) {
        if (!sub.kashidaPaths[index].isEmpty()) {
            cut.topBaseLine = cut.topLeft; cut.bottomBaseLine = cut.bottomLeft;
        }
        return cut;
    };
    const auto join = [](Stretching sending, const Stretching& receiving, int order, bool shared) {
        sending.topRight = sending.topBaseLine; sending.bottomRight = sending.bottomBaseLine;
        sending.topLeft = receiving.topBaseLine; sending.bottomLeft = receiving.bottomBaseLine;
        sending.fixedLeftBoundary = sending.fixedRightBoundary = true;
        sending.readingOrder = order; sending.length = 0;
        sending.kashidaOverrides.insert("enabled", !shared);
        return sending;
    };
    for (int i = 1; i < letters.size() - 1; ++i) {
        if (!form.value("current").toString().contains(letters[i])) continue;
        const auto slot = sub.letterDiagnostics[i].toObject();
        const bool ain = QStringLiteral("عغ").contains(letters[i-1]);
        const bool seen = QStringLiteral("سش").contains(letters[i-1]);
        if (!ain && !seen && slot.value("template") != form.value("id")) continue;
        auto cuts = std::vector<Stretching>(sub.joins.begin(), sub.joins.end());
        const auto entry = cuts[i-1]; const auto exit = cuts[i];
        if (entry.counterEntry.pathIndex > 0 || exit.counterEntry.pathIndex > 0 ||
            toSubpaths(sub.baseGlyphs[i].path).size() != 1) continue;
        const auto oldHeadStart = receiver(entry, i-1);
        const auto nextBody = receiver(exit, i);
        const auto rightSide = getJoinTime(oldHeadStart, ctx.orientation);
        const auto leftSide = getJoinTime(exit, ctx.orientation);
        const MouthLips* found = nullptr;
        for (const auto& m : mouths) {
            if (!rightSide.isStrictLeft(m.notch.time) || !rightSide.isStrictLeft(m.upper.time) ||
                !leftSide.isStrictRight(m.notch.time) || !leftSide.isStrictRight(m.upper.time)) continue;
            if (found) { found = nullptr; break; }
            found = &m;
        }
        if (!found) continue;
        // At the free tip's projection there is only the departing stroke.
        // Keep the last fraction of the nib with the lip before starting the join.
        Stretching newExit = exit, projected{};
        const double x = found->upper.point.x() - mouth.value("exitMargin").toDouble();
        if (medialChordAt(ctx, x, found->lower.point.y(), 2.5, projected)) {
            orientSection(ctx, projected, found->notch.time, false);
            if (before(oldHeadStart, projected) && before(projected, nextBody) &&
                chord(projected).length() <= 2.5 && std::abs(chord(projected).center().x() - chord(exit).center().x()) > .05)
                newExit = projected;
        }
        auto headStart = oldHeadStart, sender = entry;
        const bool wasShared = sub.kashidaPaths[i-1].isEmpty();
        bool shared = wasShared;
        std::optional<Stretching> ainExit;
        const bool refineSharedEntry = wasShared && mouth.value("separateSharedEntry").toBool();
        if (ain || seen || refineSharedEntry) {
            auto start = lipEntry(ctx, *found, mouth.value("maxThroat").toDouble(), false, oldHeadStart);
            if (!start && ain) start = lipEntry(ctx, *found, 0, true, oldHeadStart);
            if (start && before(oldHeadStart, *start) && before(*start, newExit)) {
                if (ain && shared) { sender = headStart = *start; }
                else {
                    headStart = *start; shared = false;
                    if (seen && i == 1) {
                        for (const auto& v : config.value("initial").toArray()) {
                            auto rule = v.toObject();
                            if (rule.value("method") != "seen-teeth" ||
                                rule.value("id") != sub.letterDiagnostics[0].toObject().value("template")) continue;
                            rule.insert("exitBendFirst", true);
                            rule.insert("exitOffset", .25);
                            Stretching tooth{};
                            if (seenTeethBoundaries(ctx, rule, ctx.bbox.left(), ctx.bbox.right()+1e-6, nullptr, &tooth) &&
                                before(entry, tooth) && before(tooth, headStart)) sender = tooth;
                            break;
                        }
                    }
                }
            }
            if (ain && i > 1 && shared && start && chord(headStart) == chord(*start)) {
                ainExit = matchedOpenHeadExit(ctx, config.value("medial").toArray(),
                    sub.letterDiagnostics[i-1].toObject().value("template").toString(),
                    receiver(cuts[i-2], i-2), headStart);
            }
        }
        const bool exitChanged = chord(newExit) != chord(exit);
        const auto apply = [&](const Stretching& sending, bool sharedEntry) {
            const bool entryChanged = sharedEntry != wasShared || chord(sending) != chord(entry) ||
                                      chord(headStart) != chord(oldHeadStart);
            if (!entryChanged && !exitChanged) return false;
            cuts[i-1] = entryChanged ? join(sending, headStart, entry.readingOrder, sharedEntry) : entry;
            if (exitChanged) cuts[i] = join(newExit, nextBody, exit.readingOrder, false);
            auto previous = sub;
            sub.joins.clear();
            for (const auto& c : cuts) sub.joins.insert(c);
            bool ok = sub.joins.size() == cuts.size() && cutSubword(line, wordIndex, subIndex, config) &&
                      sub.baseGlyphs.size() == previous.baseGlyphs.size();
            // A distinct exit is useful only if the two specified seams bound
            // one connecting stroke and the other letter masks retain their ink.
            for (int j = 0; ok && j < letters.size(); ++j) {
                if (j != i-1 && j != i) ok = sub.baseGlyphs[j].path == previous.baseGlyphs[j].path;
                if (j >= int(cuts.size())) break;
                if (j != i-1 && j != i) ok &= previous.kashidaPaths[j] == sub.kashidaPaths[j];
                else if ((j == i-1 && entryChanged && !sharedEntry) || (j == i && exitChanged)) {
                    const auto& [r,l] = sub.kashidaCuts[j];
                    ok &= r == chord(j == i ? newExit : sending) && l == chord(j == i ? nextBody : headStart) &&
                          followsOneStroke(mapped(sub.kashidaPaths[j]), r, l);
                } else if (j == i-1 && sharedEntry) ok &= sub.kashidaPaths[j].isEmpty();
            }
            if (ok && coversOriginalInk(word, sub)) return true;
            sub = std::move(previous);
            return false;
        };
        if (ainExit && apply(*ainExit, false)) {
            sub.templateTrace.append(sub.letterDiagnostics[i-1].toObject().value("template").toString() +
                                     " · own exit starts kashida before the open lip");
        } else if (!apply(sender, shared)) {
            continue;
        }
        auto diagnostic = slot;
        diagnostic.insert("boundaryTemplate", form.value("id"));
        sub.letterDiagnostics[i] = diagnostic;
        if (!sub.matchedTemplates.contains(form.value("id").toString())) sub.matchedTemplates.append(form.value("id").toString());
        sub.templateTrace.append(form.value("id").toString() + " · open lip boundaries refined");
    }
}

void PageAnalysisResult::refineBodyOwnership(int line, int wordIndex, int subIndex, const QJsonObject& config) {
    auto& word = page.lines[line].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subIndex];
    const auto letters = bodyLetters(sub.text);
    const int last = letters.size() - 1;
    if (last < 1 || sub.joins.size() != size_t(last) ||
        sub.kashidaPaths.size() != size_t(last) || sub.baseGlyphs.size() != size_t(last + 1) ||
        sub.letterDiagnostics.size() != last + 1)
        return;
    const auto templateId = [&](int slot) { return sub.letterDiagnostics[slot].toObject().value("template").toString(); };
    const auto initial = templateId(0), terminal = templateId(last);
    const auto shape = word.bodyShape(sub);
    const auto world = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    const auto first = world(sub.baseGlyphs.front().path).boundingRect();
    const auto tail = world(sub.baseGlyphs.back().path).boundingRect();
    const bool clippedLam = initial == "lam-normal-init" && templateId(1) == "hah-stacked-medi" && first.height() < 3.2;
    const bool clippedSeen = initial == "seen-init" && first.width() < 1.5 && first.height() < 2;
    const bool clippedHah = initial == "hah-open-init" && first.width() < 1 && first.height() < 1;
    const bool clippedTooth = initial == "beh-shallow-step-init" && (first.width() < .25 || first.height() < .6);
    const bool lostShaft = terminal == "alef-fina" && letters[last - 1] == QChar(0x0647) &&
        std::min(tail.width(), tail.height()) < .05;
    const bool lostFold = terminal == "yeh-fina" && templateId(last - 1) == "lam-medi" &&
        tail.width() < 1.2 && tail.height() < 1.4;
    const bool risingTail = terminal == "reh-rising-tail-fina" && initial == "ain-round-init";
    const bool clippedReh = !risingTail && (terminal == "reh-rising-tail-fina" || terminal == "reh-fina" ||
        terminal == "reh-shouldered-fina" || terminal == "reh-hook-fina");
    const bool clippedKaf = initial == "kaf-hook-init" && terminal == "waw-corner-bridge-fina" &&
        last == 1 && world(sub.kashidaPaths.front()).boundingRect().width() > 5;
    const bool connectedNoon = letters[last] == QChar(0x0646) && !sub.kashidaPaths.back().isEmpty();
    const bool liftedAlef = terminal == "alef-return-fina" && letters[last - 1] == QChar(0x0644) &&
        tail.width() < 1.5 && sub.kashidaPaths.back().isEmpty();
    if (!clippedLam && !clippedSeen && !clippedHah && !clippedTooth && !lostShaft && !lostFold &&
        !risingTail && !clippedReh && !clippedKaf && !liftedAlef && !connectedNoon)
        return;

    WordResultInfo::SubWordInfo trial{};
    SearchContext outline{.transformedPath = world(shape.path), .subword = trial};
    if (!readTerminalOutline(outline)) return;
    outline.baseText = letters;
    outline.joinDefaults = config.value("joinDefaults").toObject();
    const auto form = [&](const char* position, const QString& id) {
        for (const auto& value : config.value(position).toArray())
            if (value.toObject().value("id") == id) return value.toObject();
        return QJsonObject{};
    };
    const auto walk = [&](const char* position, const QString& id, bool seen = false) -> std::optional<Stretching> {
        outline.form = form(position, id);
        outline.nextIter = std::next(outline.xSorted.begin());
        outline.res = {};
        if (outline.form.isEmpty() || !(outline.form.contains("body") ?
            declaredLamBoundary(outline, config, outline.form) :
            seen ? searchSeenTeeth(outline) : searchContourWalk(outline))) return {};
        return outline.res;
    };
    const auto entry = [](Stretching cut) {
        cut.topBaseLine = cut.topLeft;
        cut.bottomBaseLine = cut.bottomLeft;
        return cut;
    };
    const auto replace = [&](int slot, Stretching right, const std::optional<Stretching>& left,
                             const QString& message, const auto& accepts,
                             const std::optional<Stretching>& incoming = std::nullopt) {
        if (!interiorChord(outline.transformedPath, right.topBaseLine, right.bottomBaseLine) ||
            (left && !interiorChord(outline.transformedPath, left->topBaseLine, left->bottomBaseLine))) return false;
        auto previous = sub;
        right.length = 0;
        right.fixedRightBoundary = true;
        right.fixedLeftBoundary = left.has_value();
        right.topRight = right.topBaseLine;
        right.bottomRight = right.bottomBaseLine;
        right.topLeft = left ? left->topBaseLine : right.topBaseLine;
        right.bottomLeft = left ? left->bottomBaseLine : right.bottomBaseLine;
        right.kashidaOverrides.insert("enabled", left.has_value());
        sub.joins.clear();
        int i = 0;
        for (auto cut : previous.joins) {
            if (i == slot) cut = right;
            else if (incoming && i == slot - 1) cut = *incoming;
            cut.readingOrder = i++;
            sub.joins.insert(cut);
        }
        bool valid = cutSubword(line, wordIndex, subIndex, config) &&
            sub.baseGlyphs.size() == previous.baseGlyphs.size() && sub.kashidaPaths.size() == previous.kashidaPaths.size() &&
            sub.kashidaPaths[slot].isEmpty() == !left && accepts();
        for (size_t j = 0; valid && j < sub.baseGlyphs.size(); ++j)
            if (j != size_t(slot) && j != size_t(slot + 1) && !(incoming && j + 1 == size_t(slot)))
                valid = sub.baseGlyphs[j].path == previous.baseGlyphs[j].path;
        for (size_t j = 0; valid && j < sub.kashidaPaths.size(); ++j)
            if (incoming && j + 1 == size_t(slot)) {
                valid = sub.kashidaPaths[j].isEmpty() == previous.kashidaPaths[j].isEmpty();
                if (valid && !sub.kashidaPaths[j].isEmpty())
                    valid = sub.kashidaCuts[j].first == previous.kashidaCuts[j].first &&
                        sub.baseGlyphs[j].path == previous.baseGlyphs[j].path;
            } else if (j != size_t(slot)) valid = sub.kashidaPaths[j] == previous.kashidaPaths[j];
        if (!valid || !coversOriginalInk(word, sub)) {
            sub = std::move(previous);
            return false;
        }
        sub.templateTrace.append(message);
        return true;
    };

    if (clippedLam || clippedSeen || clippedHah || clippedTooth) {
        const auto current = *sub.joins.begin();
        auto connector = sub.kashidaPaths.front().isEmpty() ? std::optional<Stretching>{} : entry(current);
        std::optional<Stretching> boundary;
        if (clippedLam) {
            const auto declaration = form("initial", "lam-hah-stacked-init");
            if (!declaration.isEmpty() && declaredLamBoundary(outline, config, declaration)) boundary = outline.res;
        } else if ((clippedSeen || clippedTooth) && last >= 2 && QStringLiteral("جحخ").contains(letters[1])) {
            const auto settings = form("medial", "hah-stacked-medi").value("upperAttachment").toObject();
            boundary = stackedAttachment(outline, settings, first, world(sub.baseGlyphs[1].path),
                                         current, *std::next(sub.joins.begin()), true);
            if (boundary) connector.reset();
        } else {
            boundary = walk("initial", clippedTooth ? "beh-foot-init" : initial, clippedSeen);
        }
        if (boundary) {
            const auto restoresBody = [&] {
                const auto repaired = world(sub.baseGlyphs.front().path).boundingRect();
                return clippedLam ? repaired.height() >= 2 * first.height() :
                    repaired.width() >= std::max(.45, 1.5 * first.width()) && repaired.height() >= 1.2;
            };
            replace(0, *boundary, connector, initial + " · initial stroke restored at its own boundary", restoresBody);
        }
    }

    if (lostShaft || lostFold) {
        auto seam = entry(*sub.joins.rbegin());
        orientSection(outline, seam, outline.xSorted.rbegin()->time, true);
        const auto restoresBody = [&] {
            const auto repaired = world(sub.baseGlyphs.back().path).boundingRect();
            return repaired.left() <= outline.bbox.left() + 1e-5 &&
                (lostShaft ? repaired.height() >= 4 : repaired.width() >= 2.5 && repaired.height() >= 1.4);
        };
        replace(last - 1, seam, {}, terminal + " · terminal stroke restored at its own boundary", restoresBody);
    }

    if (risingTail) {
        const auto join = *sub.joins.rbegin();
        const auto seam = sub.kashidaPaths.back().isEmpty() ? join : entry(join);
        auto boundary = cutAfterEntryCrest(outline, seam);
        if (!boundary && last > 1) {
            boundary = walk("final", "reh-rising-root-fina");
            if (boundary) orientSection(outline, *boundary, outline.xSorted.rbegin()->time, true);
        }
        if (boundary) {
            const auto right = last == 1 ? walk("initial", initial) : std::optional<Stretching>{join};
            if (right)
                replace(last - 1, *right, boundary, terminal + " · raised entry restored", [&] {
                    return world(sub.baseGlyphs.back().path).boundingRect().top() < tail.top() - .3;
                });
        }
    }

    if (clippedReh) {
        const auto join = *sub.joins.rbegin();
        const bool connected = !sub.kashidaPaths.back().isEmpty();
        const auto seam = connected ? entry(join) : join;
        std::optional<Stretching> preceding;
        if (last > 1) {
            const auto before = *std::next(sub.joins.rbegin());
            preceding = sub.kashidaPaths[last - 2].isEmpty() ? before : entry(before);
            orientSection(outline, *preceding, outline.xSorted.rbegin()->time, true);
        }
        // With an existing connector, the recovered hook must stay within
        // that connector so its sending edge and the previous body stay put.
        auto limit = connected ? std::optional<Stretching>{join} : preceding;
        if (limit) orientSection(outline, *limit, outline.xSorted.rbegin()->time, true);
        const auto boundary = cutAfterEntryCrest(outline, seam, limit ? &*limit : nullptr, true);
        if (boundary) {
            const auto restoredHook = [&] {
                return world(sub.baseGlyphs.back().path).boundingRect().top() < tail.top() - .2 * tail.height();
            };
            const auto message = terminal + " · upper hook retained in final body";
            const auto chord = [](const Stretching& cut) { return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point); };
            const bool sliver = preceding && !connected &&
                QLineF(chord(*preceding).center(), chord(*boundary).center()).length() < .4 * chord(*boundary).length();
            if (!sliver) {
                replace(last - 1, connected ? join : *boundary, connected ? boundary : std::nullopt, message, restoredHook);
            } else if (templateId(last - 1).startsWith("beh-")) {
                // The old tooth match can be the reh hook itself. If giving
                // that hook back leaves a sliver, look for the actual tooth
                // and recover its incoming boundary at the same time.
                const auto oldEntry = *std::next(sub.joins.rbegin());
                const bool ribbon = !sub.kashidaPaths[last - 2].isEmpty();
                const auto declaration = resolvedBodyForm(config, form("medial", "beh-shoulder-medi"));
                auto teeth = declaredToothBoundaries(outline, declaration);
                std::stable_sort(teeth.begin(), teeth.end(), [](const auto& a, const auto& b) {
                    return a.tip.point.y() < b.tip.point.y();
                });
                for (const auto& tooth : teeth) {
                    auto start = *tooth.right;
                    orientSection(outline, start, outline.xSorted.rbegin()->time, true);
                    if (!getJoinTime(start, outline.orientation).isStrictLeft(preceding->topBaseLine.time) ||
                        !getJoinTime(start, outline.orientation).isStrictLeft(preceding->bottomBaseLine.time)) continue;
                    auto incoming = ribbon ? oldEntry : start;
                    incoming.readingOrder = last - 2;
                    incoming.topLeft = start.topBaseLine;
                    incoming.bottomLeft = start.bottomBaseLine;
                    incoming.fixedRightBoundary = true;
                    incoming.fixedLeftBoundary = ribbon;
                    incoming.kashidaOverrides.insert("enabled", ribbon);
                    if (replace(last - 1, *boundary, {}, message + " and preceding tooth restored", [&] {
                            return restoredHook() && tooth.retainedBy(world(sub.baseGlyphs[last - 1].path));
                        }, incoming)) break;
                }
            }
        }
    }

    if (clippedKaf) {
        const double rail = .45 * world(sub.kashidaPaths.front()).boundingRect().width();
        const QJsonObject parallel{{"minRun", rail}, {"minGap", .25}};
        if (const auto cut = walk("initial", "kaf-elbow-init"))
            replace(0, *cut, entry(*sub.joins.begin()), initial + " · lower return retained in kaf", [&] {
                return containsHeadLips(world(sub.baseGlyphs.front().path), parallel);
            });
    }

    if (connectedNoon && sub.kashidaCuts.size() == sub.kashidaPaths.size()) {
        const auto [rightCap, leftCap] = sub.kashidaCuts.back();
        const std::vector<std::vector<QBezier>> outer{baseContourCurves(outline.subPaths.front())};
        const auto readCap = [&](const QLineF& cap) -> std::optional<Stretching> {
            Stretching cut{};
            cut.topBaseLine.point = cap.p1();
            cut.bottomBaseLine.point = cap.p2();
            if (!sourceLandmark(cut.topBaseLine, outer) || !sourceLandmark(cut.bottomBaseLine, outer)) return {};
            orientSection(outline, cut, outline.xSorted.rbegin()->time, true);
            return cut;
        };
        const auto sending = readCap(rightCap), receiving = readCap(leftCap);
        const auto boundary = sending && receiving ? restoreNoonBowlEntry(outline, *receiving, *sending) : std::nullopt;
        if (boundary)
            replace(last - 1, *sending, boundary, terminal + " · outer bowl retained", [&] {
                const auto repaired = world(sub.baseGlyphs.back().path).boundingRect();
                return repaired.right() > tail.right() && repaired.left() <= tail.left() + 1e-5 &&
                    repaired.bottom() >= tail.bottom() - 1e-5;
            });
    }

    if (liftedAlef) {
        const auto contours = toSubpaths(shape.path);
        if (contours.size() != 2) return;
        const auto branch = world(contours[1]);
        const auto main = world(contours[0]);
        const auto box = branch.boundingRect();
        if (box.height() < 3 || box.width() < 2 || box.top() > main.boundingRect().top() + 3 ||
            box.left() >= tail.left() || branch.intersects(main))
            return;
        const auto top = singleHorizontalStroke(branch, box, box.top() + .25 * box.height());
        const auto bottom = singleHorizontalStroke(branch, box, box.top() + .75 * box.height());
        if (!top || !bottom || bottom->center().x() - top->center().x() < .3 * box.height() ||
            std::max(top->length(), bottom->length()) > .45 * box.height())
            return;
        auto previous = sub;
        sub.joins.erase(std::prev(sub.joins.end()));
        if (!cutSubword(line, wordIndex, subIndex, config) || sub.baseGlyphs.size() != size_t(last)) {
            sub = std::move(previous);
            return;
        }
        sub.baseGlyphs.push_back({letters.right(1), contours[1]});
        bool valid = sub.kashidaPaths.size() + 1 == previous.kashidaPaths.size();
        for (int i = 0; valid && i < last - 1; ++i)
            valid = sub.baseGlyphs[i].path == previous.baseGlyphs[i].path &&
                sub.kashidaPaths[i] == previous.kashidaPaths[i];
        if (!valid || !coversOriginalInk(word, sub)) {
            sub = std::move(previous);
            return;
        }
        auto diagnostic = sub.letterDiagnostics[last].toObject();
        diagnostic.insert("boundaryKind", "separate-contours");
        sub.letterDiagnostics[last] = diagnostic;
        sub.templateTrace.append(terminal + " · detached diagonal retained in alef");
    }
}

// A terminal lam needs only its incoming fork flank. Its bowl, lower return,
// and any shared alef seam are body topology, not a second cut to shorten.
static bool resolveLamForkEntry(const Word& word, const LamBodyRecognition& recognition,
        SearchContext& context, WordResultInfo::SubWordInfo& sub, int slot,
        const QJsonObject& bodySpec, const QJsonObject& policy, const QJsonObject& config) {
    const auto old = sub;
    const std::vector<Stretching> joins(old.joins.begin(), old.joins.end());
    const auto& incoming = joins[slot - 1];
    if (incoming.counterEntry.pathIndex > 0) return false;
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    const auto bounds = mapped(old.baseGlyphs[slot].path).boundingRect();
    const auto chord = [](const Stretching& cut) { return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point); };
    const auto same = [](double a, double b) { return std::abs(a - b) < .05; };
    const auto ordered = [&](const Stretching& right, const Stretching& left) {
        const auto r = getJoinTime(right, context.orientation), l = getJoinTime(left, context.orientation);
        return r.isLeft(left.topBaseLine.time) && r.isLeft(left.bottomBaseLine.time) &&
            l.isRight(right.topBaseLine.time) && l.isRight(right.bottomBaseLine.time);
    };
    const auto entrySide = getJoinTime(incoming, context.orientation);
    const bool hasExit = slot < int(joins.size());
    // Assign the recognized shaft to this transcript slot. The crest and fork
    // come from uncut ink; no landmark of the preceding letter defines them.
    std::vector<std::pair<double, const LamForkBody*>> candidates;
    for (const auto& body : recognition.lamBodies.forks) {
        if (!body.isConcave(context.orientation) || (!body.fork && !body.folded()) ||
            !entrySide.isStrictLeft(body.tip.time) ||
            (hasExit && !getJoinTime(joins[slot], context.orientation).isStrictRight(body.tip.time))) continue;
        double score = std::numeric_limits<double>::infinity();
        for (const auto& crest : recognition.lamBodies.crests) {
            const double rise = body.tip.point.y() - crest.point.y();
            const double lean = std::abs(body.tip.point.x() - crest.point.x());
            if (same(crest.point.y(), bounds.top()) && entrySide.isStrictLeft(crest.time) &&
                rise >= bodySpec.value("minRise").toDouble() &&
                lean <= bodySpec.value("maxWidthToHeight").toDouble() * rise &&
                body.tip.point.x() >= bounds.left() && body.tip.point.x() <= bounds.right())
                score = std::min(score, lean / rise);
        }
        if (std::isfinite(score)) candidates.emplace_back(score, &body);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto apply = [&](const Stretching& cut) {
        if (!ordered(incoming, cut) || (hasExit && !ordered(cut, joins[slot]))) return false;
        const auto cap = chord(cut);
        if (!old.kashidaPaths[slot - 1].isEmpty() && old.kashidaCuts[slot - 1].second == cap) return true;
        auto cuts = joins;
        auto& entry = cuts[slot - 1];
        entry.topLeft = cut.topBaseLine;
        entry.bottomLeft = cut.bottomBaseLine;
        entry.fixedRightBoundary = entry.fixedLeftBoundary = true;
        entry.length = 0;
        entry.kashidaOverrides.insert("enabled", true);
        sub.joins.clear();
        for (size_t j = 0; j < cuts.size(); ++j) {
            cuts[j].readingOrder = int(j);
            sub.joins.insert(cuts[j]);
        }
        bool valid = assembleBodyPartition(word, sub, config);
        // A detached alef has no chord in the connected partition.
        if (valid && old.baseGlyphs.size() == joins.size() + 2 && sub.baseGlyphs.size() == joins.size() + 1)
            sub.baseGlyphs.push_back(old.baseGlyphs.back());
        valid = valid && sub.baseGlyphs.size() == old.baseGlyphs.size() &&
            sub.kashidaPaths.size() == old.kashidaPaths.size() &&
            sub.kashidaCuts[slot - 1] == std::make_pair(chord(incoming), cap);
        if (valid) {
            const auto newBounds = mapped(sub.baseGlyphs[slot].path).boundingRect();
            // Entry policy must leave the shaft, free tail and return intact.
            valid = same(newBounds.top(), bounds.top()) && same(newBounds.bottom(), bounds.bottom()) &&
                same(newBounds.left(), bounds.left()) &&
                toSubpaths(sub.baseGlyphs[slot].path).size() == toSubpaths(old.baseGlyphs[slot].path).size();
        }
        for (size_t j = 0; valid && j < old.baseGlyphs.size(); ++j)
            if (j != size_t(slot)) sub.baseGlyphs[j].path = old.baseGlyphs[j].path;
        for (size_t j = 0; valid && j < old.kashidaPaths.size(); ++j)
            if (j != size_t(slot - 1)) {
                valid = sub.kashidaCuts[j] == old.kashidaCuts[j];
                if (valid) sub.kashidaPaths[j] = old.kashidaPaths[j];
            }
        if (valid && coversOriginalInk(word, sub)) return true;
        sub = old;
        return false;
    };
    // Prefer the full configured walk. If an adjacent attachment leaves less
    // room, use the normal at the same intrinsic tip without moving its owner.
    for (double distance : {policy.value("forkOffset").toDouble(), 0.}) {
        if (distance == 0 && policy.value("tightJoin") != "tip-normal") continue;
        for (const auto& [score, body] : candidates) {
            const auto cut = cutLamForkFlank(context, body->tip, std::nullopt, body->rightForward, 1, distance);
            if (!cut || !apply(*cut)) continue;
            auto diagnostic = sub.letterDiagnostics[slot].toObject();
            diagnostic.insert("bodyForm", hasExit ? "lam-terminal-return" : "lam-terminal-bowl");
            diagnostic.insert("entryPolicy", "lam-fork-flank");
            diagnostic.insert("entryFork", body->tip.time);
            diagnostic.insert("entryWalk", distance);
            sub.letterDiagnostics[slot] = diagnostic;
            sub.templateTrace.append("lam · incoming cut from its own fork; terminal return retained");
            return true;
        }
    }
    return false;
}

// Phase 3: choose compatible attachment policies, then assemble their masks.
// The source body inventory is immutable; there is no recognition or repair
// of a published letter here. Other forms currently arrive through the legacy
// proposal adapter and can migrate to intrinsic body records one at a time.
static bool resolveSubwordCutPlan(const Word& word, const SubwordBodyRecognition& recognition,
                                  SubwordCutPlan& plan, const QJsonObject& config) {
    auto& sub = plan.partition;
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 2 || plan.bodies.empty() ||
        sub.letterDiagnostics.size() != letters.size() || sub.baseGlyphs.size() != size_t(letters.size())) return true;
    // A lam-alef has one shared interface. The lam body includes the return
    // below its stem; it has no independent outgoing attachment to shorten.
    // Place its interface at the intrinsic arm attachment when that policy is
    // enabled. Recognition by a different route must not bypass the junction.
    // Otherwise reuse the proposed boundary. Initial lam-alef uses this too.
    const bool detachedFinal = sub.letterDiagnostics.last().toObject().value("boundaryKind") == "separate-contours";
    const int connectedCount = letters.size() - int(detachedFinal);
    const auto returnBody = std::find_if(plan.bodies.begin(), plan.bodies.end(), [](const auto& body) {
        return body.attachment == BodyAttachmentPolicy::SharedAlefReturn ||
            body.attachment == BodyAttachmentPolicy::SeparateAlefReturn;
    });
    if (returnBody != plan.bodies.end()) {
        InspectFormScope inspection(returnBody->formId);
        const auto* recognized = recognizedLamBody(recognition, returnBody->body);
        if (!recognized) return true;
        const auto& attachment = recognized->alefAttachment;
        if (attachment) {
            inspectLandmark("Alef junction", attachment->first);
            inspectLandmark("Alef junction", attachment->second);
        }
        const auto& lam = returnBody->cuts;
        const int terminalLam = returnBody->letterIndex;
        const auto old = sub;
        const bool separate = returnBody->attachment == BodyAttachmentPolicy::SeparateAlefReturn;
        bool junction = !separate && lam.value("junction") == "inward-corners" &&
            attachment.has_value();
        if (junction && sub.joins.size() > 1) {
            Stretching seamCandidate{};
            seamCandidate.topBaseLine = attachment->first;
            seamCandidate.bottomBaseLine = attachment->second;
            auto entry = *std::prev(sub.joins.end(), 2);
            entry.topBaseLine = entry.topLeft;
            entry.bottomBaseLine = entry.bottomLeft;
            const auto incoming = getJoinTime(entry, sub.basePathOrientation);
            const auto terminal = getJoinTime(seamCandidate, sub.basePathOrientation);
            // Shape evidence can also describe a preceding diagonal (e.g. kaf).
            // Both interfaces must preserve reading order, not enclose one another.
            junction = incoming.isLeft(seamCandidate.topBaseLine.time) && incoming.isLeft(seamCandidate.bottomBaseLine.time) &&
                terminal.isRight(entry.topBaseLine.time) && terminal.isRight(entry.bottomBaseLine.time);
        }
        const auto sameLandmark = [](const PathExtrema& a, const PathExtrema& b) {
            return a.pathIndex == b.pathIndex && std::abs(a.time - b.time) < 1e-8;
        };
        const bool moveJunction = junction && !sub.joins.empty() &&
            !(sameLandmark(sub.joins.rbegin()->topLeft, attachment->first) &&
              sameLandmark(sub.joins.rbegin()->bottomLeft, attachment->second));
        // A body already ending at these landmarks needs no new assembly.
        // In particular, keep exact recovered contours and earlier connectors.
        if (!separate && sub.joins.size() + 1 == sub.baseGlyphs.size() &&
            sub.kashidaPaths.size() == sub.joins.size() &&
            (moveJunction || !sub.kashidaPaths.back().isEmpty())) {
            auto seam = *sub.joins.rbegin();
            seam.topBaseLine = seam.topRight = seam.topLeft = junction ?
                attachment->first : seam.topLeft;
            seam.bottomBaseLine = seam.bottomRight = seam.bottomLeft = junction ?
                attachment->second : seam.bottomLeft;
            seam.fixedRightBoundary = true;
            seam.fixedLeftBoundary = false;
            seam.length = 0;
            seam.kashidaOverrides.insert("enabled", false);
            sub.joins.erase(std::prev(sub.joins.end()));
            sub.joins.insert(seam);
            bool valid = assembleBodyPartition(word, sub, config) &&
                sub.baseGlyphs.size() == old.baseGlyphs.size() &&
                sub.kashidaPaths.size() == old.kashidaPaths.size() && sub.kashidaPaths.back().isEmpty();
            for (size_t j = 0; valid && j < old.baseGlyphs.size(); ++j)
                if (j != size_t(terminalLam) && (!junction || j + 1 < old.baseGlyphs.size()))
                    valid = sub.baseGlyphs[j].path == old.baseGlyphs[j].path;
            for (size_t j = 0; valid && j + 1 < old.kashidaPaths.size(); ++j)
                valid = sub.kashidaPaths[j] == old.kashidaPaths[j] && sub.kashidaCuts[j] == old.kashidaCuts[j];
            if (!valid || !coversOriginalInk(word, sub)) {
                sub = old;
                sub.correct = false;
                auto diagnostic = sub.letterDiagnostics[terminalLam].toObject();
                diagnostic.insert("status", "no-match");
                sub.letterDiagnostics[terminalLam] = diagnostic;
                sub.templateTrace.append("lam-alef · shared-return policy failed ink ownership");
                return false;
            }
        }
        auto diagnostic = sub.letterDiagnostics[terminalLam].toObject();
        diagnostic.insert("bodyForm", "lam-terminal-return");
        diagnostic.insert("cutPolicy", separate ? "separate-contours" : "lam-alef-shared");
        if (junction) diagnostic.insert("junctionPolicy", "inward-corners");
        diagnostic.remove("boundaryKind");
        sub.letterDiagnostics[terminalLam] = diagnostic;
        sub.templateTrace.append(separate ? "lam-alef · return retained, separate alef outline" :
            junction ? "lam-alef · shared boundary at intrinsic attachment corners, lower return retained" :
            "lam-alef · lower return retained, shared boundary without kashida");
    }
    if (connectedCount < 2 || sub.joins.size() + 1 != size_t(connectedCount) ||
        sub.kashidaPaths.size() != sub.joins.size()) return true;
    const auto shape = word.bodyShape(sub);
    const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    if (!recognition.valid) return true;
    SearchContext context{.transformedPath = recognition.source, .subword = sub};
    context.transformedPath.setFillRule(Qt::WindingFill);
    context.bbox = context.transformedPath.boundingRect();
    context.subPaths = recognition.contours;
    for (const auto& point : recognition.landmarks) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }
    context.orientation = sub.basePathOrientation;
    const auto before = [&](const Stretching& a, const Stretching& b) {
        return cutPrecedes(a, b, context.orientation);
    };
    const auto chord = [](const Stretching& cut) {
        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
    };

    for (const auto& bodyRequest : plan.bodies) {
        InspectFormScope inspection(bodyRequest.formId);
        if (bodyRequest.attachment == BodyAttachmentPolicy::SharedAlefReturn ||
            bodyRequest.attachment == BodyAttachmentPolicy::SeparateAlefReturn) continue;
        const int i = bodyRequest.letterIndex;
        const auto& lam = bodyRequest.cuts;
        const auto& lamBody = bodyRequest.body;
        const auto* recognized = recognizedLamBody(recognition, lamBody);
        const double offset = lam.value("forkOffset").toDouble();
        if (!recognized || offset <= 0) continue;
        if (inspectingForm) for (const auto& crest : recognized->lamBodies.crests) inspectLandmark("Stem crest", crest);
        if (inspectingForm) for (const auto& fork : recognized->lamBodies.forks)
            if (fork.isConcave(context.orientation) && (fork.fork || fork.folded())) inspectLandmark("Fork", fork.tip);
        if (bodyRequest.attachment == BodyAttachmentPolicy::ForkEntry) {
            if (i > 0 && i < connectedCount)
                resolveLamForkEntry(word, *recognized, context, sub, i, lamBody, lam, config);
            continue;
        }
        if (i < 1 || i + 1 >= connectedCount) continue;
        const auto diagnostic = sub.letterDiagnostics[i].toObject();
        const auto form = diagnostic.value("template").toString();
        // The adjacent legacy proposals constrain attachment compatibility,
        // but do not supply the fork or the two intrinsic flank directions.
        const auto old = sub;
        const auto joins = std::vector<Stretching>(old.joins.begin(), old.joins.end());
        const auto& previous = joins[i - 1];
        const auto& exit = joins[i];
        const bool sharedCounter = previous.counterEntry.pathIndex > 0;
        if (exit.counterEntry.pathIndex > 0 || (sharedCounter &&
            (lam.value("counterJoin") != "preserve-entry" ||
             previous.counterEntry.pathIndex != previous.counterExit.pathIndex ||
             !old.kashidaPaths[i - 1].isEmpty()))) continue;
        auto next = exit;
        if (!old.kashidaPaths[i].isEmpty()) {
            next.topBaseLine = exit.topLeft;
            next.bottomBaseLine = exit.bottomLeft;
        }
        const auto previousCap = chord(previous), nextCap = chord(next);
        auto existingRight = previous;
        if (!old.kashidaPaths[i - 1].isEmpty()) {
            existingRight.topBaseLine = previous.topLeft;
            existingRight.bottomBaseLine = previous.bottomLeft;
        }
        existingRight.referenceWidth = chord(existingRight).length();
        auto existingLeft = exit;
        existingLeft.referenceWidth = chord(existingLeft).length();
        const double stroke = std::min(sharedCounter ? previous.referenceWidth : previousCap.length(), nextCap.length());
        if (stroke <= 0) continue;
        const auto bounds = mapped(old.baseGlyphs[i].path).boundingRect();
        const auto apply = [&](const Stretching& right, const Stretching& left,
                               const Stretching& sending, const Stretching& receiving, bool moveExit = true) {
            const auto sendingCap = chord(sending), receivingCap = chord(receiving);
            if (!sharedCounter && old.kashidaCuts[i - 1] == std::make_pair(sendingCap, chord(right)) &&
                old.kashidaCuts[i] == std::make_pair(chord(left), receivingCap)) return true;
            const bool adjustedSending = !sharedCounter && sendingCap != previousCap;
            const bool adjustedReceiving = receivingCap != nextCap;
            if ((!sharedCounter && !before(sending, right)) || !before(right, left) || !before(left, receiving) ||
                std::min(right.referenceWidth, left.referenceWidth) <= 1e-6) return false;
            auto cuts = joins;
            if (adjustedSending && !sharedCounter) {
                cuts[i - 1].topBaseLine = cuts[i - 1].topRight = sending.topBaseLine;
                cuts[i - 1].bottomBaseLine = cuts[i - 1].bottomRight = sending.bottomBaseLine;
            }
            cuts[i - 1].topLeft = right.topBaseLine;
            cuts[i - 1].bottomLeft = right.bottomBaseLine;
            if (moveExit) {
                cuts[i].topBaseLine = left.topBaseLine;
                cuts[i].bottomBaseLine = left.bottomBaseLine;
                cuts[i].topRight = left.topBaseLine;
                cuts[i].bottomRight = left.bottomBaseLine;
                cuts[i].topLeft = receiving.topBaseLine;
                cuts[i].bottomLeft = receiving.bottomBaseLine;
                cuts[i].referenceWidth = left.referenceWidth;
            }
            for (int index = i - 1; index < i + int(moveExit); ++index) {
                cuts[index].fixedRightBoundary = cuts[index].fixedLeftBoundary = true;
                cuts[index].length = 0;
                cuts[index].kashidaOverrides.insert("enabled", true);
            }
            if (sharedCounter) {
                // Both bridges belong to the shared ligature interface.
                // Preserve that entire entry and shorten only the lam exit.
                cuts[i - 1] = previous;
            }
            sub.joins.clear();
            for (size_t index = 0; index < cuts.size(); ++index) {
                cuts[index].readingOrder = int(index);
                sub.joins.insert(cuts[index]);
            }
            const bool recut = assembleBodyPartition(word, sub, config);
            // A detached final outline has no join. Recut only the connected
            // bodies and preserve that independently recognized outline.
            if (recut && detachedFinal && sub.baseGlyphs.size() == size_t(connectedCount))
                sub.baseGlyphs.push_back(old.baseGlyphs.back());
            bool valid = recut &&
                sub.baseGlyphs.size() == old.baseGlyphs.size() &&
                (sharedCounter ? sub.kashidaPaths[i - 1].isEmpty() : !sub.kashidaPaths[i - 1].isEmpty()) &&
                !sub.kashidaPaths[i].isEmpty() &&
                (sharedCounter || sub.kashidaCuts[i - 1] == std::make_pair(sendingCap, chord(right))) &&
                sub.kashidaCuts[i] == std::make_pair(chord(left), receivingCap) &&
                (moveExit || sub.kashidaPaths[i] == old.kashidaPaths[i]);
            if (valid) {
                const auto body = mapped(sub.baseGlyphs[i].path);
                const auto box = body.boundingRect();
                valid = std::abs(box.top() - bounds.top()) < 1e-5 &&
                    box.height() >= lamBody.value("minRise").toDouble() * stroke &&
                    box.width() <= lamBody.value("maxWidthToHeight").toDouble() * box.height() &&
                    toSubpaths(body).size() == toSubpaths(old.baseGlyphs[i].path).size();
            }
            for (size_t j = 0; valid && j < old.baseGlyphs.size(); ++j) {
                if (j == size_t(i)) continue;
                const auto& oldBody = old.baseGlyphs[j].path;
                const auto& newBody = sub.baseGlyphs[j].path;
                const bool sendingBody = adjustedSending && j + 1 == size_t(i);
                const bool receivingBody = adjustedReceiving && j == size_t(i + 1);
                if (!sendingBody && !receivingBody) {
                    // Their defining cuts did not move. Retain the recognized
                    // bodies exactly, avoiding Bezier roundoff from rebuilding
                    // the complete partition in a different cut order.
                    sub.baseGlyphs[j].path = oldBody;
                    continue;
                }
                const auto oldContours = toSubpaths(oldBody), newContours = toSubpaths(newBody);
                const auto oldBox = mapped(oldBody).boundingRect(), newBox = mapped(newBody).boundingRect();
                const auto oldCap = sendingBody ? previousCap : nextCap;
                const auto cap = sendingBody ? sendingCap : receivingCap;
                // Preserve counters and the body extrema away from the seam.
                // An extreme lying on a cap can move with that cap.
                const auto same = [](double a, double b) { return std::abs(a - b) < .05; };
                valid = !oldContours.empty() && oldContours.size() == newContours.size() &&
                    std::equal(std::next(oldContours.begin()), oldContours.end(), std::next(newContours.begin())) &&
                    (sendingBody ? same(oldBox.right(), newBox.right()) : same(oldBox.left(), newBox.left())) &&
                    (same(oldBox.top(), newBox.top()) ||
                     (same(oldBox.top(), std::min(oldCap.y1(), oldCap.y2())) &&
                      newBox.top() >= oldBox.top() - .05 &&
                      newBox.top() <= std::min(cap.y1(), cap.y2()) + .05)) &&
                    (same(oldBox.bottom(), newBox.bottom()) ||
                     (same(oldBox.bottom(), std::max(oldCap.y1(), oldCap.y2())) &&
                      newBox.bottom() <= oldBox.bottom() + .05 &&
                      newBox.bottom() >= std::max(cap.y1(), cap.y2()) - .05));
            }
            for (size_t j = 0; valid && j < old.kashidaPaths.size(); ++j)
                if (j != size_t(i - 1) && j != size_t(i)) {
                    valid = sub.kashidaCuts[j] == old.kashidaCuts[j];
                    if (valid) sub.kashidaPaths[j] = old.kashidaPaths[j];
                }
            if (valid && coversOriginalInk(word, sub)) return true;
            sub = old;
            return false;
        };

        // The contour walks locate both cuts intrinsically. Keep the width
        // safeguard for fallback cuts that lack one or both walked anchors.
        const auto applyFallback = [&](const Stretching& right, const Stretching& left, bool moveExit = true) {
            return right.referenceWidth <= 2.5 * previousCap.length() &&
                left.referenceWidth <= 2.5 * nextCap.length() &&
                apply(right, left, previous, next, moveExit);
        };
        const bool sharedTip = !old.kashidaPaths[i - 1].isEmpty() && !old.kashidaPaths[i].isEmpty() &&
            old.kashidaCuts[i - 1].second.p2() == old.kashidaCuts[i].first.p2();
        const auto placeHalfWalk = [&](const Stretching& right, const Stretching& left,
                                       const PathExtrema& tip, bool rightForward) {
            if (apply(right, left, previous, next)) return true;
            // A tight attachment may already stop closer than half a unit to
            // the fork. Keep that valid boundary while shortening the other
            // side, rather than rejecting both independent proposals.
            const auto retainNearBoundary = [&] {
                return (lamAnchorWithinWalk(context, tip, existingRight, rightForward, offset) &&
                        apply(existingRight, left, previous, next)) ||
                    (lamAnchorWithinWalk(context, tip, existingLeft, !rightForward, offset) &&
                        apply(right, existingLeft, previous, next));
            };
            const bool sending = !before(previous, right);
            const bool receiving = !before(left, next);
            if (!sharedTip || sending == receiving || !before(right, left)) return retainNearBoundary();

            // The half-unit anchors are fixed. If a provisional adjacent cap
            // crosses one of them, try a nearby section of that same join.
            const auto oldCap = sending ? previousCap : nextCap;
            const auto flank = chord(sending ? right : left);
            const auto accept = [&](const Stretching& seam) {
                const auto cap = chord(seam);
                if ((sending ? !before(seam, right) : !before(left, seam)) ||
                    QLineF(cap.center(), oldCap.center()).length() > oldCap.length() / 2 ||
                    QLineF(cap.center(), flank.center()).length() < offset / 3) return false;
                return sending ? apply(right, left, seam, next) : apply(right, left, previous, seam);
            };
            // Prefer the final body's own loop shoulder when its template has one.
            if (receiving && i + 2 == letters.size()) {
                for (const auto& value : config.value("final").toArray()) {
                    const auto rule = bodyConstraintForm(config, value.toObject());
                    if (!rule.contains("shoulder") || !rule.value("current").toString().contains(letters.back())) continue;
                    Stretching seam{};
                    bool projected = false;
                    if (rule.value("body").toObject().value("shape") == "counter-shoulder") {
                        const auto body = recognizedSharedShape(context, rule.value("body").toObject());
                        projected = body->terminalContour && cutLoopShoulder(context, body->terminalContour->box,
                            body->shoulders, shapeSettings(rule, "shoulder"), std::prev(context.xSorted.end())->time, true, seam);
                    } else projected = terminalLoopShoulder(context, rule, seam);
                    if (!projected || !accept(seam)) continue;
                    auto diagnostic = sub.letterDiagnostics.last().toObject();
                    diagnostic.insert("template", rule.value("id"));
                    sub.letterDiagnostics[sub.letterDiagnostics.size() - 1] = diagnostic;
                    sub.templateTrace.append(rule.value("id").toString() + " · neighboring cut restored at its shoulder");
                    return true;
                }
            }
            const bool forward = sending ? rightForward : !rightForward;
            const int side = sending ? 1 : -1;
            for (double distance = 1.5 * offset; distance <= offset + oldCap.length(); distance += offset / 4) {
                const auto seam = cutLamForkFlank(context, tip, std::nullopt, forward, side, distance);
                if (seam && accept(*seam)) return true;
            }
            return retainNearBoundary();
        };

        // Assign the pre-recognized body to this shaft/slot. A thin adjacent
        // connector does not limit the height of its intrinsic fork geometry.
        const auto& recognizedBodies = recognized->lamBodies.forks;
        const auto rightFlank = [&](const PathExtrema& tip, std::optional<PathExtrema> end,
                                    bool forward, double distance) -> std::optional<Stretching> {
            if (sharedCounter) return previous;
            return cutLamForkFlank(context, tip, end, forward, 1, distance);
        };
        const auto refine = [&] {
            std::vector<std::pair<Stretching, Stretching>> tipFallbacks;
            std::vector<Stretching> foldedEntries;
            bool refined = false;
            for (const auto& body : recognizedBodies) {
                if (refined) break;
                if (!body.isConcave(context.orientation)) continue;
                const auto* tip = &body.tip;
                if (tip->point.y() - bounds.top() < lamBody.value("minRise").toDouble() * stroke ||
                    tip->point.y() > bounds.bottom() + stroke ||
                    tip->point.x() < bounds.left() - stroke || tip->point.x() > bounds.right() + stroke) continue;
                if (!getJoinTime(previous, context.orientation).isStrictLeft(tip->time) ||
                    !getJoinTime(next, context.orientation).isStrictRight(tip->time)) continue;
                const bool fork = body.fork;
                const bool folded = body.folded();
                if (!fork && !folded) continue;
                const bool rightForward = body.rightForward;
                auto right = rightFlank(*tip, std::nullopt, rightForward, offset);
                auto left = cutLamForkFlank(context, *tip, std::nullopt, !rightForward, -1, offset);
                if (!right && lamAnchorWithinWalk(context, *tip, existingRight, rightForward, offset)) right = existingRight;
                if (!left && lamAnchorWithinWalk(context, *tip, existingLeft, !rightForward, offset)) left = existingLeft;
                if (right && left && (refined = placeHalfWalk(*right, *left, *tip, rightForward))) break;
                const auto tipRight = rightFlank(*tip, previous.bottomBaseLine, rightForward, 0);
                const auto tipLeft = cutLamForkFlank(context, *tip, next.bottomBaseLine, !rightForward, -1, 0);
                if (tipRight && tipLeft) tipFallbacks.emplace_back(*tipRight, *tipLeft);
                if (folded && right && std::abs(chord(exit).center().x() - tip->point.x()) <= 2 * stroke &&
                    old.kashidaCuts[i - 1].second.center().x() - tip->point.x() >= 2.5 * stroke)
                    foldedEntries.push_back(*right);
            }
            if (!refined && lam.value("tightJoin") == "tip-normal") for (const auto& [right, left] : tipFallbacks)
                if ((refined = applyFallback(right, left))) break;
            if (!refined) {
                // A folded foot can already have a local exit even if its left
                // normal cannot make a valid cut.
                auto left = exit;
                left.referenceWidth = chord(exit).length();
                for (const auto& right : foldedEntries)
                    if ((refined = applyFallback(right, left, false))) break;
            }
            return refined;
        };
        const bool refined = refine();
        if (refined) {
            auto diagnostic = sub.letterDiagnostics[i].toObject();
            diagnostic.insert("bodyForm", "lam-fork");
            diagnostic.insert("cutPolicy", sharedCounter ? "lam-fork-preserved-entry" : "lam-fork-flanks");
            sub.letterDiagnostics[i] = diagnostic;
            sub.templateTrace.append(form + " · fork cut policy applied");
        }
    }
    return true;
}

bool PageAnalysisResult::segmentSubword(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                        bool refresh, const QJsonObject* draft) {
    resolveCompoundBody(pageIndex, lineIndex, wordIndex);
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!refresh && !sub.baseGlyphs.empty())
        return sub.correct;
    auto requested = draft ? *draft : letterForms();
    auto recognition = recognizeSubwordBodies(word, sub, requested);

    // The shared collector combines declared lām candidates with the existing
    // matchers for other letters. Lām never enters their method dispatcher.
    // Resolve the declared policies before committing the partition.
    const bool proposed = collectLegacyCutProposals(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested);
    auto plan = makeSubwordCutPlan(std::move(sub), requested);
    bool valid = proposed;
    if (valid && pageIndex >= 0 && plan.partition.baseGlyphs.size() >= 2 && !plan.partition.provisional()) {
        // A recovery can select a different original contour (e.g. a detached
        // arm). Recognize that source, never a generated letter mask.
        if (recognition.valid) {
            const auto source = word.bodyShape(plan.partition);
            if (recognition.source != source.transform.map(source.path).translated(source.pos))
                recognition = recognizeSubwordBodies(word, plan.partition, requested);
        }
        valid = resolveSubwordCutPlan(word, recognition, plan, requested);
    }
    sub = std::move(plan.partition);
    return valid;
}

bool PageAnalysisResult::collectLegacyCutProposals(int pageIndex, int lineIndex, int wordIndex,
        int subWordIndex, AFont* font, QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested)) {
        const auto letters = bodyLetters(sub.text);
        if (sub.provisional() || !medialLamShortExit(requested).value("retryOnUnresolved").toBool() ||
            letters.size() < 3 || !QStringView(letters).mid(1, letters.size() - 2).contains(QChar(0x0644)))
            return false;

        // Reassemble the whole subword: a shorter exit can invalidate a neighbouring cut.
        auto partial = sub;
        requested.insert("_disableLamShortExit", true);
        if (!segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested) ||
            sub.provisional() || !coversOriginalInk(word, sub)) {
            sub = std::move(partial);
            return false;
        }
        sub.templateTrace.append("Complete partition recovered with the original lam exit");
    }
    if (pageIndex < 0 || sub.baseGlyphs.size() < 2 || sub.provisional()) return true;
    const auto refine = [&] {
        refineLipCuts(lineIndex, wordIndex, subWordIndex, requested);
        refineBodyOwnership(lineIndex, wordIndex, subWordIndex, requested);
        if (sub.joins.empty() || sub.kashidaPaths.empty() || sub.kashidaPaths.back().isEmpty() ||
            sub.letterDiagnostics.isEmpty()) return;
        const auto last = std::prev(sub.joins.end());
        if (!last->fixedLeftBoundary) return;
        const auto id = sub.letterDiagnostics.last().toObject().value("template");
        const auto finals = requested.value("final").toArray();
        const auto found = std::find_if(finals.begin(), finals.end(), [&](const auto& item) {
            const auto rule = item.toObject();
            return rule.value("id") == id && rule.value("retainEntryCrest").toBool();
        });
        if (found == finals.end()) return;
        const auto shape = word.bodyShape(sub);
        SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                              .subword = sub};
        if (!readTerminalOutline(context)) return;
        auto seam = *last;
        seam.topBaseLine = last->topLeft;
        seam.bottomBaseLine = last->bottomLeft;
        const auto boundary = cutAfterEntryCrest(context, seam);
        if (!boundary || !matchesBoundaryBody(context, found->toObject(), *boundary, false)) return;

        // The terminal boundary may move only inside its existing connector.
        // Recut from the same source; keep the other boundary and every prior mask.
        const auto previous = sub;
        auto join = *last;
        join.topLeft = boundary->topBaseLine;
        join.bottomLeft = boundary->bottomBaseLine;
        sub.joins.erase(last);
        sub.joins.insert(join);
        bool valid = cutSubword(lineIndex, wordIndex, subWordIndex, requested) &&
            sub.baseGlyphs.size() == previous.baseGlyphs.size() &&
            sub.kashidaPaths.size() == previous.kashidaPaths.size() &&
            !sub.kashidaPaths.back().isEmpty() &&
            sub.kashidaCuts.back().first == previous.kashidaCuts.back().first &&
            sub.kashidaCuts.back().second != previous.kashidaCuts.back().second;
        for (size_t i = 0; valid && i + 1 < sub.baseGlyphs.size(); ++i)
            valid = sub.baseGlyphs[i].path == previous.baseGlyphs[i].path;
        for (size_t i = 0; valid && i + 1 < sub.kashidaPaths.size(); ++i)
            valid = sub.kashidaPaths[i] == previous.kashidaPaths[i];
        if (!valid || !coversOriginalInk(word, sub)) sub = previous;
        else sub.templateTrace.append(id.toString() + " · raised entrance retained in terminal body");
    };
    const auto letters = bodyLetters(sub.text);
    QJsonObject checks, connectorLips;
    for (const auto& entry : requested.value("recoveryChecks").toObject().value("bodyExtents").toArray()) {
        const auto spec = entry.toObject();
        if (!spec.value("protectHead").toBool()) continue;
        const auto chars = spec.value("letters").toString();
        if (spec.value("position") == "initial" && chars.contains(letters.front()))
            checks.insert("_initialHeadLips", spec.value("lips"));
        if (spec.value("position") == "medial")
            for (QChar ch : chars)
                if (QStringView(letters).mid(1, letters.size() - 2).contains(ch))
                    connectorLips.insert(QString(ch), spec.value("lips"));
    }
    if (!connectorLips.isEmpty()) checks.insert("_connectorHeadLips", connectorLips);
    const auto missing = missingHeadInk(word, sub, letters, checks);
    if (missing.empty()) {
        refine();
        return true;
    }

    auto repair = requested;
    for (auto it = checks.begin(); it != checks.end(); ++it) repair.insert(it.key(), it.value());
    QJsonObject evidence;
    for (const auto& [slot, lips] : missing)
        if (!lips.isNull()) evidence.insert(QString::number(slot), QJsonArray{lips.x1(), lips.y1(), lips.x2(), lips.y2()});
    repair.insert("_observedHeadInk", evidence);
    if (!segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, repair)) return false;
    auto failed = missingHeadInk(word, sub, letters, checks);
    const auto shape = word.bodyShape(sub);
    for (const auto& [slot, lips] : missing) {
        if (lips.isNull()) continue;
        const auto body = shape.transform.map(sub.baseGlyphs[slot].path).translated(shape.pos);
        if (!body.contains(lips.p1()) || !body.contains(lips.p2())) failed.emplace(slot, lips);
    }
    if (failed.empty() && coversOriginalInk(word, sub)) {
        refine();
        return true;
    }
    sub.correct = false;
    sub.templateTrace.append("Head ownership could not be verified after recutting");
    for (const auto& [slot, _] : failed) {
        auto diagnosis = sub.letterDiagnostics[slot].toObject();
        diagnosis.insert("status", "no-match");
        sub.letterDiagnostics[slot] = diagnosis;
    }
    return false;
}

bool PageAnalysisResult::segmentWithRecovery(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                            const QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    if (!requested.contains("retryUnresolvedForms"))
        return segmentConfigured(pageIndex, lineIndex, wordIndex, subWordIndex, font, requested);

    // Preserve an established cut or provisional remainder. Only an unresolved
    // body enters the wider contour search.
    static thread_local QJsonObject lastRequested, usual, extended;
    if (lastRequested != requested) {
        lastRequested = requested;
        usual = requested;
        usual.remove("retryUnresolvedForms");
        extended = usual;
        auto recovery = extended.value("recoveryChecks").toObject();
        auto baseChecks = recovery;
        QJsonArray baseExtents;
        for (const auto& item : recovery.value("bodyExtents").toArray()) {
            if (!item.toObject().value("onUnresolved").toBool()) baseExtents.append(item);
        }
        baseChecks.insert("bodyExtents", baseExtents);
        usual.insert("recoveryChecks", baseChecks);
        recovery.insert("initialShaft", requested.value("unresolvedInitialShaft"));
        extended.insert("recoveryChecks", recovery);
        auto scales = extended.value("reconsiderScale").toObject();
        scales.insert("alternateScales", requested.value("unresolvedScaleCenters"));
        scales.insert("tilts", requested.value("unresolvedTiltAngles"));
        extended.insert("reconsiderScale", scales);
        QJsonObject selectors, headRetries;
        for (const auto* position : {"initial", "medial", "final"}) {
            QJsonArray original, searchable, headForms;
            QString characters;
            for (const auto& value : requested.value(position).toArray()) {
                const auto form = value.toObject();
                if (form.value("onHeadConflict").toBool()) headForms.append(form);
                if (!hasCounterOutline(form)) {
                    auto retry = form;
                    if (form.contains("recoveryVisibleFrom"))
                        retry.insert("visibleFrom", form.value("recoveryVisibleFrom"));
                    searchable.append(retry);
                }
                if (!form.value("onUnresolved").toBool())
                    original.append(form);
                for (auto ch : form.value("current").toString())
                    if (!characters.contains(ch))
                        characters.append(ch);
            }
            usual.insert(position, original);
            extended.insert(position, searchable);
            selectors.insert(position, characters);
            if (!headForms.isEmpty()) headRetries.insert(position, headForms);
        }
        usual.insert("_headRetryForms", headRetries);
        extended.insert("_headRetryForms", headRetries);
        extended.insert("reconsiderForms", selectors);
        extended.insert("reconsiderFinalAttachments", selectors.value("final"));
    }
    const auto run = [&](const QJsonObject& forms) {
        return segmentConfigured(pageIndex, lineIndex, wordIndex, subWordIndex, font, forms);
    };
    if (run(usual)) {
        // An accepted generic walk can cut through the folded bowl itself.
        // Keep an established result unless the independently found bowl
        // reaches beyond the final mask by a substantial part of the join.
        if (sub.baseGlyphs.size() < 2 || sub.provisional()) return true;
        const auto letters = bodyLetters(sub.text);
        auto finals = requested.value("final").toArray();
        for (qsizetype i = 0; i < finals.size(); ++i) {
            auto form = finals.at(i).toObject();
            if (form.value("method") != "folded-bowl" || !form.value("onUnresolved").toBool() ||
                !form.value("current").toString().contains(letters.back())) continue;
            const auto shape = word.bodyShape(sub);
            const auto mapped = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
            SearchContext outline{.transformedPath = mapped(shape.path), .subword = sub};
            Stretching seam{};
            double shoulderX = 0;
            if (!readTerminalOutline(outline) || !foldedBowlEntry(outline, form.value("fold").toObject(), seam, &shoulderX))
                break;
            const double width = QLineF(seam.topBaseLine.point, seam.bottomBaseLine.point).length();
            const auto retainsBowl = [&](double margin) {
                const auto bounds = mapped(sub.baseGlyphs.back().path).boundingRect();
                return bounds.left() <= outline.bbox.left() + margin && bounds.right() >= shoulderX - margin;
            };
            if (retainsBowl(.5 * width)) break;
            auto original = sub;
            auto retry = requested;
            form.remove("onUnresolved");
            finals.replace(i, form);
            retry.insert("final", finals);
            if (segmentWithRecovery(pageIndex, lineIndex, wordIndex, subWordIndex, font, retry) &&
                sub.baseGlyphs.size() == original.baseGlyphs.size() && retainsBowl(.05 * width) &&
                coversOriginalInk(word, sub)) {
                sub.templateTrace.append("Terminal bowl recovered at its first fold");
            } else sub = std::move(original);
            break;
        }
        return true;
    }
    if (!requested.value("retryUnresolvedForms").toBool() || sub.provisional() || bodyLetters(sub.text).size() < 2 || sub.paths.empty())
        return false;
    auto original = sub;
    if (run(extended) && !sub.provisional() &&
        plausibleRecoveredBodies(word, sub, extended.value("recoveryChecks").toObject()) &&
        coversOriginalInk(word, sub)) {
        sub.templateTrace.append(QStringLiteral("Unresolved form matched by contour recovery"));
        return true;
    }
    const auto checks = extended.value("recoveryChecks").toObject();
    if (confirmCounterOutlines(word, sub, requested, checks) || recoverSharedTerminal(word, sub, requested, checks))
        return true;
    sub = std::move(original);
    if (confirmCounterOutlines(word, sub, requested, checks) || recoverSharedTerminal(word, sub, requested, checks) ||
        recoverExposedHead(word, sub, requested, checks)) return true;
    const auto frames = requested.value("unresolvedShapeFrames").toArray();
    if (frames.isEmpty()) return false;
    auto usualWithChecks = usual;
    usualWithChecks.insert("recoveryChecks", checks);
    return stableTemplateCuts(word, sub, usualWithChecks, frames) || stableTemplateCuts(word, sub, extended, frames);
}

bool PageAnalysisResult::segmentConfigured(int pageIndex, int lineIndex, int wordIndex, int subWordIndex, AFont* font,
                                           const QJsonObject& requested) {
    auto& word = page.lines[lineIndex].words[wordIndex];
    auto& sub = word.wordResultInfo.subWords[subWordIndex];
    // Preserve established initial/medial recoveries. A new final attachment
    // gets a vote only when the first unresolved body is an eligible final.
    static thread_local QJsonObject previousConfig, primary, established;
    if (previousConfig != requested) {
        previousConfig = requested;
        primary = established = requested;
        established.remove("reconsiderFinalAttachments");
        for (const auto* position : {"initial", "medial", "final"}) {
            QJsonArray firstPass, usualRecovery;
            for (const auto& value : requested.value(position).toArray()) {
                auto form = value.toObject();
                if (form.value("onFinalRetry").toBool()) continue;
                usualRecovery.append(form);
                if (!form.value("onIncomplete").toBool()) {
                    form.remove("retryWithVariants");
                    firstPass.append(form);
                }
            }
            primary.insert(position, firstPass);
            established.insert(position, usualRecovery);
        }
    }
    if (segmentWithLetterForms(pageIndex, lineIndex, wordIndex, subWordIndex, font, primary, false))
        return true;
    const auto first = std::find_if(sub.letterDiagnostics.constBegin(), sub.letterDiagnostics.constEnd(),
        [](const auto& value) { return value.toObject().value("status") != "matched"; });
    if (first == sub.letterDiagnostics.constEnd()) return false;
    const auto slot = first->toObject();
    const auto position = slot.value("position").toString(), ch = slot.value("character").toString();
    const bool finalRetry = position == "final" && requested.value("reconsiderFinalAttachments").toString().contains(ch);
    // Detach the choice from the thread-local cache before analysing a folded prefix recursively.
    const QJsonObject config = finalRetry ? requested : established;
    if (!finalRetry && !config.value("reconsiderForms").toObject().value(position).toString().contains(ch))
        return false;
    auto previous = sub;
    if (recoverFoldedArm(word, sub, config, false)) {
        if (plausibleRecoveredBodies(word, sub, config.value("recoveryChecks").toObject()) &&
            coversOriginalInk(word, sub))
            return true;
        sub = previous;
    }
    if (segmentWithLetterForms(pageIndex, lineIndex, wordIndex, subWordIndex, font, config, true)) {
        if (coversOriginalInk(word, sub))
            return true;
        sub = std::move(previous);
        sub.templateTrace.append("Recovered boundaries failed the ink ownership check");
        return false;
    }
    // Only retain a partial retry that actually resolved an additional letter
    // while preserving all previously recognized letters.
    int gained = 0;
    for (int i = 0; i < previous.letterDiagnostics.size(); ++i) {
        const bool was = previous.letterDiagnostics[i].toObject().value("status") == "matched";
        const bool now = sub.letterDiagnostics[i].toObject().value("status") == "matched";
        if (was && !now) {
            gained = 0;
            break;
        }
        if (!was && now)
            ++gained;
    }
    auto folded = sub;
    if (recoverFoldedArm(word, folded, config, true) &&
        plausibleRecoveredBodies(word, folded, config.value("recoveryChecks").toObject()) &&
        coversOriginalInk(word, folded)) {
        sub = std::move(folded);
        return true;
    }
    if (detachedHeadEntry(word, sub, config))
        return true;
    const bool everySlotMatched =
        std::all_of(sub.letterDiagnostics.begin(), sub.letterDiagnostics.end(),
                    [](const auto& value) { return value.toObject().value("status") == "matched"; });
    if (!gained || everySlotMatched)
        sub = std::move(previous);
    return stableTemplateCuts(word, sub, config);
}

bool PageAnalysisResult::segmentWithLetterForms(int pageIndex, int lineIndex, int wordIndex, int subWordIndex,
                                                AFont* font, const QJsonObject& active, bool expand) {
    auto passConfig = active;
    OutlineSearchCache outlineCache;
    if (pageIndex >= 0) {
        const auto observed = active.value("_observedHeadInk").toObject();
        for (auto it = observed.begin(); it != observed.end(); ++it) {
            const auto p = it.value().toArray();
            outlineCache.misplacedHeads.emplace(it.key().toInt(), QLineF(p[0].toDouble(), p[1].toDouble(),
                                                                        p[2].toDouble(), p[3].toDouble()));
        }
    }
    const auto& text = page.lines[lineIndex].words[wordIndex].wordResultInfo.subWords[subWordIndex].text;
    const auto letters = bodyLetters(text);
    if (letters.size() >= 2) {
        // All placement retries have the same letter selectors. Keep the original
        // priority and let the cutter test the remaining shape and position rules.
        // Resolve declarations before filtering by transcript position. A final
        // or initial lām may inherit from a medial form even when no medial lām
        // occurs in this subword. The execution plan carries only resolved data.
        const auto& declarations = bodyCutProgram(active);
        const auto select = [&](const char* position, QStringView slotLetters) {
            QJsonArray rules;
            for (const auto& item : active.value(position).toArray()) {
                auto rule = item.toObject();
                const auto selector = rule.value("current");
                const auto characters = selector.toString();
                if (selector.isString() && std::none_of(slotLetters.begin(), slotLetters.end(),
                        [&](QChar letter) { return characters.contains(letter); })) continue;
                const auto declaration = declarations.forms.constFind(rule.value("id").toString());
                if (declaration != declarations.forms.cend()) {
                    rule.insert("body", declaration->body);
                    rule.insert("cuts", declaration->cuts);
                }
                rules.append(rule);
            }
            passConfig.insert(position, rules);
        };
        const QStringView all(letters);
        select("initial", all.left(1));
        select("medial", all.mid(1, all.size() - 2));
        select("final", all.right(1));
    }
    if (expand)
        passConfig.insert("_checkRecoveredCuts", true);
    const auto* draft = &passConfig;
    const bool matched = segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true, draft, false, outlineCache);
    auto& sub = page.lines[lineIndex].words[wordIndex].wordResultInfo.subWords[subWordIndex];
    if (!outlineCache.misplacedHeads.empty()) {
        // Refit an observed head at that same location. A terminal bowl's own fork
        // can also have been inside the same connector as the medial head.
        auto preferred = passConfig;
        const auto retries = active.value("_headRetryForms").toObject();
        for (auto phase = retries.begin(); phase != retries.end(); ++phase) {
            QJsonArray first;
            QStringList promoted;
            for (const auto& item : phase.value().toArray()) {
                auto rule = item.toObject();
                // The observed head was in connecting ink. Its recovery must
                // respect the preceding body's independently matched exit.
                rule.remove("reclaimEntry");
                first.append(rule);
                promoted.append(rule.value("id").toString());
            }
            for (const auto& rule : preferred.value(phase.key()).toArray())
                if (!promoted.contains(rule.toObject().value("id").toString())) first.append(rule);
            preferred.insert(phase.key(), first);
        }
        auto previous = sub;
        if (!retries.isEmpty() && segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true,
                &preferred, false, outlineCache) && coversOriginalInk(page.lines[lineIndex].words[wordIndex], sub)) {
            sub.templateTrace.append("Recovered the letter head from connecting ink");
            return true;
        }
        sub = std::move(previous);
    }
    if (matched) return true;
    const auto& config = draft ? *draft : letterForms();
    const auto forms = config.value("medial").toArray();
    const auto needsRecovery = [&](const QJsonValue& diagnosis) {
        const auto slot = diagnosis.toObject();
        if (slot.value("position") != "medial" || slot.value("status") != "no-match") return false;
        return std::any_of(forms.begin(), forms.end(), [&](const auto& value) {
            const auto form = value.toObject();
            return form.value("recoveryStage").toInt() > 0 &&
                   form.value("current").toString().contains(slot.value("character").toString());
        });
    };
    const auto trySeenExit = [&](SeenExitMode mode, bool wideLastPitch = false) {
        const bool checkAssembly = mode == SeenExitMode::AlongContour &&
            std::all_of(sub.letterDiagnostics.begin(), sub.letterDiagnostics.end(),
                [](const auto& value) { return value.toObject().value("status") == "matched"; });
        for (int i = 0; i + 1 < sub.letterDiagnostics.size(); ++i) {
            const SeenExitRetry retry{i, mode, wideLastPitch};
            const auto slot = sub.letterDiagnostics[i].toObject();
            const bool unresolved = slot.value("status") == "no-match";
            if (!unresolved && (slot.value("status") != "matched" ||
                (!checkAssembly && sub.letterDiagnostics[i + 1].toObject().value("status") != "no-match"))) continue;
            const bool medial = i > 0;
            const auto rules = medial ? forms : config.value("initial").toArray();
            QStringList eligible;
            for (const auto& rule : rules) {
                const auto form = rule.toObject();
                const auto id = form.value("id").toString();
                const bool reconsiderForm = unresolved || wideLastPitch || mode == SeenExitMode::AlongContour;
                if (reconsiderForm ? !slot.value("candidates").toArray().contains(id) : slot.value("template") != id)
                    continue;
                if (!retry.supports(medial ? form.value("seenTeeth").toObject() : form))
                    continue;
                eligible.append(id);
                if (mode == SeenExitMode::ShiftedBend && !wideLastPitch) break;
            }
            if (eligible.isEmpty()) continue;
            auto partial = sub;
            if (segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true, &config, false, outlineCache, nullptr, &retry)) {
                const auto id = sub.letterDiagnostics[i].toObject().value("template").toString();
                if (eligible.contains(id)) {
                    if (mode != SeenExitMode::Configured)
                        sub.templateTrace.prepend(id + (mode == SeenExitMode::AlongContour
                            ? " · exit follows the contour from the third-tooth bend"
                            : " · exit at the bend after the third tooth"));
                    if (wideLastPitch)
                        sub.templateTrace.prepend(id + " · wider spacing before the third tooth");
                    return true;
                }
            }
            sub = std::move(partial);
        }
        return false;
    };
    const auto retrySeenExit = [&] {
        if (trySeenExit(SeenExitMode::ShiftedBend) || trySeenExit(SeenExitMode::AlongContour)) return true;
        for (auto mode : {SeenExitMode::Configured, SeenExitMode::ShiftedBend, SeenExitMode::AlongContour})
            if (trySeenExit(mode, true)) return true;
        return false;
    };
    const auto initial = sub.letterDiagnostics;
    auto best = sub;
    int bestScore = 0;
    const auto consider = [&] {
        int gain = 0;
        for (int i = 0; i < initial.size(); ++i) {
            const bool had = initial[i].toObject().value("status") == "matched";
            const bool has = sub.letterDiagnostics[i].toObject().value("status") == "matched";
            if (had && !has) return;
            if (!had && has) gain += needsRecovery(initial[i]) ? initial.size() + 1 : 1;
        }
        if (gain > bestScore) {
            bestScore = gain;
            best = sub;
        }
    };
    const auto searchPlacements = [&](const SeenExitRetry* seenExit) {
        ToothSearchPlan plan(initial.size());
        const auto run = [&] {
            if (segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true,
                                   &config, true, outlineCache, &plan, seenExit)) return true;
            if (!seenExit) consider();
            return false;
        };
        // Exhaust earlier forms and their placements before expanding the shape vocabulary.
        for (; plan.stage <= 3; ++plan.stage) {
            if (plan.stage > 1) {
                if (std::none_of(forms.begin(), forms.end(), [&](const auto& value) {
                        return value.toObject().value("recoveryStage").toInt() == plan.stage;
                    })) continue;
            }
            plan.enumerate = false;
            if (run()) return true;
            plan.enumerate = true;
            plan.skip.fill(0);
            plan.choices.fill(0);
            for (auto& seen : plan.encountered) seen.clear();
            for (int trial = 0; trial < 64; ++trial) {
                if (run()) return true;
                int failed = 0;
                while (failed < sub.letterDiagnostics.size() &&
                       sub.letterDiagnostics[failed].toObject().value("status") == "matched")
                    ++failed;
                if (!plan.advance(failed)) break;
            }
        }
        return false;
    };
    if (expand || std::any_of(initial.begin(),initial.end(),needsRecovery)) {
        if (searchPlacements(nullptr)) return true;
        sub = std::move(best);
    }
    if (retrySeenExit()) return true;

    // An earlier plausible cut can consume the first of three teeth, or stop
    // before the seen is reached. Revisit those choices only when the terminal
    // supplies an independent tail and the seen can end at its own third bend.
    if (initial.size() < 3) return false;
    const int slot = static_cast<int>(initial.size()) - 2;
    const SeenExitRetry seenExit{slot, SeenExitMode::AlongContour};
    const auto applicable = [&](const QJsonValue& rule, int letter) {
        return initial[letter].toObject().value("candidates").toArray().contains(rule.toObject().value("id"));
    };
    const auto finalForms = config.value("final").toArray();
    if (!std::any_of(finalForms.begin(), finalForms.end(), [&](const auto& rule) {
            return applicable(rule, slot + 1) && rule.toObject().value("ownsLeftTip").toBool();
        }) || !std::any_of(forms.begin(), forms.end(), [&](const auto& rule) {
            return applicable(rule, slot) && seenExit.supports(rule.toObject().value("seenTeeth").toObject());
        })) return false;
    auto previous = sub;
    if (searchPlacements(&seenExit)) {
        const auto id = sub.letterDiagnostics[slot].toObject().value("template").toString();
        sub.templateTrace.prepend(id + " · earlier cuts reconsidered with the exit at the third-tooth bend");
        return true;
    }
    sub = std::move(previous);
    return false;
}

bool PageAnalysisResult::segmentSubwordPass(int pageIndex, int lineIndex, int wordIndex, int subWordIndex,
                                            AFont* font, bool refresh, const QJsonObject* draft, bool recoverTeeth,
                                            OutlineSearchCache& outlineCache, ToothSearchPlan* plan, const SeenExitRetry* seenRetry) {
    const auto seenRetryFor = [&](int slot, const QJsonObject& settings) {
        return seenRetry && seenRetry->slot == slot && seenRetry->supports(settings) ? seenRetry : nullptr;
    };
	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];
    const bool enumerate = plan && plan->enumerate;
	auto& subWord = word.wordResultInfo.subWords[subWordIndex];

	if (subWord.baseGlyphs.size() > 0 && !refresh) {
		return subWord.correct;
	}

	subWord.baseGlyphs.clear();
	subWord.kashidaPaths.clear();
	subWord.kashidaCuts.clear();

    subWord.joins.clear();
    subWord.extrema.clear();
    subWord.templateTrace.clear();
    subWord.matchedTemplates.clear();
    subWord.letterDiagnostics = {};
    if (subWord.paths.empty() || subWord.paths.front() < 0 || subWord.paths.front() >= word.paths.size() ||
        word.paths[subWord.paths.front()].path.isEmpty()) {
        subWord.correct = false;
        return false;
    }
	const auto baseText = bodyLetters(subWord.text);
    const auto text = QStringView(subWord.text);
    bool hasFinalTatweel = false, pastFinal = false;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        const auto ch = *it;
        if (ch.isMark()) continue;
        if (pastFinal) {
            hasFinalTatweel = ch == QChar(0x0640);
            break;
        }
        if (ch != QChar(0x0640)) pastFinal = true;
    }

	const auto shape = word.bodyShape(subWord);

	if (baseText.size() < 2) {
		subWord.correct = !baseText.isEmpty();
		GlyphInfo glyphInfo{ baseText ,shape.path };
		subWord.baseGlyphs.push_back(glyphInfo);
		if (!baseText.isEmpty()) subWord.letterDiagnostics.append(QJsonObject{
            {"letterIndex", 1}, {"character", baseText}, {"position", "isolated"},
            {"status", "not-required"}, {"template", ""}, {"candidates", QJsonArray{}}});
		if (baseText.isEmpty()) subWord.templateTrace.append("No body letter in transcript · needs review");
		return subWord.correct;
	}


	const auto& config = draft ? *draft : letterForms();
	SearchContext context{ .joinDefaults = config.value("joinDefaults").toObject(),
	                       .subword = subWord, .baseText = baseText, .outlineCache = &outlineCache };

	auto& subPaths = context.subPaths;

	auto pos = shape.pos;
	context.transformedPath = shape.path * shape.transform;
	context.transformedPath.translate(pos.x(), pos.y());
	context.transformedPath.setFillRule(Qt::WindingFill);

	context.bbox = context.transformedPath.boundingRect();

	ShapeItem::getExtrema(context.transformedPath, subPaths, subWord.extrema);

	int basePathIndex = 0;

    // Near-coincident extrema must not erase a true Bezier corner.
    const auto oldExtrema = subWord.extrema;
    subWord.extrema.clear();
    for (const auto& point : oldExtrema) {
        auto existing = std::find_if(subWord.extrema.begin(), subWord.extrema.end(), [&](const auto& other) {
            const double dx = std::abs(other.point.x() - point.point.x());
            const double dy = std::abs(other.point.y() - point.point.y());
            return other.pathIndex == point.pathIndex && dx < .01 && dy < .01 &&
                (other.isSharp == point.isSharp || (dx < 1e-5 && dy < 1e-5));
        });
        if (existing == subWord.extrema.end()) subWord.extrema.append(point);
        else if (point.isSharp && !existing->isSharp) *existing = point;
    }
    for (const auto& point : subWord.extrema) if (point.pathIndex == basePathIndex) {
        context.timeSorted.insert(point);
        context.xSorted.insert(point);
        context.ySorted.insert(point);
    }

	if (context.timeSorted.size() < 3) { subWord.correct = false; return false; }
	context.setOrientation();
	subWord.basePathOrientation = context.orientation;


    for (const auto* phase : {"initial", "medial", "final"}) {
        const auto text = QString(phase) == "initial" ? baseText.left(1) : QString(phase) == "final" ? baseText.right(1) :
            baseText.mid(1, baseText.size() - 2);
        for (const auto& value : config.value(phase).toArray()) {
            const auto form = resolvedBodyForm(config, value.toObject());
            if (!sharedShapeDescription(form.value("body").toObject()) ||
                std::none_of(text.begin(), text.end(), [&](QChar c) { return form.value("current").toString().contains(c); })) continue;
            const auto evidence = recognizedSharedShape(context, form.value("body").toObject());
            InspectFormScope inspection(form, context.transformedPath);
            inspectSharedShape(*evidence);
        }
    }

    // Discover medial lām bodies before even the initial boundary is tried.
    // A failed preceding letter must not hide their evidence. Transcript
    // selectors limit which descriptions need evaluating, never their shape
    // search; the fixed-outline cache also serves every subsequent retry.
    if (baseText.size() > 2) {
        const auto medialText = QStringView(baseText).mid(1, baseText.size() - 2);
        for (const auto& value : config.value("medial").toArray()) {
            const auto form = value.toObject();
            const auto current = form.value("current").toString();
            if (std::none_of(medialText.begin(), medialText.end(), [&](QChar ch) { return current.contains(ch); }))
                continue;
            const auto body = formDescription(config, form, "body");
            if (body.value("shape") != "forked-stem" && body.value("shape") != "turning-shaft") continue;
            const auto evidence = recognizedMedialLamBodies(context, body);
            InspectFormScope inspection(form, context.transformedPath);
            inspectLamBodyEvidence(*evidence);
        }
    }

	context.nextIter = context.xSorted.size() != 0 ? std::next(context.xSorted.begin()) : context.xSorted.end();

	int lastIndex = baseText.length() - 1;

    // Position owns the letter selector and the side of its body boundary.
    // These derived flags are internal to the geometric searches, not template ordering.
    QJsonArray boundaryRules, finalRules;
    for (const auto* position : {"initial", "final"}) {
        for (const auto& value : config.value(position).toArray()) {
            auto rule = bodyConstraintForm(config, value.toObject());
            if (rule.contains("detachedArm")) continue;
            if (rule.value("retryWithVariants").toBool()) rule.insert("tryNext",true);
            if (!recoverTeeth && rule.value("onPartial").toBool()) continue;
            if (QString(position) == "initial") {
                rule.insert("first", true);
                rule.insert("bodyBoundary", "current");
            } else {
                rule.insert("next", rule.take("current"));
                rule.insert("last", true);
                rule.insert("bodyBoundary", "next");
                finalRules.append(rule);
            }
            boundaryRules.append(rule);
        }
    }
    QJsonArray alefTurnRules;
    for (const auto& value : finalRules)
        if (value.toObject().value("method") == "alef-stem") alefTurnRules.append(value);
    QVector<QString> bodyTemplateIds(baseText.size());
    QVector<int> sharedEntries;
    QMap<int, ToothBody> ownedTeeth;
    QMap<int, PathExtrema> ownedLamCrests;
    QMap<int, PathExtrema> matchedLamForks;
    QMap<int, PathExtrema> ownedOpenHeads;
    QVector<bool> bodyAttempted(baseText.size(), false);
    const auto applies = [&](const QJsonObject& rule, int index) {
        const auto current = rule.value("current");
        const auto next = rule.value("next");
        return (!current.isString() || current.toString().contains(baseText[index])) &&
               (!next.isString() || next.toString().contains(baseText[index + 1])) &&
               (!rule.contains("ligatureWith") || rule.value("ligatureWith").toString().contains(baseText[index + 1])) &&
               (!rule.contains("first") || rule.value("first").toBool() == (index == 0)) &&
               (!rule.contains("last") || rule.value("last").toBool() == (index + 1 == lastIndex));
    };
    QString retryRule;
    QMap<QString, Stretching> intrinsicFinalCuts;
    QMap<QString, Stretching> visibleFinalCuts;
    const auto attempt = [&](const QJsonObject& rule, int index, bool record = true) {
        InspectFormScope inspection(rule, context.transformedPath);
        const auto savedIter = context.nextIter;
        context.form = rule;
        context.res = {};
        const auto method = rule.value("method").toString();
        const int owner = rule.value("bodyBoundary") == "current" ? 0 : lastIndex;
        bodyAttempted[owner] = true;
        // These terminal searches use the fixed outline and their own settings.
        // Keep the proposal, including a miss. The join and ownership checks
        // below still run against the current placement.
        const auto intrinsicTerminal = [&](bool variant, const auto& search) {
            const auto key = std::make_pair(rule.value("id").toString(), variant);
            auto [it, inserted] = outlineCache.terminalCuts.try_emplace(key);
            if (inserted && search()) it->second = context.res;
            if (!it->second) return false;
            context.res = *it->second;
            return true;
        };
        bool found = false;
        if (sharedShapeDescription(rule.value("body").toObject())) {
            const bool terminal = owner == lastIndex && rule.value("body").toObject().value("shape").toString().startsWith("counter-");
            found = terminal ? intrinsicTerminal(false, [&] { return declaredSharedBoundary(context, rule); }) :
                               declaredSharedBoundary(context, rule);
        }
        else if (rule.contains("body")) found = declaredLamBoundary(context, config, rule);
        else if (method == "descending-stem") found = descendingStemExit(context,rule.value("descendingStem").toObject(),context.res);
        else if (method == "walk") {
            if (recoverTeeth && rule.contains("tailEntry"))
                found = terminalTailEntry(context,rule.value("tailEntry").toObject(),context.res);
            if (!found) found = searchContourWalk(context);
        }
        else if (method == "folded-bowl") found = foldedBowlEntry(context,rule.value("fold").toObject(),context.res);
        else if (method == "rising-tail") found = risingTailEntry(context,rule.value("tail").toObject(),context.res);
        else if (method == "folded-arm")
            found = intrinsicTerminal(false, [&] { return foldedArmRoot(context, rule, context.res); });
        else if (method == "bowl-fork")
            found = rule.value("stemFoot").toBool() ? terminalStemFork(context, rule, context.res)
                                                    : terminalFork(context, rule, context.res);
        else if (method == "tail-entry") found = terminalTailEntry(context,rule.value("tailEntry").toObject(),context.res);
        else if (method == "reh") found = searchNextReh(context);
        else if (method == "noon") found = searchNoonFina(context);
        else if (method == "alef-stem") {
            const bool shaftTurn = recoverTeeth && rule.value("shaftTurn").toBool();
            found = intrinsicTerminal(shaftTurn, [&] {
                return (shaftTurn && terminalStemTurn(context, rule, context.res)) ||
                       finalAlefStemCut(context, rule, context.res);
            });
        }
        else if (method == "stem-entry") found = searchStemEntry(context);
        else if (method == "lam-alef") found = searchLamAlefLiga(context);
        else if (method == "waw") found = searchWawFina(context);
        else if (method == "sad-tah") found = searchNextSadOrTah(context);
        else if (method == "seen-teeth") found = searchSeenTeeth(context, seenRetryFor(owner, rule));
        else if (method=="enclosed-head") found=initialEnclosedHead(context,rule.value("head").toObject(),context.res);
        else if (method == "hah") found = searchHah(context);
        else if (method == "kaf") found = searchKaf(context);
        else if (method == "kaf-zigzag") found = searchZigzagKaf(context);
        else if (method == "final-counter") {
            found = intrinsicTerminal(false, [&] {
                if (rule.contains("bridge")) return terminalLoopBridge(context, rule.value("bridge").toObject(), context.res);
                if (rule.contains("shoulder")) return terminalLoopShoulder(context, rule, context.res);
                return searchFinalCounter(context);
            });
        }
        else if (method == "initial-counter") found = searchInitialCounter(context);
        else if (method == "counter-ligature") found = searchCounterLigature(context);
        else if (method == "initial-loop-body") {
            const auto body = medialLoopBoundaries(context, loopSettings(rule),
                context.bbox.left(), context.bbox.right(), false);
            found = body.has_value();
            if (found) { context.res = body->second; context.res.fixedRightBoundary = true; }
        }
        else if (method == "heh-initial-counters") found = searchInitialHeh(context);
        else qWarning() << "Unknown letter form method:" << method;
        if (found && recoverTeeth && owner == lastIndex &&
            (method == "alef-stem" || rule.value("bottomProbe") == "walk-stroke-normal")) {
            const auto terminal = std::prev(context.xSorted.end())->time;
            if (getJoinTime(context.res,context.orientation).isStrictRight(terminal))
                std::swap(context.res.topBaseLine, context.res.bottomBaseLine);
        }
        // A normal starts at its contour anchor, which can be on either
        // flank. Orient a terminal slice by its own free end rather than
        // interpreting geometric upper/lower as letter ownership.
        if (found && owner == lastIndex && rule.value("ownsLeftTip").toBool() &&
            rule.value("bottomProbe") == "normal" &&
            getJoinTime(context.res, context.orientation).isStrictRight(std::prev(context.xSorted.end())->time))
            std::swap(context.res.topBaseLine, context.res.bottomBaseLine);
        QString rejectionReason;
        const bool bodyRejected = found && !matchesBoundaryBody(context, rule, context.res, owner == 0,
            config.value("_initialHeadLips").toObject(), &rejectionReason);
        if (bodyRejected) {
            inspectReason("Body", rejectionReason.isEmpty() ? "The candidate violates the configured body proportions." : rejectionReason);
            found = false;
        }
        if (found && rule.contains("bodyCounters")) {
            const auto bodies = cutRegions(context.transformedPath, {}, {context.res}, context.orientation, true);
            const int body = rule.value("bodyBoundary") == "current" ? 0 : 1;
            found = bodies.size() == 2 && static_cast<int>(toSubpaths(bodies[body].path).size()) - 1 == rule.value("bodyCounters").toInt();
        }
        bool accepted = found && matchesJoinFeatures(context);
        if (record && accepted && enumerate && owner == 0)
            accepted = plan->accepts(owner, context.res, context.res);
        if (accepted && owner == lastIndex && rule.value("retainBodyFeatures").toBool() &&
            !intrinsicFinalCuts.contains(rule.value("id").toString()))
            intrinsicFinalCuts.insert(rule.value("id").toString(), context.res);
        if (accepted && owner == lastIndex && rule.contains("visibleFrom") &&
            !visibleFinalCuts.contains(rule.value("id").toString()))
            visibleFinalCuts.insert(rule.value("id").toString(), context.res);
        if (!record) {
            context.nextIter = savedIter;
            return accepted;
        }
        const auto id = rule.value("id").toString();
        subWord.templateTrace.append(QString("%1 → %2 · %3 · %4")
            .arg(baseText.mid(index, 1), baseText.mid(index + 1, 1), id,
                 accepted ? QStringLiteral("matched") : bodyRejected ?
                    (rejectionReason.isEmpty() ? QStringLiteral("body proportions rejected") : rejectionReason) : QStringLiteral("no cut")));
        if (accepted) {
            subWord.matchedTemplates.append(id);
            bodyTemplateIds[owner] = id;
            if (retryRule.isEmpty() && rule.value("retryOnPartial").toBool()) retryRule = id;
            return true;
        }
        context.nextIter = savedIter;
        return false;
    };
    const auto preservesBody = [&](const QString& templateId, const Stretching& boundary, bool initial) {
        for (const auto& value : config.value(initial ? "initial" : "final").toArray()) {
            const auto rule = bodyConstraintForm(config, value.toObject());
            if (rule.value("id") != templateId) continue;
            if (!matchesBoundaryBody(context, rule, boundary, initial, config.value("_initialHeadLips").toObject()))
                return false;
            if (!initial && intrinsicFinalCuts.contains(templateId)) {
                const auto& intrinsic = intrinsicFinalCuts[templateId];
                const auto expected = getJoinTime(intrinsic, context.orientation);
                const auto actual = getJoinTime(boundary, context.orientation);
                const QLineF seam(intrinsic.topBaseLine.point, intrinsic.bottomBaseLine.point);
                const auto vector = seam.p2() - seam.p1();
                for (const auto& point : context.timeSorted) {
                    if (!expected.isStrictLeft(point.time) || actual.isLeft(point.time)) continue;
                    const double t = std::clamp(QPointF::dotProduct(point.point - seam.p1(), vector) /
                        std::max(1e-12, QPointF::dotProduct(vector, vector)), 0., 1.);
                    if (QLineF(point.point, seam.pointAt(t)).length() > seam.length()) return false;
                }
            }
            return true;
        }
        return true;
    };
    const auto sharedBoundary = [&](const QJsonObject& rule) {
        return rule.value("method") == "lam-alef" || (rule.value("sharedBoundary").toBool() &&
            (!rule.contains("sharedBoundaryWith") ||
             rule.value("sharedBoundaryWith").toString().contains(baseText[lastIndex - 1])));
    };
    struct BodyPair {
        Stretching primary;
        Stretching other;
        QString templateId;
        bool primaryIsNext;
        bool tightJoin;
        bool ligature;
        bool returning;
    };
    const auto medialForms = config.value("medial").toArray();
    const auto matchesBody = [&](const QJsonObject& form, int letter) {
        const auto final = form.value("sharedFoldWithFinal").toString();
        return form.value("current").toString().contains(baseText[letter]) &&
            (final.isEmpty() || (letter + 1 == lastIndex && final.contains(baseText.back())));
    };
    const auto supportsOpenBoundary = [&](const QJsonObject& form) {
        return form.contains("parallelReturn") || form.contains("enclosedHead") || form.contains("shaft") || form.contains("counterLigature") || form.contains("mouth") || form.contains("blade") || !form.value("contourBody").toObject().isEmpty() ||
            (formDescription(config, form, "body").value("shape") == "forked-stem" ||
             formDescription(config, form, "body").value("shape") == "turning-shaft") ||
            !form.value("pairedCounterCorners").toObject().isEmpty() ||
            !loopSettings(form).isEmpty() ||
            !form.value("openHead").toObject().isEmpty() ||
            hasToothBody(form) ||
            !form.value("seenTeeth").toObject().isEmpty() ||
            !loopHeadSettings(form).isEmpty() ||
            form.value("pairedCounters").toBool() ||
            (!form.value("right").toObject().value("walk").toArray().isEmpty() &&
             !form.value("left").toObject().value("walk").toArray().isEmpty());
    };
    const auto canProposeMedialBoundary = [&](int letter) {
        return letter > 0 && letter < lastIndex &&
            std::any_of(medialForms.begin(), medialForms.end(), [&](const auto& value) {
                const auto form = value.toObject();
                return supportsOpenBoundary(form) && matchesBody(form, letter);
            });
    };
    bool deferredFinalJoin = false;
    bool resolvedFinalJoin = false;
    const auto canDefer = [&](int index) {
        if (index == 0 || index + 1 != lastIndex ||
            subWord.joins.size() != static_cast<size_t>(index)) return false;
        const auto medial = config.value("medial").toArray();
        const bool hasTooth = std::any_of(medial.begin(), medial.end(), [&](const auto& value) {
            const auto form = value.toObject();
            return hasToothBody(form) &&
                form.value("current").toString().contains(baseText[index]);
        });
        return hasTooth && (std::any_of(finalRules.begin(), finalRules.end(), [&](const auto& value) {
            const auto rule = value.toObject();
            return rule.value("bodyBoundary") == "next" && rule.value("pairedOnly").toBool() &&
                applies(rule, index);
        }) || std::any_of(alefTurnRules.begin(), alefTurnRules.end(), [&](const auto& value) {
            return applies(value.toObject(), index);
        }));
    };
    const bool preserveReadingOrder = config.value("_checkRecoveredCuts").toBool();
    const auto storeJoins = [&](std::vector<Stretching>& cuts) {
        if (preserveReadingOrder)
            for (int i = 0; i < static_cast<int>(cuts.size()); ++i)
                cuts[i].readingOrder = i;
        subWord.joins = Joins(cuts.begin(), cuts.end());
    };
    std::vector<BodyPair> bodyPairs;
    // Preserve explicit boundaries of following letters before the medial pass
    // replaces a join's primary anchor with the medial body's outgoing cut.
    struct BodyBoundary {
        Stretching cut;
        QString templateId;
        bool ligature = false;
    };
    std::vector<std::optional<BodyBoundary>> nextBoundaries(lastIndex);
    for (int index = 0; index < lastIndex; ++index) {
        bool found = false;
        for (const auto& value : boundaryRules) {
            const auto rule = value.toObject();
            if (rule.value("pairedOnly").toBool() || !applies(rule, index)) continue;
            if (rule.value("method") == "alef-stem" && canDefer(index)) {
                deferredFinalJoin = true;
                break;
            }
            found = attempt(rule, index);
            if (found || (!enumerate && !rule.value("tryNext").toBool())) break;
        }
        if (!found) {
            if (canDefer(index)) deferredFinalJoin = true;
            break;
        }
        context.res.kashidaOverrides = context.form.value("kashida").toObject();
        if (context.res.counterEntry.pathIndex > 0) {
            context.res.kashidaOverrides.insert("enabled", false);
            if (index + 1 == lastIndex) bodyTemplateIds[lastIndex] = context.form.value("id").toString();
            subWord.templateTrace.append(context.form.value("id").toString() + " · shared ligature boundary across two ink bridges");
            if (preserveReadingOrder) context.res.readingOrder=index;
            subWord.joins.insert(context.res);
            continue;
        }
        const auto primary = context.res;
        const auto primaryForm = context.form;
        const auto side = primaryForm.value("bodyBoundary").toString();
        if (side == "next") nextBoundaries[index] = BodyBoundary{primary, primaryForm.value("id").toString(),
            sharedBoundary(primaryForm)};
        if (!side.isEmpty()) {
            const auto otherSide = side == "next" ? QStringLiteral("current") : QStringLiteral("next");
            context.pairedStrokeWidth = QLineF(primary.topBaseLine.point, primary.bottomBaseLine.point).length();
            const auto& pairingRules = boundaryRules;
            for (const auto& value : pairingRules) {
                const auto rule = value.toObject();
                if (rule.value("bodyBoundary").toString() != otherSide || !applies(rule, index)) continue;
                if (attempt(rule, index, false)) {
                    bodyPairs.push_back({primary, context.res, rule.value("id").toString(), side == "next",
                                         rule.value("tightJoin").toBool() || primaryForm.value("tightJoin").toBool(),
                                         sharedBoundary(rule) || sharedBoundary(primaryForm),primaryForm.value("returningStroke").toBool() || rule.value("returningStroke").toBool()});
                }
            }
            context.pairedStrokeWidth = 0;
            context.res = primary;
            context.form = primaryForm;
        }
        if (preserveReadingOrder) context.res.readingOrder=index;
            subWord.joins.insert(context.res);
    }

    // A join can carry one cut from each neighboring letter body.
    // Keep the original single-rule result when the pair cannot form a valid span.
    if (!bodyPairs.empty() && subWord.joins.size() == static_cast<size_t>(lastIndex)) {
        auto cuts = std::vector<Stretching>(subWord.joins.begin(), subWord.joins.end());
        const Stretching::cmpbyx before;
        const auto chord = [](const Stretching& cut) {
            return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
        };
        const auto& ink = context.transformedPath;
        const auto& localPath = shape.path;
        for (const auto& pair : bodyPairs) {
            if (!bodyTemplateIds[pair.primaryIsNext ? 0 : lastIndex].isEmpty()) continue;
            const auto original = std::find_if(cuts.begin(), cuts.end(), [&](const Stretching& cut) {
                return !before(cut, pair.primary) && !before(pair.primary, cut);
            });
            if (original == cuts.end()) continue;
            const auto index = std::distance(cuts.begin(), original);
            const auto& right = pair.primaryIsNext ? pair.other : pair.primary;
            const auto& left = pair.primaryIsNext ? pair.primary : pair.other;
            const double gap = chord(right).center().x() - chord(left).center().x();
            const auto incomingRegion = getJoinTime(right, context.orientation);
            const bool ordered = incomingRegion.isStrictLeft(left.topBaseLine.time) &&
                incomingRegion.isStrictLeft(left.bottomBaseLine.time);
            const bool closeInterleaved = !ordered && QLineF(chord(right).center(), chord(left).center()).length() <=
                std::min(chord(right).length(), chord(left).length());
            if ((pair.ligature || (pair.tightJoin &&
                ((gap <= 0 && -gap <= chord(right).length()) || closeInterleaved))) &&
                (index == 0 || before(cuts[index - 1], left)) &&
                (index + 1 == cuts.size() || before(left, cuts[index + 1])) &&
                interiorChord(ink, left.topBaseLine, left.bottomBaseLine)) {
                auto trial = cuts;
                trial[index] = left;
                const auto pieces = cutRegions(localPath, subWord.text, trial, subWord.basePathOrientation, true);
                if ((index != 0 || preservesBody(bodyTemplateIds[0], left, true)) &&
                    (index + 1 != cuts.size() || preservesBody(bodyTemplateIds[lastIndex], left, false)) &&
                    pieces.size() == trial.size() + 1 &&
                    std::none_of(pieces.begin(), pieces.end(), [](const GlyphInfo& piece) {
                        return piece.path.boundingRect().isEmpty();
                    })) {
                    original->topBaseLine = original->topLeft = original->topRight = left.topBaseLine;
                    original->bottomBaseLine = original->bottomLeft = original->bottomRight = left.bottomBaseLine;
                    original->fixedLeftBoundary = false;
                    original->length = 0;
                    original->kashidaOverrides.insert("enabled", false);
                    subWord.matchedTemplates.append(pair.templateId);
                    bodyTemplateIds[pair.primaryIsNext ? 0 : lastIndex] = pair.templateId;
                    subWord.templateTrace.append(pair.templateId + (pair.ligature
                        ? " · shared ligature cut" : " · shared cut for tight join"));
                    continue;
                }
            }
            const auto settings = withOverrides(config.value("kashidaDefaults").toObject(), original->kashidaOverrides);
            if (!settings.value("enabled").toBool(true) || (pair.returning ? !ordered : !before(right,left)) ||
                ((settings.value("minSpan").toDouble()>0 || !pair.returning) &&
                 chord(right).center().x() - chord(left).center().x() < settings.value("minSpan").toDouble()) ||
                (index > 0 && !before(cuts[index - 1], right)) ||
                (index + 1 < cuts.size() && !before(left, cuts[index + 1])) ||
                !interiorChord(ink, right.topBaseLine, right.bottomBaseLine, right.topEndpointTolerance) ||
                !interiorChord(ink, left.topBaseLine, left.bottomBaseLine, left.topEndpointTolerance)) continue;

            auto trial = cuts;
            trial[index] = right;
            trial.insert(trial.begin() + index + 1, left);
            bool crossed = false;
            for (size_t i = 0; i < trial.size(); ++i) {
                if (i != index && chord(right).intersects(chord(trial[i]), nullptr) == QLineF::BoundedIntersection)
                    crossed = true;
                if (i != index + 1 && chord(left).intersects(chord(trial[i]), nullptr) == QLineF::BoundedIntersection)
                    crossed = true;
            }
            if (crossed) continue;
            const auto pieces = cutRegions(localPath, subWord.text, trial, subWord.basePathOrientation, true);
            if (pieces.size() != trial.size() + 1 ||
                std::any_of(pieces.begin(), pieces.end(), [](const GlyphInfo& piece) {
                    return piece.path.boundingRect().isEmpty();
                })) continue;

            original->topBaseLine = right.topBaseLine;
            original->bottomBaseLine = right.bottomBaseLine;
            original->topLeft = left.topBaseLine;
            original->bottomLeft = left.bottomBaseLine;
            original->fixedRightBoundary = true;
            original->fixedLeftBoundary = true;
            subWord.matchedTemplates.append(pair.templateId);
            bodyTemplateIds[pair.primaryIsNext ? 0 : lastIndex] = pair.templateId;
            subWord.templateTrace.append(pair.templateId + " · paired body boundary matched");
        }
        storeJoins(cuts);
    }

    const auto missingJoin = static_cast<int>(subWord.joins.size());
    const bool openMedialJoin = !deferredFinalJoin && canProposeMedialBoundary(missingJoin);
    bool pendingMedialJoin = openMedialJoin;
    // A medial body contributes one boundary to each adjacent join.
    if (openMedialJoin || deferredFinalJoin || subWord.joins.size() == static_cast<size_t>(lastIndex)) {
        auto cuts = std::vector<Stretching>(subWord.joins.begin(), subWord.joins.end());
        // Reserve the final slot without proposing any join geometry.
        if (openMedialJoin || deferredFinalJoin) cuts.emplace_back();
        const auto before = [&](const Stretching& a, const Stretching& b) {
            return cutPrecedes(a, b, context.orientation);
        };
        const auto center = [](const Stretching& join) {
            return QLineF(join.topBaseLine.point, join.bottomBaseLine.point).center();
        };
        QMap<QString, std::vector<ToothBody>> localTeeth;
        auto& teethByForm = plan ? plan->teeth : localTeeth;
        struct MedialAttempt {
            QJsonObject form;
            const ToothBody* tooth = nullptr;
            bool requireFinal = false;
        };
        const auto medialAttempts = [&](int letter, bool open) {
            std::vector<MedialAttempt> attempts, teeth;
            std::optional<size_t> toothPosition;
            for (const auto& value : medialForms) {
                const auto form = value.toObject();
                if (!matchesBody(form, letter) || (open && !supportsOpenBoundary(form))) continue;
                const auto toothSettings = toothRecognitionSettings(form);
                const int stage = form.value("recoveryStage").toInt();
                if ((!recoverTeeth && stage > 0) || (plan && stage > plan->stage)) continue;
                bodyAttempted[letter] = true;
                if (!hasToothBody(form)) {
                    attempts.push_back({form});
                    continue;
                }
                if (!toothPosition) toothPosition = attempts.size();
                const auto id = form.value("id").toString();
                if (!teethByForm.contains(id)) {
                    auto bodies = declaredToothBoundaries(context, form);
                    if (recoverTeeth && toothPairDescription(toothSettings) && toothSettings.value("baselineEntry").isObject()) {
                        const auto entry = toothSettings.value("baselineEntry").toObject();
                        const auto policy = toothPlacementSettings(form).value("baselineEntry").toObject();
                        for (auto& body : bodies) extendToothToBaseline(context, body, entry, policy);
                    }
                    teethByForm.insert(id, std::move(bodies));
                }
                for (const auto& candidate : teethByForm.constFind(id).value()) {
                    const bool used = std::any_of(ownedTeeth.cbegin(), ownedTeeth.cend(), [&](const ToothBody& owned) {
                        return std::abs(owned.tip.time - candidate.tip.time) < 1e-7;
                    });
                    if (!used) teeth.push_back({form, &candidate});
                }
            }
            // A short tooth may precede a tall tooth. Template priority only
            // breaks ties at one tip; successive tips follow reading order.
            std::stable_sort(teeth.begin(), teeth.end(), [](const auto& a, const auto& b) {
                return a.tooth->tip.point.x() > b.tooth->tip.point.x();
            });
            if (toothPosition) attempts.insert(attempts.begin() + *toothPosition, teeth.begin(), teeth.end());
            return attempts;
        };
        // During medial placement the tentative cuts live in 'cuts'. The
        // committed joins seen by attempt() stay fixed until storeJoins().
        // Reuse the independent final probe when another candidate asks the
        // same question at the same cursor and local stroke width.
        struct FinalProbe {
            bool matched;
            Stretching cut;
            QJsonObject form;
        };
        using ProbeKey = std::tuple<QString, xsortedtype::difference_type, double>;
        std::map<ProbeKey, FinalProbe> finalProbes;
        const auto probeFinal = [&](const QJsonObject& rule) {
            const auto cursor = std::distance(context.xSorted.begin(), context.nextIter);
            const ProbeKey key{rule.value("id").toString(), cursor, context.pairedStrokeWidth};
            auto hit = finalProbes.find(key);
            if (hit == finalProbes.end()) {
                const bool matched = attempt(rule, lastIndex - 1, false);
                finalProbes.emplace(key, FinalProbe{matched, context.res, context.form});
                return matched;
            }
            context.res = hit->second.cut;
            context.form = hit->second.form;
            return hit->second.matched;
        };
        for (int letter = 1; letter < static_cast<int>(cuts.size()); ++letter) {
            const bool open = pendingMedialJoin && letter + 1 == static_cast<int>(cuts.size());
            auto& incoming = cuts[letter - 1];
            auto& outgoing = cuts[letter];
            const bool deferred = deferredFinalJoin && letter + 1 == lastIndex;
            if (deferred && std::any_of(alefTurnRules.begin(), alefTurnRules.end(), [&](const auto& value) {
                    return applies(value.toObject(), letter);
                })) {
                // If the initial walk ended on the tooth tip, recover the preceding
                // boundary from the two shoulders on the incoming side of that tooth.
                const auto tip = context.timeSorted.find(incoming.topBaseLine);
                if (tip != context.timeSorted.end() && tip->point.y() < context.prev(tip)->point.y() &&
                    tip->point.y() < context.next(tip)->point.y()) {
                    const auto& ink = context.transformedPath;
                    const double width = QLineF(tip->point, incoming.bottomBaseLine.point).length();
                    for (auto bend = context.timeSorted.begin(); bend != context.timeSorted.end(); ++bend) {
                        if (bend->point.y() <= tip->point.y() ||
                            bend->point.y() > context.prev(bend)->point.y() ||
                            bend->point.y() > context.next(bend)->point.y() ||
                            QLineF(tip->point, bend->point).length() > 3 * width ||
                            !interiorChord(ink, *tip, *bend)) continue;
                        const auto shoulder = [&](auto point, auto advance) {
                            for (int step = 0; step < 4; ++step) {
                                const auto next = (context.*advance)(point);
                                if (next->point.y() <= point->point.y()) break;
                                point = next;
                            }
                            return point;
                        };
                        for (const auto advance : {&SearchContext::prev, &SearchContext::next}) {
                            const auto reverse = advance == &SearchContext::prev
                                ? &SearchContext::next : &SearchContext::prev;
                            const auto top = shoulder(tip, advance);
                            const auto bottom = shoulder(bend, reverse);
                            if (top->point.x() <= tip->point.x() || bottom->point.x() <= tip->point.x() ||
                                bottom->point.y() <= top->point.y() || !interiorChord(ink, *top, *bottom)) continue;
                            incoming.topBaseLine = *top;
                            incoming.bottomBaseLine = *bottom;
                            break;
                        }
                        if (incoming.topBaseLine.time != tip->time) break;
                    }
                }
            }
            const auto entry = center(incoming);
            const auto ordinaryExit = center(outgoing);
            const double stroke = incoming.referenceWidth > 0 ? incoming.referenceWidth :
                QLineF(incoming.topBaseLine.point, incoming.bottomBaseLine.point).length();
            if (stroke <= 0 || (!open && !deferred && entry.x() <= ordinaryExit.x())) continue;

            auto attempts = medialAttempts(letter, open);
            // Try forms that preserve an independent final boundary before
            // accepting a partial reading at the end of a tooth sequence.
            if (!ownedTeeth.isEmpty() && letter + 1 == lastIndex &&
                bodyTemplateIds[lastIndex].isEmpty() && attempts.size() > 1) {
                const auto partialAttempts = attempts;
                for (auto& attempt : attempts) attempt.requireFinal = true;
                attempts.insert(attempts.end(), partialAttempts.begin(), partialAttempts.end());
            }
            for (const auto& medial : attempts) {
                const auto form = bodyConstraintForm(config, medial.form);
                const bool composeSeenTail = plan && seenRetry && seenRetry->slot == letter;
                if (composeSeenTail && !seenRetry->supports(form.value("seenTeeth").toObject())) continue;
                std::vector<MedialCandidate> candidates;
                const auto contour = form.value("contourBody").toObject();
                const auto loop = loopSettings(form);
                const auto openHead = form.value("openHead").toObject();
                const auto pairedCorners = form.value("pairedCounterCorners").toObject();
                const auto tooth = toothRecognitionSettings(form);
                const auto head = loopHeadSettings(form);
                const auto bridge = form.value("counterLigature").toObject();
                const bool internalBody =
                    formDescription(config, form, "body").value("shape") == "turning-shaft" || form.contains("parallelReturn") || form.contains("enclosedHead") || form.contains("shaft") || form.contains("mouth") || form.contains("blade") || !bridge.isEmpty() || !contour.isEmpty() || !loop.isEmpty() || !openHead.isEmpty() ||
                    !pairedCorners.isEmpty() || !tooth.isEmpty() || !head.isEmpty() ||
                    (form.value("sharedEntry").toBool() && !form.value("seenTeeth").toObject().isEmpty());
                if (!bridge.isEmpty() && bridge.value("letters").toString().contains(baseText[letter + 1])) {
                    const auto previousForm = context.form;
                    const auto previousResult = context.res;
                    context.form = bridge;
                    context.res = {};
                    if (searchCounterLigature(context)) {
                        auto left = context.res;
                        const auto hole = context.subPaths[left.counterEntry.pathIndex].boundingRect();
                        Stretching right{};
                        if (loopShoulder(context, hole, bridge.value("shoulder").toObject(), left.topBaseLine.time,
                                         false, right))
                            candidates.emplace_back(right, left);
                    }
                    context.form = previousForm;
                    context.res = previousResult;
                }
                if (form.contains("parallelReturn") || form.contains("enclosedHead") || form.contains("shaft") ||
                    form.contains("blade") || form.contains("mouth")) {
                    const auto generate = [&]() {
                        if (form.contains("parallelReturn"))
                            return parallelKafSections(context, form.value("parallelReturn").toObject());
                        if (form.contains("enclosedHead"))
                            return enclosedHeadSections(context, form.value("enclosedHead").toObject());
                        if (form.contains("shaft"))
                            return shaftSections(context, form.value("shaft").toObject());
                        if (form.contains("blade"))
                            return bladeSections(context, form.value("blade").toObject());
                        return mouthSections(context, form.value("mouth").toObject());
                    };
                    const auto id = form.value("id").toString();
                    if (plan && !plan->intrinsic.contains(id))
                        plan->intrinsic.insert(id, generate());
                    const auto& proposals = plan ? plan->intrinsic.constFind(id).value() : generate();
                    std::set<std::array<int, 4>> distinct;
                    const auto timeKey = [](double t) { return static_cast<int>(std::lround(t * 10000)); };
                    for (const auto& [right, left] : proposals) {
                        if (distinct
                                .insert({timeKey(right.topBaseLine.time), timeKey(right.bottomBaseLine.time),
                                         timeKey(left.topBaseLine.time), timeKey(left.bottomBaseLine.time)})
                                .second)
                            candidates.emplace_back(right, left);
                    }
                }
                if (!contour.isEmpty()) {
                    const auto key = std::make_pair(form.value("id").toString(), context.pairedStrokeWidth);
                    auto [it, inserted] = outlineCache.contourBodies.try_emplace(key);
                    if (inserted) it->second = contourBodyBoundaries(context, contour);
                    for (const auto& [right, left] : it->second) candidates.emplace_back(right, left);
                }
                if (!pairedCorners.isEmpty())
                    for (const auto& [right, left] : medialPairedCounterBoundaries(context, pairedCorners))
                        candidates.emplace_back(right, left);
                if (!head.isEmpty()) {
                    const auto evidence = recognizedSharedShape(context, form.value("body").toObject());
                    for (const auto& [right, left] : cutCrestedLoops(context, evidence->crestedHeads,
                            form.value("cuts").toObject().value("head").toObject()))
                        candidates.emplace_back(right, left);
                }
                const double bodyMinX = open ? context.bbox.left() : ordinaryExit.x();
                if (!loop.isEmpty()) {
                    // A shared-entry loop can reclaim its own body from an
                    // overextended preceding cut. Limit that search to the
                    // preceding body's contour interval, rather than borrowing
                    // a loop already assigned to an earlier letter.
                    QPainterPath available;
                    const QPainterPath* scope = nullptr;
                    double maxX = entry.x();
                    if (loop.contains("exitCorner") && form.value("sharedEntry").toBool()) {
                        const int boundary = letter - 2;
                        if (boundary >= 0) {
                            const auto pieces = cutRegions(context.transformedPath, {}, {cuts[boundary]},
                                context.orientation, true);
                            if (pieces.size() != 2) continue;
                            available = pieces[1].path;
                            scope = &available;
                        }
                        maxX = context.bbox.right();
                    }
                    if (loop.contains("rimAnchors")) {
                        const auto settings = loop.value("rimAnchors").toObject();
                        const auto evidence = form.value("body").toObject().value("shape") == "counter-rim" ?
                            recognizedSharedShape(context, form.value("body").toObject()) : nullptr;
                        const auto proposals = evidence ? cutRimBodies(context, evidence->rims, settings) : rimSections(context, settings);
                        for (const auto& [right, left] : proposals) candidates.emplace_back(right, left);
                    } else {
                        std::optional<std::pair<Stretching, Stretching>> body;
                        if (form.value("body").toObject().value("shape") == "counter-loop") {
                            const auto evidence = recognizedSharedShape(context, form.value("body").toObject());
                            const auto selected = selectLoopBody(context, evidence->loops, bodyMinX, maxX, scope);
                            if (selected) body = cutLoopBody(context, *selected, loop, true);
                            else {
                                InspectFormScope inspection(form);
                                inspectReason("Assembly", "No recognized loop fits this slot's contour ownership and position.");
                            }
                        } else body = medialLoopBoundaries(context, loop, bodyMinX, maxX, true, scope);
                        if (body) candidates.emplace_back(body->first, body->second);
                    }
                }
                if (!openHead.isEmpty())
                    for (const auto& [right, left] : medialOpenHeadBoundaries(context, openHead, bodyMinX, entry.x()))
                        candidates.emplace_back(right, left);
                auto exit = ordinaryExit;
                std::optional<Stretching> nextBody;
                QString nextBodyId;
                if (!tooth.isEmpty() && letter + 1 == lastIndex) {
                    context.pairedStrokeWidth = stroke;
                    for (const auto& ruleValue : finalRules) {
                        const auto rule = ruleValue.toObject();
                        // An independent final stroke walk already specifies its
                        // cut. Keep the tooth candidates on its incoming side.
                        const bool suppliesExit = rule.value("pairedOnly").toBool() ||
                            rule.value("method") == "alef-stem" || rule.contains("penWidth") ||
                            formDescription(config, rule, "body").value("shape") == "terminal-fork" ||
                            rule.value("strokeSource") == "local";
                        if (!suppliesExit || !applies(rule, letter) || !probeFinal(rule)) continue;
                        nextBody = context.res;
                        nextBodyId = rule.value("id").toString();
                        exit = center(*nextBody);
                        break;
                    }
                    context.pairedStrokeWidth = 0;
                }
                if (deferred && tooth.isEmpty()) continue;
                if (medial.tooth)
                    candidates.push_back({*medial.tooth->right, medial.tooth->left});
                const auto lamBody = formDescription(config, form, "body");
                const bool forkedLam = lamBody.value("shape") == "forked-stem";
                const bool foldedFinal = form.contains("sharedFoldWithFinal");
                std::optional<BodyBoundary> sharedExit;
                if (!bridge.isEmpty() && !candidates.empty() && letter + 1 == lastIndex)
                    sharedExit = BodyBoundary{candidates.front().left, form.value("id").toString(), true};
                if ((forkedLam || foldedFinal) && letter + 1 == lastIndex) {
                    const auto& saved = nextBoundaries[letter];
                    if (saved && (foldedFinal || saved->ligature)) sharedExit = saved;
                    else {
                        const auto originalForm = context.form;
                        const auto originalResult = context.res;
                        for (const auto& ruleValue : finalRules) {
                            const auto rule = ruleValue.toObject();
                            if ((foldedFinal || sharedBoundary(rule)) && applies(rule, letter) &&
                                probeFinal(rule)) {
                                sharedExit = BodyBoundary{context.res, rule.value("id").toString(), true};
                                break;
                            }
                        }
                        context.form = originalForm;
                        context.res = originalResult;
                    }
                }
                if (foldedFinal && !sharedExit) continue;
                if (foldedFinal && !contour.isEmpty()) {
                    // Two bodies sharing a folded attachment have one seam.
                    // Generate the head around that seam and recheck its own
                    // retained features, rather than probing into the bowl below.
                    candidates.clear();
                    for (const auto& [right, left] : contourBodyBoundaries(context, contour, &sharedExit->cut))
                        candidates.emplace_back(right, left);
                }
                if (lamDescription(form.value("body").toObject())) {
                    const auto evidence = recognizedMedialLamBodies(context, lamBody);
                    const auto declarations = assembleMedialLamCuts(context, config, form, *evidence, incoming,
                        entry, exit, stroke, open, recoverTeeth, pageIndex >= 0,
                        sharedExit ? std::optional<Stretching>{sharedExit->cut} : std::nullopt);
                    candidates.insert(candidates.end(), declarations.begin(), declarations.end());
                }
                const auto teeth = form.value("seenTeeth").toObject();
                std::array<PathExtrema,3> seenTips{};
                if (!teeth.isEmpty()) {
                    Stretching right{}, left{};
                    // A compact form may reclaim teeth swallowed by a generic
                    // cut, but it must not count a recognized open head as a tooth.
                    const bool intrinsic = form.value("sharedEntry").toBool();
                    const auto previousHead = ownedOpenHeads.constFind(letter - 1);
                    const double rightLimit = !intrinsic ? entry.x() :
                        previousHead == ownedOpenHeads.cend() ? context.bbox.right() : previousHead->point.x();
                    const double leftLimit = intrinsic || open || foldedFinal ? context.bbox.left() : exit.x() - stroke;
                    if (seenTeethBoundaries(context, teeth, leftLimit, rightLimit, &right, &left,
                                            foldedFinal ? &sharedExit->cut : nullptr, &seenTips,
                                            seenRetryFor(letter, teeth)))
                        if (!intrinsic || letter == 1 || before(cuts[letter - 2], right))
                            candidates.emplace_back(right, left);
                }
                if (!internalBody && tooth.isEmpty() && head.isEmpty() && !forkedLam && teeth.isEmpty()) for (const auto& start : context.xSorted) {
                    // Paired-counter bodies are identified by their enclosed holes
                    // below, so their walks need not start at the preceding letter.
                    if (!form.value("pairedCounters").toBool() &&
                        (std::abs(start.point.x() - entry.x()) > 2 * stroke ||
                         std::abs(start.point.y() - entry.y()) > 2 * stroke)) continue;
                    Stretching right{}, left{};
                    if (medialBodyChord(context, start, form.value("right").toObject(), right) &&
                        medialBodyChord(context, start, form.value("left").toObject(), left))
                        candidates.emplace_back(right, left);
                }
                if (form.value("pairedCounters").toBool()) {
                    // The two counters of this form straddle its joining stroke.
                    // Probe just outside their combined width if the walk misses a turn.
                    const double baseline = open ? entry.y() : (entry.y() + exit.y()) / 2;
                    const double margin = stroke * 0.2;
                    for (size_t i = 1; i < context.subPaths.size(); ++i) {
                        const auto upper = context.subPaths[i].boundingRect();
                        if (upper.center().y() >= baseline || context.transformedPath.contains(upper.center())) continue;
                        for (size_t j = 1; j < context.subPaths.size(); ++j) {
                            const auto lower = context.subPaths[j].boundingRect();
                            if (lower.center().y() <= baseline || context.transformedPath.contains(lower.center()) ||
                                upper.right() < lower.left() || lower.right() < upper.left()) continue;
                            const auto bounds = upper.united(lower);
                            if (bounds.right() >= entry.x() ||
                                (open ? entry.x() - bounds.right() > 6 * stroke : bounds.left() <= exit.x() - stroke) ||
                                bounds.height() > 8 * stroke) continue;
                            Stretching right{}, left{};
                            const double probeY = open ? (upper.bottom() + lower.top()) / 2 : baseline;
                            if (medialChordAt(context, bounds.right() + margin, probeY, stroke, right) &&
                                medialChordAt(context, bounds.left() - margin, probeY, stroke, left))
                                candidates.emplace_back(right, left);
                        }
                    }
                }
                bool matched = false;
                const bool preferFinalBoundary = forkedLam && letter + 1 == lastIndex &&
                    !outgoing.fixedLeftBoundary;
                for (int pass = 0; pass < (preferFinalBoundary ? 2 : 1) && !matched; ++pass) {
                for (auto [right, left, sharedJoins, hollowCuts, lamCrest, lamFork] : candidates) {
                    if (form.contains("bodyCounters")) {
                        const auto pieces = cutRegions(context.transformedPath, {}, {right, left}, context.orientation, true);
                        if (pieces.size() != 3 || static_cast<int>(toSubpaths(pieces[1].path).size()) - 1 !=
                            form.value("bodyCounters").toInt()) continue;
                    }
                    // A stacked ligature has one intrinsic interface between
                    // bodies. It replaces the preceding generic initial cut;
                    // no separate stretch of connecting ink exists there.
                    // A seen entry may overlap an earlier cut, but separated
                    // boundaries retain the genuine incoming connector.
                    sharedJoins |= form.value("reclaimEntry").toBool() && !before(incoming,right);
                    sharedJoins |= form.value("sharedEntry").toBool() &&
                        (teeth.isEmpty() || !before(incoming, right));
                    if (medial.tooth && !bodyTemplateIds[letter - 1].isEmpty()) {
                        const auto side = getJoinTime(incoming, context.orientation);
                        // An initial or medial form may itself end in a tooth
                        // (notably seen and sad). A later letter cannot claim
                        // a tip and its bend already inside that matched body.
                        if (side.isStrictRight(medial.tooth->tip.time) && side.isStrictRight(medial.tooth->bend.time)) {
                            const auto rules = letter == 1 ? config.value("initial").toArray() : medialForms;
                            const bool intrinsicTooth = std::any_of(rules.begin(), rules.end(), [&](const auto& value) {
                                const auto prior = value.toObject();
                                return prior.value("id") == bodyTemplateIds[letter - 1] &&
                                    (hasToothBody(prior) || prior.contains("seenTeeth") || prior.value("method") == "seen-teeth" ||
                                     loopSettings(prior).contains("exitTooth"));
                            });
                            if (intrinsicTooth) continue;
                        }
                    }
                    if (medial.tooth && !before(incoming, right) && before(incoming, left)) {
                        // The preceding body's cut can fall inside this tooth's
                        // incoming margin. Share it only if the actual remaining
                        // mask retains the complete tip and the facing bend.
                        const auto& feature = *medial.tooth;
                        const auto side = getJoinTime(incoming, context.orientation);
                        if (side.isStrictLeft(feature.tip.time) && side.isStrictLeft(feature.bend.time)) {
                            const auto parts = cutRegions(context.transformedPath, {}, {incoming, left}, context.orientation, true);

                            if (parts.size() == 3 && toSubpaths(parts[1].path).size() == 1 &&
                                feature.retainedBy(parts[1].path)) {
                                sharedJoins = true;
                                right = incoming;
                            }
                        }
                    }
                    if (!sharedJoins && !before(incoming, right) && form.value("tightJoin").toBool()) {
                        const double width = std::max(QLineF(incoming.topBaseLine.point,incoming.bottomBaseLine.point).length(),
                            QLineF(right.topBaseLine.point,right.bottomBaseLine.point).length());
                        sharedJoins = QLineF(center(incoming),center(right)).length() <= width;
                    }
                    if (sharedJoins && !ownedTeeth.isEmpty()) {
                        const auto sides = getJoinTime(right, context.orientation);
                        if (std::any_of(ownedTeeth.cbegin(), ownedTeeth.cend(), [&](const ToothBody& owned) {
                            return !sides.isStrictRight(owned.tip.time) || !sides.isStrictRight(owned.bend.time);
                        })) {
                            subWord.templateTrace.append(form.value("id").toString() +
                                " · shared entry would take a previously matched tooth");
                            continue;
                        }
                    }
                    if (sharedJoins && !teeth.isEmpty() && ownedOpenHeads.contains(letter - 1) &&
                        !getJoinTime(right, context.orientation).isStrictRight(ownedOpenHeads[letter - 1].time))
                        continue;
                    // A close shared seam may still cut around the whole lam
                    // shaft and give its crest to the following letter.
                    const auto previousLam = ownedLamCrests.constFind(letter - 1);
                    if (sharedJoins && previousLam != ownedLamCrests.cend() &&
                        !getJoinTime(right, context.orientation).isStrictRight(previousLam->time)) continue;
                    if (sharedJoins && letter == 1 && !preservesBody(bodyTemplateIds[0], right, true)) continue;
                    if ((!sharedJoins && !before(incoming, right)) || !before(right, left) ||
                        (!open && !nextBody && !internalBody && !forkedLam && std::abs(center(left).x() - exit.x()) > 2 * stroke) ||
                        (letter + 1 < static_cast<int>(cuts.size()) && !before(left, cuts[letter + 1]))) continue;

                    if (form.value("pairedCounters").toBool()) {
                        bool above = false, below = false;
                        const double middle = (center(right).y() + center(left).y()) / 2;
                        for (size_t i = 1; i < context.subPaths.size(); ++i) {
                            const auto bounds = context.subPaths[i].boundingRect();
                            const auto point = bounds.center();
                            if (bounds.left() < center(left).x() || bounds.right() > center(right).x() ||
                                context.transformedPath.contains(point)) continue;
                            above |= point.y() < middle;
                            below |= point.y() > middle;
                        }
                        if (!above || !below) continue;
                    }

                    auto trial = cuts;
                    trial[letter] = left;
                    if (sharedJoins) trial[letter - 1] = right;
                    else trial.insert(trial.begin() + letter, right);
                    const auto& path = shape.path;
                    const auto validCuts = [&](const std::vector<Stretching>& proposal) {
                        auto active = proposal;
                        // A later medial letter can still have an empty reserved
                        // slot. It is not a geometric cut and must not invalidate
                        // the complete body currently being checked.
                        if ((pendingMedialJoin || (deferredFinalJoin && !resolvedFinalJoin)) &&
                            letter + 1 < static_cast<int>(cuts.size())) active.pop_back();
                        const auto regions = cutRegions(path, subWord.text, active, subWord.basePathOrientation, true);
                        return regions.size() == active.size() + 1 &&
                            std::none_of(regions.begin(), regions.end(), [](const GlyphInfo& glyph) {
                                return glyph.path.boundingRect().isEmpty();
                            });
                    };

                    auto finalBoundary = nextBody;
                    auto finalTemplate = nextBodyId;
                    if (sharedExit) {
                        finalBoundary = sharedExit->cut;
                        finalTemplate = sharedExit->templateId;
                    } else if (finalBoundary) {
                        if (!before(left, *finalBoundary)) continue;
                        trial.insert(trial.begin() + letter + (sharedJoins ? 1 : 2), *finalBoundary);
                    } else if (letter + 1 == lastIndex && !outgoing.fixedLeftBoundary) {
                        // Reuse the boundary already matched for this letter. Re-running
                        // its search here starts after that join and can fail to find it.
                        const auto& saved = nextBoundaries[letter];
                        if (saved && before(left, saved->cut)) {
                            trial.push_back(saved->cut);
                            if (validCuts(trial)) {
                                finalBoundary = saved->cut;
                                finalTemplate = saved->templateId;
                            } else trial.pop_back();
                        }
                        // Try the final letter's own boundary before accepting a shared cut.
                        for (const auto& value : finalRules) {
                            if (finalBoundary) break;
                            const auto rule = value.toObject();
                            if (!applies(rule, letter)) continue;
                            context.pairedStrokeWidth = left.referenceWidth > 0 ? left.referenceWidth :
                                QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length();
                            const bool found = probeFinal(rule);
                            context.pairedStrokeWidth = 0;
                            if (!found) continue;
                            if (!bodyTemplateIds[lastIndex].isEmpty() &&
                                !preservesBody(bodyTemplateIds[lastIndex], context.res, false)) continue;
                            if (!before(left, context.res)) {
                                // At a tight tooth/bowl attachment, two normal
                                // probes can straddle the same neck. Share the
                                // final's intrinsic seam only if the tooth keeps
                                // its complete axis and both masks remain valid.
                                if (medial.tooth && rule.value("retainBodyFeatures").toBool() &&
                                    before(right, context.res)) {
                                    const QLineF old(left.topBaseLine.point, left.bottomBaseLine.point);
                                    const QLineF fresh(context.res.topBaseLine.point, context.res.bottomBaseLine.point);
                                    const auto bodies = cutRegions(context.transformedPath, {}, {right, context.res},
                                                                   context.orientation, true);
                                    if (QLineF(old.center(), fresh.center()).length() <=
                                            std::max(old.length(), fresh.length()) &&
                                        bodies.size() == 3 && medial.tooth->retainedBy(bodies[1].path)) {
                                        const size_t exitIndex = letter + (sharedJoins ? 0 : 1);
                                        auto sharedTrial = trial;
                                        sharedTrial[exitIndex] = context.res;
                                        if (validCuts(sharedTrial)) {
                                            trial = std::move(sharedTrial);
                                            left = context.res;
                                            finalBoundary = context.res;
                                            finalTemplate = rule.value("id").toString();
                                            sharedExit = BodyBoundary{context.res, finalTemplate, true};
                                            break;
                                        }
                                    }
                                }
                                // A tight final can overlap the proposed exit of
                                // seen. Keep the final's cut only when it follows
                                // all three tooth tips and reaches the last tip.
                                if (recoverTeeth && !teeth.isEmpty() && !foldedFinal) {
                                    const auto side = getJoinTime(context.res,context.orientation);
                                    const double pitch = QLineF(seenTips.front().point,seenTips.back().point).length()/2;
                                    const auto endpoint = QLineF(context.res.topBaseLine.point,context.res.bottomBaseLine.point);
                                    const bool retained = std::all_of(seenTips.begin(),seenTips.end(),[&](const auto& tip) {
                                        return side.isStrictRight(tip.time) && getJoinTime(right,context.orientation).isStrictLeft(tip.time);
                                    });
                                    if (retained && QLineF(endpoint.center(),seenTips.back().point).length()<=1.5*pitch) {
                                        const size_t exitIndex = letter+(sharedJoins?0:1);
                                        auto sharedTrial=trial; sharedTrial[exitIndex]=context.res;
                                        if (validCuts(sharedTrial)) {
                                            trial=std::move(sharedTrial);
                                            left=context.res;
                                            finalBoundary=context.res;
                                            finalTemplate=rule.value("id").toString();
                                            sharedExit=BodyBoundary{context.res,finalTemplate,true};
                                            break;
                                        }
                                    }
                                }
                                // Independently found boundaries may cross at a
                                // tight join. Reconcile only near-coincident cuts;
                                // the final body's own boundary keeps its tail.
                                const double width = std::min(
                                    QLineF(left.topBaseLine.point, left.bottomBaseLine.point).length(),
                                    QLineF(context.res.topBaseLine.point, context.res.bottomBaseLine.point).length());
                                const double tolerance = config.value("kashidaDefaults").toObject()
                                    .value("sharedCutTolerance").toDouble(0.25) * width;
                                if (QLineF(left.topBaseLine.point, context.res.topBaseLine.point).length() > tolerance ||
                                    QLineF(left.bottomBaseLine.point, context.res.bottomBaseLine.point).length() > tolerance) continue;
                                const size_t exitIndex = letter + (sharedJoins ? 0 : 1);
                                const auto original = trial[exitIndex];
                                trial[exitIndex] = context.res;
                                if (validCuts(trial)) {
                                    left = context.res;
                                    finalBoundary = context.res;
                                    finalTemplate = rule.value("id").toString();
                                    break;
                                }
                                trial[exitIndex] = original;
                                continue;
                            }
                            trial.push_back(context.res);
                            if (validCuts(trial)) {
                                finalBoundary = context.res;
                                finalTemplate = rule.value("id").toString();
                                break;
                            }
                            trial.pop_back();
                        }
                    }
                    if (composeSeenTail) {
                        const auto first = getJoinTime(right, context.orientation);
                        const auto last = getJoinTime(left, context.orientation);
                        const bool keepsTips = std::all_of(seenTips.begin(), seenTips.end(), [&](const auto& tip) {
                            return first.isStrictLeft(tip.time) && last.isStrictRight(tip.time);
                        });
                        const bool ownsTail = std::any_of(finalRules.begin(), finalRules.end(), [&](const auto& value) {
                            const auto rule = value.toObject();
                            return rule.value("id") == finalTemplate && rule.value("ownsLeftTip").toBool();
                        });
                        if (!keepsTips || !ownsTail || !finalBoundary || !before(left, *finalBoundary)) continue;
                    }
                    if (!validCuts(trial)) continue;
                    // A shared exit must still describe the already matched
                    // final body; a medial candidate cannot take its counter
                    // and leave only the tail under the final letter's name.
                    if (letter + 1 == lastIndex && !finalBoundary && !outgoing.fixedLeftBoundary &&
                        !preservesBody(bodyTemplateIds[lastIndex], left, false)) continue;
                    // First prefer an intrinsic lam route that composes with
                    // the final body's own cut. A partial route remains useful
                    // if none of the alternatives can complete that boundary.
                    if (((preferFinalBoundary && pass == 0) || medial.requireFinal) &&
                        !finalBoundary && bodyTemplateIds[lastIndex].isEmpty()) continue;

                    if (hollowCuts && !sharedJoins && !sharedExit &&
                        (!finalBoundary || before(hollowCuts->second, *finalBoundary))) {
                        // Refine only after the original route has passed the full partition check.
                        trial[letter] = hollowCuts->first;
                        trial[letter + 1] = hollowCuts->second;
                        if (validCuts(trial)) {
                            right = hollowCuts->first;
                            left = hollowCuts->second;
                        }
                    }
                    if (enumerate && !plan->accepts(letter, right, left)) continue;
                    if (sharedJoins) {
                        incoming.topBaseLine = incoming.topRight = right.topBaseLine;
                        incoming.bottomBaseLine = incoming.bottomRight = right.bottomBaseLine;
                        incoming.referenceWidth = right.referenceWidth;
                        incoming.fixedRightBoundary = true;
                        incoming.length = 0;
                        incoming.kashidaOverrides.insert("enabled", false);
                    }
                    incoming.topLeft = right.topBaseLine;
                    incoming.bottomLeft = right.bottomBaseLine;
                    incoming.fixedLeftBoundary = true;
                    if (left.counterEntry.pathIndex > 0) {
                        outgoing.counterBendWeights = left.counterBendWeights;
                        outgoing.counterEntry = left.counterEntry;
                        outgoing.counterExit = left.counterExit;
                        outgoing.kashidaOverrides.insert("enabled", false);
                    }
                    outgoing.topBaseLine = left.topBaseLine;
                    outgoing.bottomBaseLine = left.bottomBaseLine;
                    outgoing.referenceWidth = left.referenceWidth;
                    outgoing.fixedRightBoundary = true;
                    outgoing.length = 0;
                    // Sharing this body's entry does not make its exit shared.
                    // Keep the two independent endpoints when the transcript
                    // explicitly includes this connector. The ink between the
                    // endpoints still has to pass the contour checks.
                    const bool markedSpan = hasFinalTatweel && outgoing.fixedLeftBoundary;
                    if (sharedJoins && !forkedLam && !finalBoundary && !markedSpan && letter + 1 == lastIndex)
                        outgoing.kashidaOverrides.insert("enabled", false);
                    if (!finalTemplate.isEmpty()) {
                        bodyTemplateIds[lastIndex] = finalTemplate;
                        if (!subWord.matchedTemplates.contains(finalTemplate))
                            subWord.matchedTemplates.append(finalTemplate);
                        subWord.templateTrace.append(QString("%1 → %2 · %3 · %4")
                            .arg(baseText.mid(letter, 1), baseText.mid(letter + 1, 1), finalTemplate,
                                sharedExit ? "shared ligature cut" : "paired body boundary matched"));
                    }
                    if (sharedExit) {
                        outgoing.topRight = outgoing.topLeft = left.topBaseLine;
                        outgoing.bottomRight = outgoing.bottomLeft = left.bottomBaseLine;
                        outgoing.fixedLeftBoundary = false;
                        outgoing.kashidaOverrides.insert("enabled", false);
                    } else if (finalBoundary) {
                        outgoing.topLeft = finalBoundary->topBaseLine;
                        outgoing.bottomLeft = finalBoundary->bottomBaseLine;
                        outgoing.fixedLeftBoundary = true;
                    }
                    const auto id = form.value("id").toString();
                    subWord.matchedTemplates.append(id);
                    bodyTemplateIds[letter] = id;
                    if (medial.tooth) ownedTeeth.insert(letter, *medial.tooth);
                    if (lamCrest) ownedLamCrests.insert(letter, *lamCrest);
                    if (lamFork) matchedLamForks.insert(letter, *lamFork);
                    if (!openHead.isEmpty()) {
                        const auto entrySide = getJoinTime(right, context.orientation);
                        const auto exitSide = getJoinTime(left, context.orientation);
                        const PathExtrema* crest = nullptr;
                        for (const auto& point : context.timeSorted) {
                            if (entrySide.isStrictLeft(point.time) && exitSide.isStrictRight(point.time) &&
                                (!crest || point.point.y() < crest->point.y())) crest = &point;
                        }
                        if (crest) ownedOpenHeads.insert(letter, *crest);
                    }
                    if (form.value("sharedEntry").toBool() && sharedJoins) {
                        sharedEntries.append(letter);
                        subWord.templateTrace.append(id + " · shared incoming ligature cut");
                    }
                    subWord.templateTrace.append(id + (lamFork ? " · medial fork boundaries matched" :
                        " · medial body boundaries matched"));
                    if (deferred) resolvedFinalJoin = true;
                    if (open) pendingMedialJoin = false;
                    matched = true;
                    break;
                }
                }
                if (matched) break;
            }
            if (open && !pendingMedialJoin && canProposeMedialBoundary(letter + 1)) {
                cuts.emplace_back();
                pendingMedialJoin = true;
            }
        }
        if (pendingMedialJoin || (deferredFinalJoin && !resolvedFinalJoin)) cuts.pop_back();
        storeJoins(cuts);
    }

    // A final form can find its entry independently after the preceding body
    // has supplied its exit. Keep both cuts when they bound an actual ribbon;
    // overlapping proposals at the same attachment share the final's seam.
    if (preserveReadingOrder && subWord.joins.size() == lastIndex && lastIndex > 0 &&
        bodyTemplateIds.back().isEmpty() &&
        std::none_of(bodyTemplateIds.cbegin(), bodyTemplateIds.cend() - 1,
                     [](const QString &id) { return id.isEmpty(); })) {
        const auto original = subWord;
        const auto exit = *original.joins.rbegin();
        const QLineF outside(exit.topBaseLine.point, exit.bottomBaseLine.point);
        for (const auto &value : finalRules) {
            const auto rule = value.toObject();
            if (!rule.value("terminalAttachment").toBool() || !applies(rule, lastIndex - 1) ||
                !attempt(rule, lastIndex - 1, false))
                continue;
            const auto entry = context.res;
            const QLineF inside(entry.topBaseLine.point, entry.bottomBaseLine.point);
            const auto side = getJoinTime(exit, context.orientation);
            const bool ordered =
                side.isStrictLeft(entry.topBaseLine.time) && side.isStrictLeft(entry.bottomBaseLine.time);
            const bool close =
                QLineF(outside.center(), inside.center()).length() <= 1.5 * std::min(outside.length(), inside.length());
            bool touchingCounter = false;
            if (rule.value("stemFoot").toBool() ||
                formDescription(config, rule, "body").value("shape") == "terminal-fork")
                for (size_t i = 1; i < context.subPaths.size(); ++i) {
                    const auto box = context.subPaths[i].boundingRect();
                    if (box.left() >= inside.center().x() - inside.length() * .3 &&
                        QLineF(QPointF(box.left(), std::clamp(inside.center().y(), box.top(), box.bottom())),
                               inside.center())
                                .length() <= inside.length() * 1.5)
                        touchingCounter = true;
                }
            for (bool shared : {false, true}) {
                if (shared ? !(close || touchingCounter) : !ordered)
                    continue;
                subWord = original;
                auto joined = shared ? entry : exit;
                joined.length = 0;
                joined.readingOrder = lastIndex - 1;
                joined.topLeft = entry.topBaseLine;
                joined.bottomLeft = entry.bottomBaseLine;
                joined.fixedRightBoundary = true;
                joined.fixedLeftBoundary = !shared;
                joined.kashidaOverrides.insert("enabled", !shared);
                subWord.joins.erase(std::prev(subWord.joins.end()));
                subWord.joins.insert(joined);
                if (!cutSubword(lineIndex, wordIndex, subWordIndex, config) ||
                    subWord.baseGlyphs.size() != baseText.size() || subWord.kashidaPaths.back().isEmpty() != shared)
                    continue;
                const auto expected = cutRegions(shape.path, {}, {entry}, context.orientation, true);
                if (expected.size() != 2 || subWord.baseGlyphs.back().path != expected[1].path)
                    continue;
                const auto id = rule.value("id").toString();
                bodyTemplateIds.back() = id;
                subWord.matchedTemplates.append(id);
                subWord.templateTrace.append(id +
                                             (shared ? " · shared body attachment" : " · separate body attachment"));
                break;
            }
            if (!bodyTemplateIds.back().isEmpty())
                break;
            subWord = original;
        }
    }

    // A known final yeh can supply the sole boundary of a two-letter ligature.
    // The first body is an unreviewed remainder, not a successful form match.
    const bool remainderCandidate = config.value("provisionalFallbacks").toObject()
        .value("finalYehRemainder").toBool() && baseText.size() == 2 &&
        QStringLiteral("يئى").contains(baseText.back()) && bodyTemplateIds[0].isEmpty() &&
        !bodyTemplateIds[1].isEmpty() && subWord.joins.size() == 1;
    // No first connecting span is justified until the initial body has a
    // matched template. Keep that ink unresolved when only later bodies match.
    if (!subWord.joins.empty() && (remainderCandidate || bodyTemplateIds[0].isEmpty())) {
        auto shared = *subWord.joins.begin();
        shared.length = 0;
        shared.kashidaOverrides.insert("enabled", false);
        subWord.joins.erase(subWord.joins.begin());
        subWord.joins.insert(shared);
    }
	const bool cut = (!deferredFinalJoin || resolvedFinalJoin) && cutSubword(lineIndex, wordIndex, subWordIndex, config);
    if (cut && !subWord.joins.empty() && !bodyTemplateIds[lastIndex].isEmpty() &&
        intrinsicFinalCuts.contains(bodyTemplateIds[lastIndex])) {
        auto actual = *subWord.joins.rbegin();
        if (!subWord.kashidaPaths.back().isEmpty()) {
            actual.topBaseLine = actual.topLeft;
            actual.bottomBaseLine = actual.bottomLeft;
        }
        if (!preservesBody(bodyTemplateIds[lastIndex], actual, false)) {
            subWord.templateTrace.append(bodyTemplateIds[lastIndex] + " · final mask lost its intrinsic body features");
            bodyTemplateIds[lastIndex].clear();
        }
    }
    // A shared cut can leave a nonempty sliver carrying the final template's
    // name while the preceding body takes the entire descending tail.
    if (cut && subWord.baseGlyphs.size() == baseText.size() && !bodyTemplateIds[lastIndex].isEmpty()) {
        for (const auto& value : finalRules) {
            const auto rule = value.toObject();
            if (rule.value("id") != bodyTemplateIds[lastIndex] || !rule.value("ownsLeftTip").toBool()) continue;
            auto finalInk = shape.transform.map(subWord.baseGlyphs.back().path);
            finalInk.translate(shape.pos);
            const double roundoff = 1e-6 * std::max(1., context.bbox.width());
            if (finalInk.boundingRect().left() > context.bbox.left() + roundoff) {
                subWord.templateTrace.append(bodyTemplateIds[lastIndex] + " · final body lost its left tail tip");
                subWord.matchedTemplates.removeAll(bodyTemplateIds[lastIndex]);
                bodyTemplateIds[lastIndex].clear();
            }
            break;
        }
    }
    // Verify the masks as delivered, after coincident joins have been
    // reconciled and connectors have been cut out.
    const auto visibleFinal = visibleFinalCuts.constFind(bodyTemplateIds[lastIndex]);
    if (cut && subWord.joins.size() == size_t(lastIndex) &&
        (!matchedLamForks.isEmpty() || visibleFinal != visibleFinalCuts.cend())) {
        const auto joins = std::vector<Stretching>(subWord.joins.begin(), subWord.joins.end());
        std::vector<std::vector<QBezier>> sourceCurves;
        const auto boundary = [&](int join, bool receiving) -> std::optional<Stretching> {
            auto seam = joins[join];
            if (subWord.kashidaPaths[join].isEmpty()) return seam;
            if (receiving) {
                seam.topBaseLine = seam.topLeft;
                seam.bottomBaseLine = seam.bottomLeft;
            }
            const auto& caps = subWord.kashidaCuts[join];
            const auto& actual = receiving ? caps.second : caps.first;
            if (seam.topBaseLine.point != actual.p1() || seam.bottomBaseLine.point != actual.p2()) {
                if (sourceCurves.empty())
                    for (const auto& contour : context.subPaths) sourceCurves.push_back(baseContourCurves(contour));
                seam.topBaseLine.point = actual.p1();
                seam.bottomBaseLine.point = actual.p2();
                if (!sourceLandmark(seam.topBaseLine, sourceCurves) || !sourceLandmark(seam.bottomBaseLine, sourceCurves))
                    return {};
            }
            return seam;
        };
        const auto retains = [&](const Stretching& entry, const Stretching* exit, const PathExtrema& point) {
            return getJoinTime(entry, context.orientation).isLeft(point.time) &&
                (!exit || getJoinTime(*exit, context.orientation).isRight(point.time));
        };
        for (auto fork = matchedLamForks.cbegin(); fork != matchedLamForks.cend(); ++fork) {
            const int slot = fork.key();
            const auto entry = boundary(slot - 1, true), exit = boundary(slot, false);
            if (entry && exit && retains(*entry, &*exit, *fork) && retains(*entry, &*exit, ownedLamCrests[slot])) continue;
            subWord.templateTrace.append(bodyTemplateIds[slot] + " · cut separates the stem or fork");
            bodyTemplateIds[slot].clear();
        }
        if (visibleFinal != visibleFinalCuts.cend()) {
            const auto seam = boundary(lastIndex - 1, true);
            if (!seam || !interiorChord(context.transformedPath, seam->topBaseLine, seam->bottomBaseLine, seam->topEndpointTolerance) ||
                !retains(*seam, nullptr, visibleFinal->topBaseLine) || !retains(*seam, nullptr, visibleFinal->bottomBaseLine)) {
                subWord.templateTrace.append(bodyTemplateIds[lastIndex] + " · cut separates the visible entry stroke");
                bodyTemplateIds[lastIndex].clear();
            }
        }
        if (std::none_of(bodyTemplateIds.cbegin(), bodyTemplateIds.cend(), [](const auto& id) { return id.isEmpty(); }) &&
            !coversOriginalInk(word, subWord)) {
            for (auto fork = matchedLamForks.cbegin(); fork != matchedLamForks.cend(); ++fork)
                bodyTemplateIds[fork.key()].clear();
            if (visibleFinal != visibleFinalCuts.cend()) bodyTemplateIds[lastIndex].clear();
            subWord.templateTrace.append("Contour features failed the ink ownership check");
        }
    }
    if (cut && subWord.baseGlyphs.size() == baseText.size()) {
        const auto mapped = [&](const QPainterPath& p) { return shape.transform.map(p).translated(shape.pos); };
        if (config.contains("_observedHeadInk")) {
            for (const auto& [slot, _] : missingHeadInk(word, subWord, baseText, config)) {
                subWord.templateTrace.append(bodyTemplateIds[slot] + (slot == 0 ?
                    " · final mask lost the opening of the initial head" : " · connecting ink contains a letter head"));
                bodyTemplateIds[slot].clear();
            }
            for (const auto& [slot, lips] : outlineCache.misplacedHeads) {
                const auto body = mapped(subWord.baseGlyphs[slot].path);
                if (body.contains(lips.p1()) && body.contains(lips.p2())) continue;
                subWord.templateTrace.append(bodyTemplateIds[slot] + " · cut leaves the observed lips outside the body");
                bodyTemplateIds[slot].clear();
            }
        }
        for (auto it = ownedTeeth.cbegin(); it != ownedTeeth.cend(); ++it) {
            auto mask = shape.transform.map(subWord.baseGlyphs[it.key()].path);
            mask.translate(shape.pos);
            if (!it->retainedBy(mask)) {
                subWord.templateTrace.append(bodyTemplateIds[it.key()] + " · final mask lost its tooth");
                bodyTemplateIds[it.key()].clear();
            }
        }
    }
    // A shared ligature entry can reorder both sides of a folded outline.
    // Show that slot as unresolved if the assembled ink has multiple owners.
    if (cut && !sharedEntries.isEmpty() &&
        std::all_of(bodyTemplateIds.cbegin(), bodyTemplateIds.cend(), [](const auto& id) { return !id.isEmpty(); }) &&
        !coversOriginalInk(word, subWord)) {
        for (int slot : sharedEntries) {
            subWord.templateTrace.append(bodyTemplateIds[slot] + " · shared cut failed ink ownership");
            bodyTemplateIds[slot].clear();
        }
    }
    const bool remainder = remainderCandidate && cut && subWord.baseGlyphs.size() == 2 &&
        std::none_of(subWord.baseGlyphs.begin(), subWord.baseGlyphs.end(), [](const auto& body) {
            return body.path.isEmpty() || body.path.boundingRect().isEmpty();
        }) && std::none_of(subWord.kashidaPaths.begin(), subWord.kashidaPaths.end(), [](const auto& path) {
            return !path.isEmpty();
        });

    for (int letter = 0; letter <= lastIndex; ++letter) {
        const auto position = letter == 0 ? QStringLiteral("initial") :
            letter == lastIndex ? QStringLiteral("final") : QStringLiteral("medial");
        QJsonArray candidates;
        for (const auto& value : config.value(position).toArray()) {
            const auto rule = value.toObject();
            if (matchesBody(rule, letter)) candidates.append(rule.value("id"));
        }
        const auto status = remainder && letter == 0 ? QStringLiteral("remainder") :
            !bodyTemplateIds[letter].isEmpty() ? QStringLiteral("matched") :
            candidates.isEmpty() ? QStringLiteral("no-template") :
            bodyAttempted[letter] ? QStringLiteral("no-match") : QStringLiteral("blocked");
        QJsonObject diagnostic{{"letterIndex", letter + 1},
            {"character", baseText.mid(letter, 1)}, {"position", position}, {"status", status},
            {"template", bodyTemplateIds[letter]}, {"candidates", candidates}};
        if (matchedLamForks.contains(letter)) {
            diagnostic.insert("bodyForm", "lam-fork");
            diagnostic.insert("cutPolicy", "lam-fork-flanks");
            diagnostic.insert("boundaryKind", "lam-fork");
        }
        if (status == "remainder") {
            diagnostic.insert("inferredFrom", bodyTemplateIds[1]);
            diagnostic.insert("reviewRequired", true);
            diagnostic.insert("templateStatus", candidates.isEmpty() ? "no-template" : "no-match");
            subWord.templateTrace.append(QString("initial %1 · provisional remainder after %2 · needs review; initial form still missing")
                .arg(baseText.left(1), bodyTemplateIds[1]));
        } else if (status != "matched") subWord.templateTrace.append(QString("%1 %2 · %3")
            .arg(position, baseText.mid(letter, 1), status));
        subWord.letterDiagnostics.append(diagnostic);
    }
    subWord.correct = cut && subWord.joins.size() == lastIndex &&
        std::all_of(bodyTemplateIds.begin(), bodyTemplateIds.end(), [](const auto& id) { return !id.isEmpty(); });

    if (subWord.correct && lastIndex >= 2) for (const auto& value : medialForms) {
        const auto rule = value.toObject();
        const auto attachment = rule.value("upperAttachment").toObject();
        if (attachment.isEmpty()) continue;
        for (int i = 1; i < lastIndex; ++i) {
            if (!rule.value("current").toString().contains(baseText[i]) ||
                !attachment.value("letters").toString().contains(baseText[i-1])) continue;
            const auto& glyphs = subWord.baseGlyphs;
            const auto mapped = [&](int index) { return shape.transform.map(glyphs[index].path).translated(shape.pos); };
            auto original = std::next(subWord.joins.begin(), i-1);
            const auto shared = stackedAttachment(context, attachment, mapped(i-1).boundingRect(),
                                                 mapped(i), *original, *std::next(original));
            if (!shared) continue;
            auto previous = subWord;
            subWord.joins.erase(original);
            const auto inserted = subWord.joins.insert(*shared);
            bool accepted = inserted.second && std::distance(subWord.joins.begin(), inserted.first) == i-1 &&
                cutSubword(lineIndex, wordIndex, subWordIndex, config) && glyphs.size() == previous.baseGlyphs.size() &&
                subWord.kashidaPaths[i-1].isEmpty() &&
                std::abs(mapped(i-1).boundingRect().bottom() - shared->bottomBaseLine.point.y()) < 1e-5;
            // Only the two touching bodies and their shared join may change.
            for (int j = 0; accepted && j <= lastIndex; ++j) {
                if (j != i-1 && j != i) accepted = glyphs[j].path == previous.baseGlyphs[j].path;
                if (j < lastIndex && j != i-1)
                    accepted &= subWord.kashidaPaths[j] == previous.kashidaPaths[j] &&
                                subWord.kashidaCuts[j] == previous.kashidaCuts[j];
            }
            if (!accepted) { subWord = std::move(previous); continue; }
            const auto id = rule.value("id").toString();
            auto slot = subWord.letterDiagnostics[i].toObject();
            slot.insert("entryTemplate", id);
            subWord.letterDiagnostics[i] = slot;
            if (!subWord.matchedTemplates.contains(id)) subWord.matchedTemplates.append(id);
            subWord.templateTrace.append(id + " · shared horizontal cut at the upper-stroke attachment");
        }
    }

    if (subWord.correct && config.value("_checkRecoveredCuts").toBool() &&
        (!plausibleRecoveredBodies(word, subWord, config.value("recoveryChecks").toObject()) ||
         !coversOriginalInk(word, subWord))) {
        subWord.correct = false;
        subWord.templateTrace.append("Recovered cuts failed the body or ink ownership check");
    }
    if (!subWord.correct && !enumerate && !retryRule.isEmpty()) {
        auto alternative = config;
        for (const auto* phase : {"initial", "final"}) {
            QJsonArray rules;
            for (const auto& rule : config.value(phase).toArray()) {
                if (rule.toObject().value("id").toString() != retryRule) rules.append(rule);
            }
            alternative.insert(phase, rules);
        }
        auto partial = subWord;
        if (segmentSubwordPass(pageIndex, lineIndex, wordIndex, subWordIndex, font, true, &alternative, recoverTeeth, outlineCache, plan, seenRetry)) {
            subWord.templateTrace.prepend(retryRule + " · retried after partial segmentation");
        } else {
            subWord = std::move(partial);
        }
    }

	return subWord.correct;
}

LetterFormInspection inspectLetterForm(PageAnalysisResult& sample, int page, int line, int wordIndex,
        int subwordIndex, AFont* font, const QJsonObject& config, const QString& id) {
    LetterFormInspection result;
    result.formId = id;
    const auto form = resolvedLetterForm(config, id);
    result.body = form.value("body").toObject();
    result.cuts = form.value("cuts").toObject();
    const auto& word = sample.page.lines[line].words[wordIndex];
    const auto& sub = word.wordResultInfo.subWords[subwordIndex];
    const auto shape = word.bodyShape(sub);
    result.ink = shape.transform.map(shape.path).translated(shape.pos);
    result.ink.setFillRule(Qt::WindingFill);
    struct Capture {
        LetterFormInspection* previous = formInspection;
        bool active = inspectingForm;
        bool coordinates = inspectionCoordinates;
        explicit Capture(LetterFormInspection& result) {
            formInspection = &result; inspectingForm = false; inspectionCoordinates = true;
        }
        ~Capture() { formInspection = previous; inspectingForm = active; inspectionCoordinates = coordinates; }
    } capture(result);
    result.complete = sample.segmentSubword(page, line, wordIndex, subwordIndex, font, true, &config);
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    const auto joins = std::vector<Stretching>(sub.joins.begin(), sub.joins.end());
    for (int index = 0; index < sub.letterDiagnostics.size(); ++index) {
        const auto slot = sub.letterDiagnostics[index].toObject();
        const auto selected = slot.value("template").toString();
        if (selected != id && !slot.value("candidates").toArray().contains(id)) continue;
        result.assembly.append(QString("Letter %1 · %2 · %3 · %4%5")
            .arg(index + 1).arg(slot.value("character").toString(), slot.value("position").toString(),
                 slot.value("status").toString(), selected.isEmpty() ? QString{} : " · selected " + selected));
        if (selected != id) continue;
        if (index < int(sub.baseGlyphs.size()))
            result.selectedBody.addPath(shape.transform.map(sub.baseGlyphs[index].path).translated(shape.pos));
        LetterFormInspection::Candidate selectedCuts{QString("Selected letter %1 (assembled)").arg(index + 1), {}};
        const auto appendBoundary = [&](int joinIndex, bool receiving) {
            auto boundary = joins[joinIndex];
            if (joinIndex < int(sub.kashidaPaths.size()) && !sub.kashidaPaths[joinIndex].isEmpty()) {
                const auto& caps = sub.kashidaCuts[joinIndex];
                const auto& actual = receiving ? caps.second : caps.first;
                boundary.topBaseLine.point = actual.p1();
                boundary.bottomBaseLine.point = actual.p2();
            }
            for (const auto& cut : boundary.cutSegments()) selectedCuts.cuts.append(cut);
        };
        if (index > 0 && index - 1 < int(joins.size())) appendBoundary(index - 1, true);
        if (index < letters.size() - 1 && index < int(joins.size())) appendBoundary(index, false);
        result.candidates.prepend(selectedCuts);
        for (const auto* field : {"bodyForm", "cutPolicy", "entryPolicy", "junctionPolicy", "entryFork", "entryWalk"})
            if (slot.contains(field)) result.assembly.append(QString::fromLatin1(field) + ": " +
                slot.value(field).toVariant().toString());
    }
    if (!result.attempted) result.assembly.append("This template was not reached in this run (position, priority or recovery stage). This is not a recognition rejection.");
    if (result.assembly.isEmpty()) result.assembly.append("No letter selected this template in the assembled result.");
    // Keep the full trace: later recovery/assembly can reject a valid local
    // proposal. A failed alternative is never presented as the selected cut.
    result.trace = sub.templateTrace;
    return result;
}
