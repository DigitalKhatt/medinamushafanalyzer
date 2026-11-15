#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

bool within(const QJsonObject& bounds, const char* min, const char* max, double value) {
    return (!bounds.contains(min) || value >= bounds.value(min).toDouble()) &&
           (!bounds.contains(max) || value <= bounds.value(max).toDouble());
}

// Qt page coordinates increase rightward and downward.
bool matchesOffset(const QJsonObject& bounds, const QPointF& delta) {
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

bool matchesNeighbors(const QJsonArray& relations, const Stretching& join) {
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

bool matchesJoinFeatures(SearchContext& context) {
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

static bool retainsTailShoulder(SearchContext& context, const QJsonObject& rule, const Stretching& boundary);

bool matchesBoundaryBody(SearchContext& context, const QJsonObject& rule,
                                const Stretching& boundary, bool initial, const QJsonObject& initialLips,
                                QString* reason) {
    const auto reject = [&](const QString& message) {
        if (reason) *reason = message;
        return false;
    };
    const auto features = rule.value("bodyFeatures").toObject();
    const auto shape = rule.value("body").toObject().value("shape");
    const bool finalCounter = !initial && (shape == "counter-neck" || shape == "counter-shoulder");
    const bool protectOpening = initial && !initialLips.isEmpty();
    const bool protectTail = !initial && rule.value("ownsLeftTip").toBool();
    if (!initial && shape == "open-bowl") {
        const auto evidence = recognizedSharedShape(context, rule.value("body").toObject());
        if (!evidence->openBowl) return reject("bowl shoulder not found");
        if (!retainsOpenBowl(context, *evidence->openBowl, boundary))
            return reject("cut separates the bowl shoulder, crest or floor");
    }
    if (!initial && shape == "tail-branch" && !retainsTailShoulder(context, rule, boundary))
        return reject("cut separates the bowl shoulder");
    if (features.isEmpty() && !finalCounter && !protectOpening && !protectTail) return true;
    const auto pieces = cutRegions(context.transformedPath, {}, {boundary}, context.orientation, true);
    if (pieces.size() != 2) return false;
    if (protectTail && pieces[1].path.boundingRect().left() > context.bbox.left() + 1e-6)
        return reject("cut separates the terminal tail");
    if (protectOpening && !containsHeadLips(pieces[0].path, initialLips))
        return reject("cut separates the lips of the initial head");
    const auto bounds = pieces[initial ? 0 : 1].path.boundingRect();
    if (bounds.isEmpty()) return false;
    if (bounds.width() < features.value("minWidth").toDouble())
        return reject("body is narrower than its minimum width");
    if (bounds.width() * bounds.height() < features.value("minBoxArea").toDouble())
        return reject("body bounding box is smaller than its minimum area");
    if (finalCounter) {
        const auto evidence = recognizedSharedShape(context, rule.value("body").toObject());
        const auto& holes = evidence->holes;
        const auto counter = std::min_element(holes.begin(), holes.end(), [](const auto& a, const auto& b) {
            return a.box.center().x() < b.box.center().x();
        });
        if (counter == holes.end() || !toSubpaths(pieces[1].path).contains(context.subPaths[counter->index]))
            return reject("body counter ownership rejected");
        const auto extent = bounds.adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
        const auto entry = rule.value("cuts").toObject().value("boundaries").toObject().value("right");
        if (shape == "counter-neck" && entry == "counter-neck" &&
            (!extent.contains(context.xSorted.rbegin()->point) || extent.contains(context.xSorted.begin()->point)))
            return reject("cut separates the free end or takes the initial stroke");
    }
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

SharedShapeEvidence recognizeSharedShape(const InkOutline &outline, const QJsonObject &body,
                                         StrokeNormalCache* normals) {
    SharedShapeEvidence result;
    result.source = outline.ink;
    const auto shape = body.value("shape");
    if (shape == "open-bowl")
        result.openBowl = recognizeOpenBowl(outline, body.value("bowl").toObject());
    else if (shape == "contour-route") {
        WordResultInfo::SubWordInfo empty;
        auto geometry = outline.geometry(empty);
        const auto route = body.value("route").toObject();
        result.route = recognizeSavedRoute(geometry, route);
        if (route.contains("shoulder"))
            result.tail = recognizeTailBranches(outline, route.value("shoulder").toObject());
    } else if (shape == "suspended-loop")
        result.head = recognizeLoopHead(outline, body.value("head").toObject());
    else if (shape == "crested-loop")
        result.crestedHeads = recognizeCrestedLoops(outline, body.value("head").toObject());
    else if (shape == "bowl-turn")
        result.bowlTurn = recognizeBowlTurn(outline, body.value("junction").toObject());
    else if (shape == "separate-components")
        result.components = recognizeSourceComponents(outline.ink);
    else if (shape == "component-arm")
        result.componentArms = recognizeComponentArms(outline, body.value("arm").toObject());
    else if (shape == "tapered-arm")
        result.taperedArms = recognizeTaperedArms(outline, body.value("arm").toObject());
    else if (shape == "upright-stem") {
        result.uprightStem = recognizeUprightStem(outline);
        result.descendingShaft = recognizeDescendingShaft(outline);
    } else if (shape == "layered-head")
        result.layeredHead = recognizeLayeredHeads(outline, body);
    else if (shape == "descending-stem")
        result.descendingStemSection = recognizeDescendingStemSection(outline, body.value("stem").toObject());
    else if (hasStemFlanks(body))
        result.medialStem = recognizeMedialStems(outline, body);
    else if (shape == "counter-attachment")
        result.attachment = recognizeCounterAttachments(outline, body.value("attachment").toObject());
    else if (shape == "forked-bowl")
        result.bowlForks = recognizeBowlForks(outline);
    else if (shape == "stemmed-bowl")
        result.terminalStems = recognizeTerminalStemBodies(outline, body.value("stem").toObject());
    else if (shape == "inclined-blade")
        result.blades = recognizeBlades(outline, body.value("blade").toObject());
    else if (shape == "parallel-return")
        result.parallelReturns = recognizeParallelReturns(outline, body.value("bands").toObject());
    else if (shape == "returning-lips")
        result.mouths = recognizeOpenMouths(outline, body.value("mouth").toObject());
    else if (shape == "enclosed-lips")
        result.enclosedLips = recognizeEnclosedLips(outline, body.value("head").toObject());
    else if (shape == "tooth-cluster")
        result.clusters = recognizeToothClusters(outline, body.value("cluster").toObject());
    else if (toothPairDescription(body))
        result.toothPairs = recognizeToothBodies(outline, body, normals);
    else if (bentToothDescription(body))
        result.bentTeeth = recognizeBentToothBodies(outline, body);
    else if (shape == "profile-neck")
        result.bowlNecks = recognizeProfileNecks(outline, body);
    else if (shape == "paired-route")
        result.pairedRoutes = recognizePairedRoutes(outline, body.value("routes").toObject());
    else if (shape == "tail-branch") {
        auto tail = body.value("tail").toObject();
        tail.insert("maxShoulderBacktrackWidths", body.value("maxShoulderBacktrackWidths"));
        result.tail = recognizeTailBranches(outline, tail);
    } else if (shape == "folded-return")
        result.returnSeam = recognizeFoldedReturn(outline, body.value("return").toObject());
    else if (shape == "rising-return")
        result.returnSeam = recognizeRisingReturn(outline, body.value("return").toObject());
    else if (shape == "horizontal-counters")
        result.horizontalPair = recognizeHorizontalPair(outline);
    else if (shape == "stacked-counters")
        result.stackedPairs = recognizeStackedPairs(outline, body.value("pair").toObject());
    else if (shape == "open-head")
        result.openHeads = recognizeOpenHeads(outline, body.value("head").toObject());
    else if (shape == "counter-loop")
        result.loops = recognizeLoopBodies(outline, body.value("loop").toObject());
    else if (shape == "counter-rim")
        result.rims = recognizeRimBodies(outline);
    else if (shape == "counter-outline")
        result.outlines = recognizeCounterOutlines(outline, body);
    if (shape == "counter-neck" || shape == "counter-shoulder") {
        result.holes = recognizeCounterHoles(outline);
        for (size_t i = 1; i < outline.contours.size(); ++i) {
            CounterHole hole{i, outline.contours[i].boundingRect()};
            if (shape != "counter-shoulder" && std::none_of(result.holes.begin(), result.holes.end(),
                    [&](const auto& valid) { return valid.index == i; })) continue;
            if (!result.boundaryCounter || hole.box.center().x() < result.boundaryCounter->box.center().x())
                result.boundaryCounter = hole;
        }
    }
    if (shape == "counter-neck") {
        const auto spec = body.value("counter").toObject();
        result.initialNeck = recognizeInitialCounterBody(outline, result.holes, spec);
        if (result.boundaryCounter)
            result.terminalNeck =
                recognizeCounterCorners(outline, result.boundaryCounter->box, spec.value("entryCorner").toObject());
        if (body.contains("junction"))
            result.bowlTurn = recognizeBowlTurn(outline, body.value("junction").toObject());
    } else if (shape == "counter-shoulder" && result.boundaryCounter)
        result.shoulders = recognizeLoopShoulders(outline, result.boundaryCounter->box, body.value("shoulder").toObject());
    return result;
}

std::shared_ptr<const SharedShapeEvidence> recognizedSharedShape(SearchContext &context,
                                                                        const QJsonObject &body) {
    const auto generate = [&] {
        auto* normals = context.outlineCache ? &context.outlineCache->normalsByCenter : nullptr;
        return std::make_shared<const SharedShapeEvidence>(recognizeSharedShape(inkOutline(context), body, normals));
    };
    if (!context.outlineCache)
        return generate();
    const auto key = std::make_pair(bodyDescriptionKey(body), static_cast<int>(context.orientation));
    auto &evidence = context.outlineCache->sharedBodies[key];
    if (!evidence || evidence->source != context.transformedPath ||
        evidence->source.fillRule() != context.transformedPath.fillRule())
        evidence = generate();
    return evidence;
}

static bool retainsTailShoulder(SearchContext& context, const QJsonObject& rule, const Stretching& boundary) {
    const auto evidence = recognizedSharedShape(context, rule.value("body").toObject());
    Stretching intrinsic{};
    PathExtrema shoulder{};
    if (!evidence->tail || !cutTailBranches(context, *evidence->tail, shapeSettings(rule, "tail"), intrinsic, &shoulder))
        return false;
    // Reconciled cuts may share the attachment, but the bowl's own shoulder
    // cannot become a neighboring tooth. Test contour ownership, not distance
    // to another letter's proposed boundary.
    return getJoinTime(boundary, context.orientation).isLeft(shoulder.time);
}

void inspectSharedShape(const SharedShapeEvidence &evidence) {
    if (!inspectingForm)
        return;
    const auto corner = [&](const auto &c) {
        for (const auto &p : c.corners)
            inspectLandmark("Neck corner", p.anchor);
    };
    if (evidence.route) {
        inspectLandmark("Route origin", evidence.route->origin);
        inspectLandmark("Route anchor", evidence.route->anchor);
    }
    if (evidence.initialNeck) {
        for (const auto &p : evidence.initialNeck->valleys)
            inspectLandmark("Loop valley", p);
        for (const auto &p : evidence.initialNeck->normalAnchors)
            inspectLandmark("Loop corner", p);
    }
    if (evidence.head) {
        if (evidence.head->edge)
            inspectLandmark("Head edge", *evidence.head->edge);
        if (evidence.head->descendingTurn)
            inspectLandmark("Descending loop turn", evidence.head->descendingTurn->point);
        for (const auto &neck : evidence.head->necks)
            inspectLandmark("Head crest", neck.crest);
    }
    for (const auto &head : evidence.crestedHeads) {
        inspectLandmark("Head crest", head.crest);
        inspectLandmark("Right edge", head.rightEdge);
        inspectLandmark("Left edge", head.leftEdge);
    }
    for (const auto &neck : evidence.bowlNecks) {
        inspectLandmark("Bowl neck", neck.section.topBaseLine);
        inspectLandmark("Bowl neck", neck.section.bottomBaseLine);
    }
    for (const auto& cluster : evidence.clusters)
        for (size_t i = 0; i < cluster.turns.size(); ++i)
            inspectLandmark(i % 2 == 0 ? "Tooth tip" : "Tooth valley", cluster.turns[i]);
    for (const auto &head : evidence.enclosedLips) {
        inspectLandmark("Head nose", head.nose);
        for (const auto &side : head.corners)
            for (const auto &point : side)
                inspectLandmark("Lip corner", point);
    }
    for (const auto &mouth : evidence.mouths) {
        inspectLandmark("Mouth notch", mouth.lips.notch);
        inspectLandmark("Upper lip", mouth.lips.upper);
        inspectLandmark("Lower lip", mouth.lips.lower);
        inspectLandmark("Nose", mouth.nose);
    }
    for (const auto &blade : evidence.blades) {
        inspectLandmark("Blade tip", blade.tip);
        inspectLandmark("Elbow", blade.elbow);
        inspectLandmark("Fork", blade.fork);
    }
    for (const auto &band : evidence.parallelReturns) {
        inspectLandmark("Return edge", band.edge);
        inspectLandmark("Elbow", band.elbow);
        inspectLandmark("Blade tip", band.tip);
    }
    for (const auto &stem : evidence.terminalStems) {
        inspectLandmark("Stem crest", stem.crest);
        inspectLandmark("Stem foot", stem.limit);
        inspectLandmark("Bowl edge", stem.bowlEdge);
    }
    if (evidence.descendingStemSection) {
        inspectLandmark("Stem exit", evidence.descendingStemSection->topBaseLine);
        inspectLandmark("Opposite edge", evidence.descendingStemSection->bottomBaseLine);
    }
    if (evidence.medialStem) inspectStemEvidence(*evidence.medialStem);
    for (const auto& tooth : evidence.toothPairs) {
        inspectLandmark("Tooth tip", tooth.tip);
        for (const auto& pair : tooth.pairs) inspectLandmark("Inner bend", pair.bend);
        if (!tooth.forwardFlank.empty()) inspectLandmark("Flank foot", tooth.forwardFlank.back());
        if (!tooth.reverseFlank.empty()) inspectLandmark("Flank foot", tooth.reverseFlank.back());
    }
    for (const auto& tooth : evidence.bentTeeth.bodies) {
        inspectLandmark("Tooth tip", tooth.tip);
        inspectLandmark("Inner bend", tooth.bend);
        for (const auto& entry : tooth.entries) inspectLandmark("Baseline foot", entry.foot);
    }
    for (const auto &fork : evidence.bowlForks) {
        inspectLandmark("Fork upper", fork.upper);
        inspectLandmark("Fork lower", fork.lower);
        for (const auto &cap : fork.caps)
            inspectLandmark("Bowl cap", cap);
    }
    if (evidence.attachment) {
        if (evidence.attachment->outer)
            inspectLandmark("Outer attachment", *evidence.attachment->outer);
        for (const auto &loop : evidence.attachment->loops) {
            for (const auto &point : loop.inner)
                inspectLandmark("Inner attachment", point);
            for (const auto &point : loop.forks)
                inspectLandmark("Fork attachment", point);
        }
    }
    if (evidence.layeredHead) {
        for (const auto &neck : evidence.layeredHead->necks) {
            inspectLandmark("Head neck", neck.section.topBaseLine);
            inspectLandmark("Head neck", neck.section.bottomBaseLine);
        }
        for (const auto& rim : evidence.layeredHead->rimExits)
            inspectLandmark("Lower lip corner", rim.corner);
    }
    if (evidence.uprightStem) {
        inspectLandmark("Stem tip", evidence.uprightStem->tip);
        for (const auto &turn : evidence.uprightStem->turns)
            inspectLandmark("Lower turn", turn);
    }
    for (const auto &arm : evidence.taperedArms) {
        inspectLandmark("Arm root", arm.first);
        inspectLandmark("Arm root", arm.second);
    }
    for (const auto &arm : evidence.componentArms) {
        inspectLandmark("Separate arm top", arm.top);
        inspectLandmark("Separate arm tip", arm.bottom);
    }
    for (const auto &part : evidence.components) {
        PathExtrema corner{};
        corner.pathIndex = part.indices.front();
        corner.point = part.ink.boundingRect().topLeft();
        inspectLandmark("Source component", corner);
    }
    if (evidence.bowlTurn)
        inspectLandmark("Bowl turn", evidence.bowlTurn->turn);
    if (evidence.openBowl) {
        inspectLandmark("Bowl free end", evidence.openBowl->tip);
        inspectLandmark("Bowl floor", evidence.openBowl->floor);
        inspectLandmark("Bowl shoulder", evidence.openBowl->shoulder);
        if (evidence.openBowl->crest)
            inspectLandmark("Bowl crest", *evidence.openBowl->crest);
    }
    for (const auto &head : evidence.openHeads) {
        inspectLandmark("Open crest", head.crest);
        inspectLandmark("Right lip", head.rightEdge);
        inspectLandmark("Left lip", head.leftEdge);
    }
    for (const auto &pair : evidence.stackedPairs) {
        for (const auto &p : pair.right)
            inspectLandmark("Right counter foot", p.anchor);
        for (const auto &p : pair.left)
            inspectLandmark("Left counter foot", p.anchor);
    }
    if (evidence.tail) {
        inspectLandmark("Tail tip", evidence.tail->tip);
        for (const auto &branch : evidence.tail->branches)
            inspectLandmark("Tail fork", branch.corner);
    }
    if (evidence.returnSeam) {
        inspectLandmark("Return corner", evidence.returnSeam->first);
        inspectLandmark("Return corner", evidence.returnSeam->second);
    }
    for (const auto &route : evidence.pairedRoutes) {
        inspectLandmark("Body anchor", route.anchor);
        if (route.right)
            inspectLandmark("Right route anchor", route.right->anchor);
        if (route.left)
            inspectLandmark("Left route anchor", route.left->anchor);
    }
    corner(evidence.terminalNeck);
    if (evidence.boundaryCounter) {
        PathExtrema counter{};
        counter.pathIndex = int(evidence.boundaryCounter->index);
        counter.point = evidence.boundaryCounter->box.center();
        inspectLandmark("Body counter", counter);
    }
    for (const auto &loop : evidence.loops) {
        for (const auto &entry : loop.entryRoutes) {
            inspectLandmark("Loop crest", entry.crest);
            if (entry.route)
                inspectLandmark("Loop entry anchor", entry.route->anchor);
        }
        if (loop.teeth)
            for (const auto &tooth : *loop.teeth)
                inspectLandmark("Attached tooth", tooth.tip);
        corner(loop.entry);
        if (loop.lowerEntry)
            for (const auto &entry : loop.lowerEntry->corners)
                inspectLandmark("Lower entry corner", entry.anchor);
        for (const auto &p : loop.exit.corners) {
            auto anchor = p.anchor;
            anchor.point.setX(-anchor.point.x());
            inspectLandmark("Exit corner", anchor);
        }
    }
    for (const auto &rim : evidence.rims)
        for (const auto &p : rim.anchors)
            inspectLandmark("Rim landmark", p);
    for (const auto &p : evidence.shoulders)
        inspectLandmark("Shoulder", p);
}

std::vector<Stretching> declaredExitAlternatives(SearchContext& context, const QJsonObject& config,
                                                const QString& id) {
    const auto form = resolvedBodyForm(config, namedLetterForm(config, id));
    const auto body = form.value("body").toObject();
    const auto policy = form.value("cuts").toObject().value("head").toObject();
    if (body.value("shape") != "layered-head" || !policy.contains("rimWalkWidths")) return {};
    InspectFormScope inspection(form, context.transformedPath);
    const auto evidence = recognizedSharedShape(context, body);
    inspectSharedShape(*evidence);
    auto sections = cutLayeredHeadRims(context, *evidence->layeredHead, policy, config.value("strokeWidth").toDouble(1));
    for (const auto& section : sections) inspectCandidate("Lower lip exit", section);
    return sections;
}

std::vector<Stretching> declaredFlankExits(SearchContext& context, const QJsonObject& body,
                                         const QJsonObject& cuts, double stroke) {
    const auto angle = cuts.value("flankExitAngle");
    if (body.value("shape") != "contour-route" || !angle.isDouble()) return {};
    const auto evidence = recognizedSharedShape(context, body);
    if (!evidence->route) return {};

    const auto& route = *evidence->route;
    const auto advance = route.topPrevious ? &SearchContext::prev : &SearchContext::next;
    const auto origin = context.timeSorted.find(route.origin);
    const auto end = context.timeSorted.find(route.topEnd);
    if (origin == context.timeSorted.end() || end == context.timeSorted.end()) return {};
    auto crest = *origin;
    for (auto point = origin; point != end; point = (context.*advance)(point))
        if (ByDescendingY{}(crest, *point)) crest = *point;
    const auto outer = *context.xSorted.begin();
    inspectLandmark("Flank crest", crest);
    inspectLandmark("Outer turn", outer);

    std::vector<Stretching> exits;
    for (const auto& start : {crest, outer}) {
        auto section = strokeFlankTurn(context, start, stroke, -1, true, angle.toDouble(), .5);
        if (!section) continue;
        if (section->topBaseLine.point.y() > section->bottomBaseLine.point.y())
            std::swap(section->topBaseLine, section->bottomBaseLine);
        const auto side = getJoinTime(*section, context.orientation);
        if (!side.isStrictRight(crest.time) || !side.isStrictRight(outer.time)) continue;
        inspectCandidate("Flank exit proposal", *section);
        exits.push_back(*section);
    }
    return exits;
}

bool declaredSharedBoundary(SearchContext& context, const QJsonObject& form, const Stretching* finalFold) {
    InspectFormScope inspection(form, context.transformedPath);
    const auto body = form.value("body").toObject(), cuts = form.value("cuts").toObject();
    const auto evidence = recognizedSharedShape(context, body);
    const auto shape = body.value("shape");
    bool found = false;
    if (shape == "contour-route") {
        const auto route = body.value("route").toObject(), policy = cuts.value("route").toObject();
        const bool descending = policy.value("descendingTurn").isDouble();
        if (descending) {
            const auto project = [&] {
                std::vector<std::optional<Stretching>> result;
                if (!evidence->route) return result;
                for (const auto& [cut, fork] : descendingRouteEntries(context, *evidence->route,
                        policy, evidence->tail ? &*evidence->tail : nullptr)) {
                    inspectLandmark("Entry limit at fork", fork);
                    result.emplace_back(cut);
                }
                return result;
            };
            std::vector<std::optional<Stretching>> local;
            const auto* entries = &local;
            if (context.outlineCache && context.outlineCache->outline &&
                context.outlineCache->outline->ink == context.transformedPath) {
                auto& cached = context.outlineCache->routeCuts;
                const auto key = bodyDescriptionKey({{"route", route}, {"policy", policy}});
                auto [entry, inserted] = cached.try_emplace(key);
                if (inserted) entry->second = project();
                entries = &entry->second;
            } else local = project();
            if (!entries->empty() && entries->front()) {
                context.res = *entries->front();
                found = true;
            }
        } else found = projectSavedRoute(context, evidence->route, route, policy);
    } else if (shape == "tapered-arm")
        found = cutTaperedArms(evidence->taperedArms, cuts.value("arm").toObject(), context.res);
    else if (shape == "open-bowl") {
        if (!evidence->openBowl) {
            inspectReason("Body", "No shoulder found on the outer bowl contour.");
        } else {
            inspectLandmark("Bowl shoulder", evidence->openBowl->shoulder);
            found = cutOpenBowl(context, *evidence->openBowl, cuts.value("bowl").toObject());
            if (!found) inspectReason("Cut", "No valid cut through the bowl shoulder.");
        }
    } else if (shape == "upright-stem") {
        const auto policy = cuts.value("stem").toObject();
        if (evidence->uprightStem)
            found = cutUprightStem(context, *evidence->uprightStem, policy, context.res);
        if ((!found || std::abs(sectionLine(context.res).dy()) <= .05 * sectionLine(context.res).length()) &&
            evidence->descendingShaft) {
            Stretching foot{};
            if (cutDescendingShaft(context, *evidence->descendingShaft, policy, foot)) {
                context.res = foot;
                found = true;
            }
        }
    } else if (shape == "layered-head")
        found = evidence->layeredHead && cutLayeredHead(context, *evidence->layeredHead, cuts.value("head").toObject());
    else if (shape == "counter-attachment")
        found = evidence->attachment &&
                cutCounterAttachments(context, *evidence->attachment, shapeSettings(form, "attachment"));
    else if (shape == "forked-bowl")
        found = cutBowlForks(context, evidence->bowlForks, shapeSettings(form, "fork"), context.res);
    else if (shape == "descending-stem") {
        found = evidence->descendingStemSection.has_value();
        if (found) context.res = *evidence->descendingStemSection;
    } else if (shape == "terminal-fork")
        found = cutTerminalStemFork(context, shapeSettings(form, "stem"), context.res);
    else if (shape == "stemmed-bowl")
        found = cutTerminalStemBodies(context, evidence->terminalStems, shapeSettings(form, "stem"),
                                      cuts.value("stem").toObject());
    else if (shape == "tail-branch")
        found = evidence->tail && cutTailBranches(context, *evidence->tail, shapeSettings(form, "tail"), context.res);
    else if (shape == "rising-return" || shape == "folded-return")
        found = evidence->returnSeam &&
                cutReturnSeam(context, *evidence->returnSeam, cuts.value("return").toObject(), context.res);
    else if (shape == "horizontal-counters")
        found = evidence->horizontalPair &&
                cutHorizontalPair(context, *evidence->horizontalPair, cuts.value("pair").toObject());
    else if (shape == "suspended-loop")
        found = evidence->head &&
                cutLoopHead(context, *evidence->head, body.value("head").toObject(),
                            cuts.value("head").toObject());
    else if (shape == "enclosed-lips") {
        const auto proposals = cutEnclosedLips(context, evidence->enclosedLips, cuts.value("head").toObject(), true);
        found = !proposals.empty();
        if (found)
            context.res = proposals.front().second;
    } else if (shape == "tooth-cluster") {
        const auto settings = toothClusterSettings(form);
        const bool final = cuts.value("boundaries").toObject().value("left") == "free";
        const double maxX = context.subword.joins.empty() ? context.bbox.right() + 1e-6
                                                          : context.subword.joins.rbegin()->bottomBaseLine.point.x();
        Stretching cut{};
        std::array<PathExtrema, 3> tips;
        found = cutToothClusters(context, evidence->clusters, settings, context.bbox.left(),
                                 settings.value("terminalBowl").toBool() ? context.bbox.right() + 1e-6 : maxX,
                                 final ? &cut : nullptr, final ? nullptr : &cut, finalFold,
                                 finalFold ? &tips : nullptr);
        if (found && finalFold) {
            const auto side = getJoinTime(cut, context.orientation);
            found = std::all_of(tips.begin(), tips.end(), [&](const auto& tip) { return side.isStrictRight(tip.time); });
        }
        if (found) {
            context.res = cut;
            context.res.topLeft = context.res.topRight = cut.topBaseLine;
            context.res.bottomLeft = context.res.bottomRight = cut.bottomBaseLine;
            context.res.fixedRightBoundary = !final;
            context.nextIter = std::find_if(context.xSorted.begin(), context.xSorted.end(), [&](const auto &point) {
                return point.point.x() < cut.topBaseLine.point.x();
            });
        }
    } else if (shape == "profile-neck") {
        const auto attachment = cuts.value("attachment").toObject();
        if (!attachment.isEmpty()) {
            const bool medial = attachment.value("previousPosition") == "medial";
            if (context.baseText.size() < 2 || (context.baseText.size() > 2) != medial ||
                !attachment.value("previous").toString().contains(context.baseText[context.baseText.size() - 2]))
                return false;
        }
        const NeckBody *selected = nullptr;
        found = cutProfileNeck(context, evidence->bowlNecks, &selected, cuts.value("probe").toObject());
        if (found && cuts.value("boundaries").toObject().value("left") == "edge-projection") {
            const auto seed = context.res;
            const QLineF chord(seed.topBaseLine.point, seed.bottomBaseLine.point);
            std::optional<Stretching> aligned;
            for (const auto &edge : selected->edges) {
                auto cut = seed;
                if (!medialChordAt(context, edge.point.x(), chord.center().y(), chord.length(), cut) ||
                    cut.topBaseLine.point.y() <= edge.point.y())
                    continue;
                if (aligned) {
                    found = false;
                    break;
                }
                aligned = cut;
            }
            found = found && aligned.has_value();
            if (found) {
                context.res = *aligned;
                context.res.fixedRightBoundary = true;
                context.res.length = 0;
            }
        }
    } else if (shape == "bowl-turn")
        found = evidence->bowlTurn && cutBowlTurn(context, *evidence->bowlTurn, cuts);
    else if (shape == "counter-neck") {
        const auto boundary = cuts.value("boundaries").toObject().value("right");
        if (boundary == "free")
            found = evidence->initialNeck && cutInitialCounterBody(context, *evidence->initialNeck, shapeSettings(form, "counter"));
        else if (boundary == "corner-junction" && evidence->bowlTurn && evidence->bowlTurn->junction)
            found = cutBowlTurn(context, *evidence->bowlTurn, cuts);
        else if (evidence->boundaryCounter) {
            const auto& box = evidence->boundaryCounter->box;
            const auto policy = cuts.value("counter").toObject();
            const double offset = policy.value("offset").toDouble() * box.height();
            const double x = box.right() + offset;
            const double y = box.bottom() + policy.value("baselineOffset").toDouble() * box.height();
            context.res = {};
            found = projectCounterEntry(context, evidence->terminalNeck, policy, x, y, context.res);
            if (found) {
                context.res.topLeft = context.res.topRight = context.res.topBaseLine;
                context.res.bottomLeft = context.res.bottomRight = context.res.bottomBaseLine;
                found = matchesBoundaryBody(context, form, context.res, false);
            }
        }
    } else if (shape == "counter-shoulder" && evidence->boundaryCounter) {
        const auto shoulders = loopShoulderCuts(context, evidence->boundaryCounter->box,
            evidence->shoulders, shapeSettings(form, "shoulder"), context.xSorted.rbegin()->time, true);
        found = !shoulders.empty();
        if (found) context.res = shoulders.front();
    } else if (shape == "counter-loop") {
        const auto candidate =
            selectLoopBody(context, evidence->loops, context.bbox.left(), context.bbox.right(), nullptr);
        const auto pair =
            candidate ? cutLoopBody(context, *candidate, shapeSettings(form, "loop"), false) : std::nullopt;
        found = pair.has_value();
        if (found) {
            context.res = pair->second;
            context.res.fixedRightBoundary = true;
        }
    }
    if (found)
        inspectCandidate("Boundary proposal", context.res);
    else
        inspectReason("Cuts", "Recognized body features did not produce a section retaining this body.");
    return found;
}

} // namespace MedinaCutter
