#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

/*based on https://www.particleincell.com/2013/cubic-line-intersection/
and http://mysite.verizon.net/res148h4j/javascript/script_exact_cubic.html#the%20source%20code */
std::span<double> cubicRoots(double a, double b, double c, double d, std::array<double, 3>& storage) {

	size_t count = 0;
	const auto roots = [&] { return std::span(storage).first(count); };
	// A cubic Bezier can reduce to a quadratic or line in the probe's
	// direction. Cardano's formula divides by a and loses those intersections.
	const double scale = std::max({std::abs(a), std::abs(b), std::abs(c), std::abs(d)});
	if (scale == 0) return {}; // Coincident curves have no isolated root.
	const double tolerance = 64 * std::numeric_limits<double>::epsilon() * scale;
	if (std::abs(a) <= tolerance) {
		const auto add = [&](double t) {
			if (!std::isfinite(t) || t < -1e-12 || t > 1 + 1e-12) return;
			t = std::clamp(t, 0.0, 1.0);
			const auto existing = roots();
			if (std::none_of(existing.begin(), existing.end(), [&](double other) { return std::abs(t - other) < 1e-12; }))
				storage[count++] = t;
		};
		if (std::abs(b) <= tolerance) {
			if (std::abs(c) > tolerance) add(-d / c);
			return roots();
		}
		const double qa = b / scale, qb = c / scale, qc = d / scale;
		double discriminant = std::fma(-4 * qa, qc, qb * qb);
		const double error = 64 * std::numeric_limits<double>::epsilon() *
			(std::abs(qb * qb) + std::abs(4 * qa * qc));
		if (discriminant < -error) return {};
		discriminant = std::max(0.0, discriminant);
		const double q = -0.5 * (qb + std::copysign(std::sqrt(discriminant), qb));
		if (q == 0) add(-qb / (2 * qa));
		else { add(q / qa); add(qc / q); }
		const auto result = roots();
		std::sort(result.begin(), result.end());
		return result;
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
		storage[count++] = t[i];
	}

	return roots();
}

void FindInterections(const QBezier& b, const QLineF& line, int pathIndex, int curveIndex,
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

	std::array<double, 3> roots;
	auto result = cubicRoots(vx * Ax + vy * Ay,
		vx * Bx + vy * By,
		vx * Cx + vy * Cy,
		vx * Dx + vy * Dy - d, roots);

	for (auto t : result) {
		if (0 > t || t > 1) continue;

		auto point = QPointF{ ((Ax * t + Bx) * t + Cx) * t + Dx, ((Ay * t + By) * t + Cy) * t + Dy };

		if (probeBounds && (point.x() < probeBounds->left || point.x() > probeBounds->right ||
                            point.y() < probeBounds->top || point.y() > probeBounds->bottom)) continue;

                PathExtrema intersect{};

                intersect.pathIndex = pathIndex;
		intersect.time = curveIndex + t;
		intersect.point = point;
		intersections.push_back(intersect);

	}
}

std::vector<PathExtrema> FindInterections(const QPainterPath& path, const QLineF& line, bool segment) {

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
                            PathExtrema intersect{};
                            intersect.pathIndex = pathIndex;
                            QLineF newLine{path.elementAt(i - 1), intersetion};
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

} // namespace MedinaCutter
