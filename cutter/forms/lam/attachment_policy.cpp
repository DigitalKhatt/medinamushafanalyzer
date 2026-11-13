#include "../../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

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
    for (size_t index = 0; index < recognition.lamBodies.forks.size(); ++index) {
        const auto& body = recognition.lamBodies.forks[index];
        if (!body.isConcave(context.orientation) || (!body.fork && !body.folded()) ||
            !entrySide.isStrictLeft(body.tip.time) ||
            (hasExit && !getJoinTime(joins[slot], context.orientation).isStrictRight(body.tip.time))) continue;
        double score = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < recognition.lamBodies.crests.size(); ++i) {
            if (!recognition.shaftForks.empty() &&
                std::ranges::find(recognition.shaftForks[i], index) == recognition.shaftForks[i].end()) continue;
            const auto& crest = recognition.lamBodies.crests[i];
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
            recordBoundaryTemplate(sub, slot, true, diagnostic.value("template").toString());
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
bool resolveSubwordCutPlan(const Word& word, const SubwordBodyRecognition& recognition,
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
            const auto receiver = sub.letterDiagnostics.last().toObject();
            const auto source = junction ? returnBody->body.value("alefArm").toString() :
                receiver.value("entryTemplate").toString(receiver.value("template").toString());
            recordBoundaryTemplate(sub, terminalLam, false, source);
            recordBoundaryTemplate(sub, terminalLam + 1, true, source);
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

            // Keep the fork anchors fixed while testing the neighboring cut.
            const auto oldCap = sending ? previousCap : nextCap;
            const auto flank = chord(sending ? right : left);
            const auto accept = [&](const Stretching& seam, double maxShift, double minGap) {
                const auto cap = chord(seam);
                if ((sending ? !before(seam, right) : !before(left, seam)) ||
                    QLineF(cap.center(), oldCap.center()).length() > maxShift * oldCap.length() ||
                    QLineF(cap.center(), flank.center()).length() < minGap)
                    return false;
                return sending ? apply(right, left, seam, next) : apply(right, left, previous, seam);
            };
            if (sending) {
                const auto sender = old.letterDiagnostics[i - 1].toObject().value("template").toString();
                for (const auto& seam : declaredExitAlternatives(context, config, sender)) {
                    if (!accept(seam, .5, offset / 3)) continue;
                    recordBoundaryTemplate(sub, i - 1, false, sender);
                    sub.templateTrace.append(sender + " · alternate declared exit");
                    return true;
                }
            }
            if (receiving && i + 2 == letters.size()) {
                const auto alternative = lam.value("overlappingExit").toObject();
                const auto id = alternative.value("finalTemplate").toString();
                const auto rule = bodyConstraintForm(config, namedLetterForm(config, id));
                if (rule.value("current").toString().contains(letters.back()) &&
                    declaredSharedBoundary(context, rule) && matchesBoundaryBody(context, rule, context.res, false) &&
                    accept(context.res, alternative.value("maxEntryShiftWidths").toDouble(),
                           alternative.value("minGapToOffset").toDouble() * offset)) {
                    recordAssembledForm(sub, i + 1, id);
                    recordBoundaryTemplate(sub, i + 1, true, id);
                    sub.templateTrace.append(id + " · neighboring cut restored at its shoulder");
                    return true;
                }
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
            for (size_t index = 0; index < recognizedBodies.size(); ++index) {
                if (refined) break;
                const auto& body = recognizedBodies[index];
                if (!recognized->shaftForks.empty()) {
                    bool ownsFork = false;
                    for (size_t j = 0; j < recognized->shaftForks.size(); ++j) {
                        const auto& crest = recognized->lamBodies.crests[j];
                        // The true cap can lie above the sampled crest on a
                        // flat or slanted nib. Use membership in this body.
                        if (!bounds.adjusted(-.01, -.01, .01, .01).contains(crest.point)) continue;
                        const auto& forks = recognized->shaftForks[j];
                        if (std::ranges::find(forks, index) != forks.end()) {
                            ownsFork = true;
                            break;
                        }
                    }
                    if (!ownsFork) continue;
                }
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
            if (!sharedCounter && sub.kashidaCuts[i - 1].second != old.kashidaCuts[i - 1].second)
                recordBoundaryTemplate(sub, i, true, form);
            if (sub.kashidaCuts[i].first != old.kashidaCuts[i].first)
                recordBoundaryTemplate(sub, i, false, form);
            sub.templateTrace.append(form + " · fork cut policy applied");
        }
    }
    return true;
}

} // namespace MedinaCutter
