#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

/*FROM qpathclipper.cpp */
QVector<QPainterPath> toSubpaths(const QPainterPath& path) {
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

std::vector<GlyphInfo> computeCutRegions(const QPainterPath& path, const QString& text,
                                         const std::vector<Stretching>& cuts, Orientation orientation,
                                         bool checkSegmentOrder) {
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
                (checkSegmentOrder && i >= 2 && std::floor(nextPoints[i - 2]) == std::floor(nextPoints[i]))) {
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

std::vector<GlyphInfo> cutRegions(const QPainterPath& path, const QString& text,
                                       const std::vector<Stretching>& cuts, Orientation orientation,
                                       bool checkSegmentOrder) {
    if (cutGeometryMemo && cutGeometryMemo->activeForm) ++cutGeometryMemo->activeForm->partitions;
    if (!cutGeometryMemo || cuts.empty()) return computeCutRegions(path, text, cuts, orientation, checkSegmentOrder);
    const int source = cutGeometryMemo->outlineId(path);
    if (source < 0) return computeCutRegions(path, text, cuts, orientation, checkSegmentOrder);
    QByteArray key;
    key.reserve(int(16 + cuts.size() * 80));
    appendGeometryKey(key, source);
    appendGeometryKey(key, orientation);
    appendGeometryKey(key, checkSegmentOrder);
    for (const auto& cut : cuts) {
        // These are all the fields consumed by computeCutRegions and its
        // reading-order/shared-fork checks. Pen width and template identity
        // cannot change a partition at these same contour endpoints.
        appendGeometryAnchor(key, cut.topBaseLine);
        appendGeometryAnchor(key, cut.bottomBaseLine);
        appendGeometryKey(key, cut.counterEntry.pathIndex);
        if (cut.counterEntry.pathIndex > 0) {
            appendGeometryAnchor(key, cut.counterEntry);
            appendGeometryAnchor(key, cut.counterExit);
            appendGeometryPoint(key, cut.counterBendWeights);
        }
    }
    if (const auto* saved = cutGeometryMemo->partitions.object(key)) {
        ++cutGeometryMemo->partitionHits;
        auto result = *saved;
        for (auto& glyph : result) glyph.text = text;
        return result;
    }
    ++cutGeometryMemo->partitionMisses;
    auto result = computeCutRegions(path, text, cuts, orientation, checkSegmentOrder);
    size_t cost = 128 + key.size() + result.size() * sizeof(GlyphInfo);
    for (const auto& glyph : result) cost += size_t(glyph.path.elementCount()) * sizeof(QPainterPath::Element);
    if (cost <= size_t(cutGeometryMemo->partitions.maxCost()))
        cutGeometryMemo->partitions.insert(key, new std::vector<GlyphInfo>(result), int(cost));
    return result;
}

bool computeInteriorInkSegment(const QPainterPath& ink, const QPointF& top, const QPointF& bottom,
                                     double topTolerance) {
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

bool interiorInkSegment(const QPainterPath& ink, const QPointF& top, const QPointF& bottom,
                               double topTolerance) {
    if (!cutGeometryMemo) return computeInteriorInkSegment(ink, top, bottom, topTolerance);
    const int source = cutGeometryMemo->outlineId(ink);
    if (source < 0) return computeInteriorInkSegment(ink, top, bottom, topTolerance);
    QByteArray key;
    key.reserve(48);
    appendGeometryKey(key, source);
    appendGeometryPoint(key, top);
    appendGeometryPoint(key, bottom);
    appendGeometryKey(key, cutGeometryMemo->preciseGeometry);
    appendGeometryKey(key, topTolerance);
    if (const auto* saved = cutGeometryMemo->inkSegments.object(key)) {
        ++cutGeometryMemo->segmentHits;
        return *saved;
    }
    ++cutGeometryMemo->segmentMisses;
    const bool result = computeInteriorInkSegment(ink, top, bottom, topTolerance);
    cutGeometryMemo->inkSegments.insert(key, new bool(result));
    return result;
}

bool interiorChord(const QPainterPath& ink, const PathExtrema& top, const PathExtrema& bottom,
                          double topTolerance) {
    return top.pathIndex == 0 && bottom.pathIndex == 0 &&
        std::isfinite(top.time) && std::isfinite(bottom.time) &&
        top.time >= 0 && bottom.time >= 0 && top.time != bottom.time &&
        interiorInkSegment(ink, top.point, bottom.point, topTolerance);
}

} // namespace MedinaCutter
