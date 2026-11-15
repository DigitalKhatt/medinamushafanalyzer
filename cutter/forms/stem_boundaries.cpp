#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

MedialStemEvidence recognizeMedialStems(const InkOutline &outline, const QJsonObject &body) {
    MedialStemEvidence result;
    result.source = outline.ink;
    const bool turningShaft = body.value("shape") == "turning-shaft";
    if (!turningShaft && body.value("shape") != "forked-stem") return result;
    if (turningShaft) result.turningShafts = recognizeTurningShafts(outline, body.value("shaft").toObject());
    WordResultInfo::SubWordInfo empty;
    auto geometry = outline.geometry(empty);
    result.features = recognizeStemForkEvidence(geometry, body.value("minFlankSpan").toDouble(), true);
    const auto routes = body.value("exitRoutes").toArray();
    const auto curves = baseContourCurves(geometry.transformedPath);
    for (const auto& crest : result.features.crests) {
        MedialStem stem{crest, {}, {}, {}, stemForks(geometry, crest, result.features.forks, curves)};
        std::erase_if(stem.forks, [&](size_t i) {
            return !result.features.forks[i].isConcave(geometry.orientation);
        });
        if (turningShaft) {
            if (!stem.forks.empty()) result.stems.push_back(std::move(stem));
            continue;
        }
        stem.descendingFlanks = traceStemEdges(geometry, crest, false);
        if (body.value("entryRoute").isObject())
            for (int reverse = 0; reverse < 2; ++reverse)
                stem.entryRoutes[reverse] =
                    recognizeSavedRoute(geometry, body.value("entryRoute").toObject(), &crest, reverse);
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

void inspectStemEvidence(const MedialStemEvidence& evidence) {
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

// A shaft and its own fork supply the only anchors. Cut policies can walk
// from those landmarks or use the turn directly at a tight attachment.
std::vector<MedialCandidate> turningShaftCuts(SearchContext& context,
        const MedialStemEvidence& evidence, const QJsonObject& shape, const QJsonObject& cuts) {
    std::vector<MedialCandidate> candidates;
    const auto& ink = context.transformedPath;
    const auto policy = cuts.value("shaft").toObject();
    const auto maxAspect = shape.value("maxWidthToHeight").toDouble();
    const auto minRise = shape.value("minRise").toDouble();
    const auto forkWalk = cuts.value("forkOffset").toDouble();
    const auto walkWidths = policy.value("walkWidths").toDouble();
    const int count = int(baseContourCurves(ink).size());
    const int rightFlank = context.orientation == Orientation::ClockWise ? 0 : 1;
    const auto add = [&](const PathExtrema& crest, Stretching right, Stretching left,
                         const PathExtrema* fork = nullptr) {
        orientSection(context, right, crest.time, true);
        orientSection(context, left, crest.time, false);
        if (!cutPrecedes(right, left, context.orientation)) return;
        const QLineF entry(right.topBaseLine.point, right.bottomBaseLine.point);
        const QLineF exit(left.topBaseLine.point, left.bottomBaseLine.point);
        if (!sharesLowerForkTip(right, left) && entry.intersects(exit, nullptr) == QLineF::BoundedIntersection) return;
        const auto rightSide = getJoinTime(right, context.orientation);
        const auto leftSide = getJoinTime(left, context.orientation);
        if (!rightSide.isStrictLeft(crest.time) || !leftSide.isStrictRight(crest.time) ||
            (fork && (!rightSide.isLeft(fork->time) || !leftSide.isRight(fork->time)))) return;
        const auto pieces = cutRegions(ink, {}, {right, left}, context.orientation, true);
        if (pieces.size() != 3 || toSubpaths(pieces[1].path).size() != 1) return;
        const auto bounds = pieces[1].path.boundingRect();
        if (bounds.width() > maxAspect * bounds.height() || std::abs(bounds.top() - crest.point.y()) > 1e-6) return;
        candidates.emplace_back(right, left, false, std::nullopt, crest,
                                fork ? std::optional{*fork} : std::nullopt);
    };

    for (const auto& stem : evidence.stems) {
        for (const auto index : stem.forks) {
            const auto& fork = evidence.features.forks[index];
            if (fork.tip.point.y() - stem.crest.point.y() < minRise) continue;
            const auto placeFork = [&](double distance) {
                const auto right = cutForkFlank(context, fork.tip, {}, fork.rightForward, 1, distance);
                const auto left = cutForkFlank(context, fork.tip, {}, !fork.rightForward, -1, distance);
                if (right && left) add(stem.crest, *right, *left, &fork.tip);
            };
            if (forkWalk > 0) placeFork(forkWalk);
            if (cuts.value("tightJoin") == "tip-normal") placeFork(0);
        }
    }
    for (const auto& shaft : evidence.turningShafts) {
        const auto normal = [&](int side, double distance) -> std::optional<Stretching> {
            auto anchor = *shaft.turns[side];
            const bool forward = side == 0;
            if (distance > 0) {
                auto limit = anchor;
                limit.time += forward ? count : -count;
                QPointF tangent;
                if (!advanceOnContour(ink, anchor, limit, forward, distance, anchor, tangent)) return {};
            }
            Stretching section{};
            if (!contourNormalCut(context, anchor, forward, false, section) ||
                section.referenceWidth < .4 * shaft.stroke || section.referenceWidth > 3.2 * shaft.stroke) return {};
            return section;
        };
        const double walk = walkWidths * shaft.stroke;
        const auto exit = normal(1 - rightFlank, walk);
        const auto entry = normal(rightFlank, walk);
        if (entry && exit) add(shaft.crest, *entry, *exit);
        if (walkWidths > 0 && policy.value("atTurnWhenTight").toBool()) {
            const auto right = normal(rightFlank, 0);
            const auto left = normal(1 - rightFlank, 0);
            if (right && left) add(shaft.crest, *right, *left);
        }
        if (!exit || !policy.value("forkJunction").toBool()) continue;
        const auto stem = std::ranges::find_if(evidence.stems, [&](const auto& stem) {
            return stem.crest.time == shaft.crest.time;
        });
        if (stem == evidence.stems.end()) continue;
        for (const auto index : stem->forks) {
            const auto& upper = *shaft.turns[rightFlank];
            const auto& lower = evidence.features.forks[index].tip;
            const auto width = QLineF(upper.point, lower.point).length();
            if (width < .4 * shaft.stroke || width > 3.2 * shaft.stroke || !interiorChord(ink, upper, lower)) continue;
            Stretching junction{};
            junction.topBaseLine = upper;
            junction.bottomBaseLine = lower;
            junction.referenceWidth = width;
            add(shaft.crest, junction, *exit, &lower);
        }
    }
    return candidates;
}

} // namespace MedinaCutter
