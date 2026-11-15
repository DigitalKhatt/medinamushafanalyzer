#include "../detail.h"
#include <QScopeGuard>

namespace MedinaCutter {
namespace {

using Subword = WordResultInfo::SubWordInfo;
using BodySequence = std::span<const AnchoredBody* const>;
using FormChoices = std::vector<std::vector<const QJsonObject*>>;

enum class PartitionRank { Preferred, SecondaryBody, AdjustedJoin, RefinedBody, None };

struct CandidateBudget {
    int ordinary;
    int refined;

    int& forEvidence(bool usesRefinement) { return usesRefinement ? refined : ordinary; }
};

bool fitsBody(const AnchoredBody& body, const SearchContext& source, size_t slot, size_t count);

struct AnchorCandidateKey {
    QByteArray form;
    size_t slot;
    bool precise;
    bool entryTurn;
    bool continuation;
    const Stretching* sharedExit = nullptr;

    auto tie() const { return std::tie(form, slot, precise, entryTurn, continuation, sharedExit); }
    bool operator<(const AnchorCandidateKey& other) const { return tie() < other.tie(); }
};

struct JoinCompatibility {
    bool allowed;
    bool stopAtBowl = false;
};

struct AnchorSearchMemo {
    std::map<const QJsonObject*, QByteArray> formKeys;
    std::map<AnchorCandidateKey, std::vector<AnchoredBody>> bodies;
    std::map<std::tuple<const AnchoredBody*, QByteArray, size_t, bool>, std::vector<AnchoredBody>> sharedEntries;
    std::map<std::tuple<const QJsonObject*, QString, bool>, std::optional<Stretching>> folds;
    std::map<std::tuple<const AnchoredBody*, const AnchoredBody*, size_t, bool, bool, bool, double>,
             JoinCompatibility> joins;

    const QByteArray& key(const QJsonObject& form) {
        auto [entry, inserted] = formKeys.try_emplace(&form);
        if (inserted) {
            auto geometry = form;
            // Eligibility is checked before recognition. Only the presence
            // of an ending constraint affects the descending blade policy.
            geometry.remove("preferSharedAttachment");
            if (geometry.contains("fixedAnchorsEnding"))
                geometry.insert("fixedAnchorsEnding", "eligible");
            entry->second = QJsonDocument(geometry).toJson(QJsonDocument::Compact);
        }
        return entry->second;
    }

    const std::vector<AnchoredBody>& candidates(SearchContext& source, const QJsonObject& form,
            size_t position, size_t slot, size_t count, double penWidth, bool precise, bool entryTurn,
            AnchorPass pass, const Stretching* sharedExit = nullptr) {
        auto [saved, inserted] = bodies.try_emplace(AnchorCandidateKey{key(form), slot, precise,
            entryTurn, pass == AnchorPass::Continuation, sharedExit});
        if (inserted) {
            saved->second = anchoredBodies(source, form, position, penWidth, entryTurn, sharedExit, pass);
            std::erase_if(saved->second, [&](const auto& body) { return !fitsBody(body, source, slot, count); });
        }
        return saved->second;
    }
};

std::optional<std::vector<bool>> explicitConnectors(const QString& text, const QString& letters) {
    std::vector<bool> connectors(letters.size() - 1);
    qsizetype slot = 0;
    for (const auto ch : text) {
        if (ch.unicode() == 0x0640) {
            if (slot == 0 || slot >= letters.size() || connectors[slot - 1]) return {};
            connectors[slot - 1] = true;
        } else if (slot < letters.size() && ch == letters.at(slot))
            ++slot;
    }
    if (slot != letters.size()) return {};
    return connectors;
}

size_t letterPosition(size_t slot, size_t count) {
    return slot == 0 ? 0 : slot + 1 == count ? 2 : 1;
}

bool sameSection(const Stretching& a, const Stretching& b) {
    return a.topBaseLine.time == b.topBaseLine.time && a.bottomBaseLine.time == b.bottomBaseLine.time &&
           sectionLine(a) == sectionLine(b);
}

bool sharesIncomingSection(const Stretching& sending, const AnchoredBody& receiving, Orientation orientation) {
    if (!receiving.sharesIncoming || !receiving.exit || cutPrecedes(sending, *receiving.entry, orientation) ||
        !orderedSections(sending, *receiving.exit, orientation)) return false;
    const auto side = getJoinTime(sending, orientation);
    return std::all_of(receiving.landmarks.begin(), receiving.landmarks.end(),
                       [&](const auto& landmark) { return side.isStrictLeft(landmark.time); });
}

bool sharesTightEntry(const AnchoredBody& sending, const AnchoredBody& receiving, double tolerance) {
    if (receiving.tightEntryAfter != sending.formId) return false;
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    // A triangular remnant at a stacked join has one nearly coincident
    // endpoint. The other end of the receiving seam stays on the loop rim.
    return std::min(QLineF(exit.topBaseLine.point, entry.topBaseLine.point).length(),
                    QLineF(exit.bottomBaseLine.point, entry.bottomBaseLine.point).length()) <= tolerance;
}

bool sharesCornerExit(const AnchoredBody& sending, const Stretching& entry,
                      Orientation orientation, double tolerance) {
    if (!sending.sharesExitAtCorners || !entry.topBaseLine.isSharp || !entry.bottomBaseLine.isSharp) return false;
    const auto& exit = *sending.exit;
    if (orderedSections(exit, entry, orientation)) return false;
    const double topGap = QLineF(exit.topBaseLine.point, entry.topBaseLine.point).length();
    const double bottomGap = QLineF(exit.bottomBaseLine.point, entry.bottomBaseLine.point).length();
    return std::min(topGap, bottomGap) <= tolerance &&
           std::max(topGap, bottomGap) <= sectionLine(exit).length();
}

bool leavesSendingStroke(const SearchContext& source, const Stretching& exit, double penWidth) {
    const auto side = getJoinTime(exit, source.orientation);
    const int count = baseContourCurves(source.transformedPath).size();
    const auto chord = sectionLine(exit);
    const auto axis = (chord.p2() - chord.p1()) / chord.length();
    const auto flankDirection = [&](const PathExtrema& end, bool sending) {
        const bool forward = sending == (side.startAtLeft == (end.time == side.minTime));
        auto limit = end;
        limit.time += forward ? count : -count;
        PathExtrema sample{};
        QPointF tangent;
        if (!advanceOnContour(source.transformedPath, end, limit, forward, .25 * penWidth, sample, tangent))
            return QPointF{};
        const auto delta = sample.point - end.point;
        const double length = std::hypot(delta.x(), delta.y());
        return length > 0 ? delta / length : QPointF{};
    };
    const auto top = flankDirection(exit.topBaseLine, true);
    const auto bottom = flankDirection(exit.bottomBaseLine, true);
    constexpr double minFlankAlignment = .6;
    constexpr double minStrokeAlignment = .866;
    const double alongTop = QPointF::dotProduct(top, axis);
    const double alongBottom = QPointF::dotProduct(bottom, axis);
    const auto stroke = top + bottom;
    const bool alongBody = alongTop * alongBottom > 0 &&
        std::min(std::abs(alongTop), std::abs(alongBottom)) >= minFlankAlignment &&
        std::abs(alongTop + alongBottom) >= minStrokeAlignment * std::hypot(stroke.x(), stroke.y());
    if (!alongBody) return true;

    // The body flanks can run along the cap at a natural turn. Keep the
    // cut when the two flanks leave the cap together as a single stroke.
    constexpr double minOutgoingAlignment = .819152; // cos(35 degrees)
    return QPointF::dotProduct(flankDirection(exit.topBaseLine, false),
                               flankDirection(exit.bottomBaseLine, false)) >= minOutgoingAlignment;
}

bool sharesCounterSection(const Stretching& sending, const AnchoredBody& receiving, Orientation orientation) {
    return sending.counterEntry.pathIndex > 0 && receiving.keepsSharedCounterEntry && receiving.exit &&
        sending.topBaseLine.point == receiving.entry->topBaseLine.point &&
        orderedSections(sending, *receiving.exit, orientation) &&
        retainsLandmarks(receiving, &sending, &*receiving.exit, orientation);
}

bool fitsBody(const AnchoredBody& body, const SearchContext& source, size_t slot, size_t count) {
    if (body.entry.has_value() != (slot > 0) || body.exit.has_value() != (slot + 1 < count)) return false;
    const auto validSection = [&](const Stretching& section, bool entry) {
        const auto& top = section.topBaseLine;
        const auto& bottom = section.bottomBaseLine;
        return (section.counterEntry.pathIndex > 0 ? (entry ? body.keepsSharedCounterEntry :
                !body.sharedCounterReceiver.isEmpty()) :
            interiorChord(source.transformedPath, top, bottom, section.topEndpointTolerance)) &&
            top.pathIndex == 0 && bottom.pathIndex == 0 &&
            top.time != bottom.time && sectionLine(section).length() > 1e-6;
    };
    if ((body.entry && !validSection(*body.entry, true)) || (body.exit && !validSection(*body.exit, false)) ||
        (body.entry && body.exit && !orderedSections(*body.entry, *body.exit, source.orientation))) return false;
    return retainsLandmarks(body, body.entry ? &*body.entry : nullptr, body.exit ? &*body.exit : nullptr,
                            source.orientation);
}

bool canUseReceivingEntry(const AnchoredBody& sending, const AnchoredBody& receiving, Orientation orientation,
                          QChar preceding, double penWidth) {
    if (!sending.entry && receiving.keepInitialExitAfter.contains(preceding)) return false;
    if (receiving.sharesFinalEntry || receiving.shareEntryWith.contains(preceding)) {
        if (!receiving.sharesFinalEntry) {
            if (sending.entry && sending.sharedFold.isEmpty()) return false;
            const auto exit = sectionLine(*sending.exit), entry = sectionLine(*receiving.entry);
            const double gap = exit.center().x() - entry.center().x();
            const double width = std::max(penWidth, exit.length());
            const bool atSameFoot = QLineF(exit.p2(), entry.p2()).length() <= .05 * width;
            if (std::abs(gap) > (atSameFoot ? 1.25 : 1.) * width) return false;
            const auto front = getJoinTime(*sending.exit, orientation);
            if (front.isStrictLeft(receiving.entry->topBaseLine.time) &&
                front.isStrictLeft(receiving.entry->bottomBaseLine.time) &&
                entry.length() <= 2 * exit.length()) return false;
        }
        if (sending.entry && !orderedSections(*sending.entry, *receiving.entry, orientation)) return false;
        return retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &*receiving.entry, orientation);
    }
    if ((!receiving.usesSharedEntry && !receiving.sharesOverlappingEntryAfter.contains(sending.formId)) ||
        orderedSections(*sending.exit, *receiving.entry, orientation)) return false;
    if (!receiving.usesSharedEntry) {
        const auto exit = sectionLine(*sending.exit), entry = sectionLine(*receiving.entry);
        if (QLineF(exit.p1(), entry.p1()).length() >= entry.length() ||
            QLineF(exit.p2(), entry.p2()).length() >= entry.length() ||
            QPointF::dotProduct(exit.p2() - exit.p1(), entry.p2() - entry.p1()) <= 0) return false;
    }
    if (sending.entry && !orderedSections(*sending.entry, *receiving.entry, orientation)) return false;
    return retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &*receiving.entry, orientation);
}

bool sharesLoopNeck(const AnchoredBody& sending, const AnchoredBody& receiving,
                    Orientation orientation, double tightTolerance) {
    if (sending.minimumExitAtTurn || sending.counters.size() != 1 || sending.entry ||
        (!receiving.sharesLoopNeck && receiving.sharedEntryFrom != sending.formId) ||
        leavesEntryClearance(*sending.exit, *receiving.entry, orientation, tightTolerance)) return false;
    const auto exit = sectionLine(*sending.exit), entry = sectionLine(*receiving.entry);
    const double width = exit.length();
    if (width <= 0 || QLineF(exit.p1(), entry.p1()).length() > 1.5 * width ||
        QLineF(exit.p2(), entry.p2()).length() > 1.5 * width ||
        QPointF::dotProduct(exit.p2() - exit.p1(), entry.p2() - entry.p1()) <= 0) return false;
    return retainsLandmarks(sending, nullptr, &*receiving.entry, orientation);
}

bool sharesCloseAttachment(const AnchoredBody& sending, const AnchoredBody& receiving,
                           Orientation orientation, double tolerance, double penWidth = 0) {
    if (!receiving.acceptsCloseSharedJoin || sending.usesSharedEntry) return false;
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    if (leavesEntryClearance(exit, entry, orientation, tolerance) || exit.counterEntry.pathIndex > 0 ||
        entry.counterEntry.pathIndex > 0) return false;
    const auto a = sectionLine(exit), b = sectionLine(entry);
    const bool endsMeet = QLineF(a.center(), b.center()).length() <= tolerance &&
        QLineF(a.p1(), b.p1()).length() <= tolerance && QLineF(a.p2(), b.p2()).length() <= tolerance;
    if (!endsMeet) {
        if (penWidth <= 0 || sending.entry || !receiving.coversSharedEntry ||
            QLineF(a.center(), b.center()).length() > penWidth ||
            std::max(a.length(), b.length()) > 3.2 * penWidth) return false;
        // A terminal counter bridge can meet the inside of the initial
        // probe. Require aligned sections and a common flank.
        const auto side = getJoinTime(exit, orientation);
        if (side.isStrictLeft(entry.topBaseLine.time) && side.isStrictLeft(entry.bottomBaseLine.time)) return false;
        const auto axis = a.p2() - a.p1();
        if (QPointF::dotProduct(axis, b.p2() - b.p1()) < .85 * a.length() * b.length()) return false;
        const auto distance = [&](const QPointF& point) {
            const double t = QPointF::dotProduct(point - a.p1(), axis) / QPointF::dotProduct(axis, axis);
            return QLineF(point, a.pointAt(std::clamp(t, 0., 1.))).length();
        };
        if (std::min(distance(b.p1()), distance(b.p2())) > .3 * penWidth) return false;
    }
    return (!sending.entry || orderedSections(*sending.entry, entry, orientation)) &&
        retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &entry, orientation);
}

bool keepsStrokeAtEntry(const AnchoredBody& sending, const AnchoredBody& receiving, SearchContext& source,
                        double penWidth) {
    if (!receiving.keepsEntryConnector || sending.entry || !orderedSections(*sending.exit, *receiving.entry, source.orientation))
        return false;
    const auto a = sectionLine(*sending.exit), b = sectionLine(*receiving.entry);
    if (a.center().x() - b.center().x() <= 2 * penWidth) return false;
    const auto parts = cutRegions(source.transformedPath, {}, {*sending.exit, *receiving.entry}, source.orientation, true);
    return parts.size() == 3 && toSubpaths(parts[1].path).size() == 1 &&
        followsOneStroke(parts[1].path, a, b, ConnectorEndpointMatch::NearestVertex);
}

bool fitsDeclaredSharedEntry(const AnchoredBody& sending, const AnchoredBody& receiving,
                            SearchContext& source, QChar lastLetter, double penWidth) {
    if (!sending.sharedEntryBody || sending.entry) return false;
    const auto& form = *sending.sharedEntryBody;
    const auto& section = *receiving.entry;
    const auto proposed = sectionLine(section);
    const auto previous = sectionLine(*sending.exit);
    const bool returnSeam = receiving.sharesFinalEntry && sending.sharedReturnWith.contains(lastLetter);
    const bool stacked = receiving.exit && !receiving.keepsEntryConnector && receiving.usesSharedEntry &&
        form.value("body").toObject().value("shape") == "descending-stem" &&
        QLineF(previous.center(), proposed.center()).length() <= penWidth;
    const bool sameRim = !receiving.exit && !returnSeam && receiving.acceptsCloseSharedJoin && sending.counters.size() == 2 &&
        QLineF(previous.p1(), proposed.p1()).length() <= .75 * penWidth &&
        QLineF(previous.p2(), proposed.p2()).length() <= 4 * penWidth &&
        !orderedSections(*sending.exit, section, source.orientation);
    const bool hangingLoop = receiving.exit && receiving.prefersTightIncoming && !receiving.counters.isEmpty() &&
        form.value("body").toObject().value("shape") == "layered-head" &&
        proposed.length() <= 1.5 * penWidth && proposed.center().y() < previous.center().y() &&
        QLineF(previous.center(), proposed.center()).length() <= 5 * penWidth;
    if (!returnSeam && !sameRim && !stacked && !hangingLoop) return false;
    if (!retainsLandmarks(sending, nullptr, &section, source.orientation)) return false;
    if (returnSeam || hangingLoop) {
        const auto parts = cutRegions(source.transformedPath, {}, {section}, source.orientation, false);
        if (parts.size() != 2 || parts.front().path.boundingRect().height() < 3 * penWidth) return false;
        if (hangingLoop && toSubpaths(parts.front().path).size() != 1) return false;
    }
    return sameRim || matchesBoundaryBody(source, form, section, true);
}

bool separatesFoldedTooth(SearchContext& source, const AnchoredBody& sending,
                          const AnchoredBody& receiving) {
    if (!sending.toothExitFlank || sameSection(*sending.exit, *receiving.entry)) return false;
    const auto& [tip, increasingTime] = *sending.toothExitFlank;
    return seenTipMeetsFold(source, tip, increasingTime, *receiving.entry);
}


bool fitsAnchorGap(const AnchoredBody& sending, const AnchoredBody& receiving,
                   Orientation orientation, double penWidth, bool checkTerminalBody) {
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    if ((!checkTerminalBody || !sending.preservesTerminalBody || !receiving.stemmedBowl) &&
        !receiving.needsSharedInwardCorner) return true;
    const double gap = sectionLine(exit).center().x() - sectionLine(entry).center().x();
    if (gap <= 1.5 * penWidth) return true;
    return !(checkTerminalBody && sending.preservesTerminalBody && receiving.stemmedBowl) &&
           !orderedSections(exit, entry, orientation);
}

bool sharesDeclaredFixedEntry(SearchContext& source, const AnchoredBody& sending, const AnchoredBody& receiving,
                              double penWidth) {
    if (!receiving.sharedFixedEntryAfter.contains(sending.formId) || !receiving.fixedEntryJunction || sending.requiresSharedExit ||
        receiving.requiresSharedEntry) return false;
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    if (sameSection(exit, entry) || exit.counterEntry.pathIndex > 0 || entry.counterEntry.pathIndex > 0 ||
        orderedSections(exit, entry, source.orientation)) return false;
    if (!sending.entry && receiving.coversSharedEntry &&
        sharesCloseAttachment(sending, receiving, source.orientation, .75 * penWidth, penWidth))
        return true;
    const auto preferred = sectionLine(exit), junction = sectionLine(entry);
    const double width = std::min(penWidth, junction.length());
    const double topGap = QLineF(preferred.p1(), junction.p1()).length();
    const double bottomGap = QLineF(preferred.p2(), junction.p2()).length();
    // The cuts must trace the same stroke. A large disagreement on both
    // flanks calls for a different body candidate.
    if (std::min(topGap, bottomGap) > .35 * width ||
        std::max(topGap, bottomGap) > 1.05 * penWidth ||
        QPointF::dotProduct(preferred.p2() - preferred.p1(), junction.p2() - junction.p1()) <= 0 ||
        (sending.entry && !orderedSections(*sending.entry, entry, source.orientation)) ||
        !retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &entry, source.orientation))
        return false;
    return !sending.sharedEntryBody || sending.entry ||
           matchesBoundaryBody(source, *sending.sharedEntryBody, entry, true);
}

const std::optional<Stretching>& exitLimitAtJoin(const AnchoredBody& sending, const AnchoredBody& receiving,
                                                 QChar following) {
    static const std::optional<Stretching> noLimit;
    if (receiving.acceptsCounterRim && receiving.entryLimit && sending.rimExitLimit) return sending.rimExitLimit;
    if (!sending.minimumExitAtTurn &&
        (sending.minimumExitBefore.isEmpty() || sending.minimumExitBefore.contains(following))) return sending.minimumExit;
    return noLimit;
}

const std::optional<Stretching>& entryLimitAtJoin(const AnchoredBody& sending, const AnchoredBody& receiving) {
    static const std::optional<Stretching> noLimit;
    return receiving.minimumEntryAfterForm.isEmpty() || receiving.minimumEntryAfterForm == sending.formId
        ? receiving.minimumEntry : noLimit;
}

const Stretching* exitBeforeFollowingTooth(const AnchoredBody& sending, const AnchoredBody& receiving,
                                           Orientation orientation, QChar following) {
    if (!sending.minimumExitAtFlank || !sending.minimumExitBefore.contains(following) ||
        orderedSections(*sending.exit, *receiving.entry, orientation) ||
        !orderedSections(*sending.minimumExit, *receiving.entry, orientation)) return nullptr;
    const auto onExit = [&](const PathExtrema& landmark) {
        return landmark.time == sending.exit->topBaseLine.time || landmark.time == sending.exit->bottomBaseLine.time;
    };
    return std::any_of(receiving.landmarks.begin(), receiving.landmarks.end(), onExit) ? &*sending.minimumExit : nullptr;
}

bool hasOverlapLimits(const AnchoredBody& sending, const AnchoredBody& receiving,
                      QChar following, double tolerance, double penWidth) {
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    if (sending.requiresSharedExit || receiving.requiresSharedEntry ||
        exit.counterEntry.pathIndex > 0 || entry.counterEntry.pathIndex > 0) return false;
    if (entryLimitAtJoin(sending, receiving)) return true;
    if (!exitLimitAtJoin(sending, receiving, following)) return false;
    if (receiving.fixedEntryJunction || receiving.keepsEntryJunction) return true;
    const auto a = sectionLine(exit), b = sectionLine(entry);
    const double upperGap = QLineF(a.p1(), b.p1()).length();
    const double lowerGap = QLineF(a.p2(), b.p2()).length();
    if (std::max(upperGap, lowerGap) <= tolerance) return true;
    if (receiving.entryLimit &&
        std::max(upperGap, lowerGap) <= std::max(penWidth, b.length()) &&
        QPointF::dotProduct(a.p2() - a.p1(), b.p2() - b.p1()) >= .7 * a.length() * b.length())
        return true;
    // A stacked sender already has a shared entry. Its lower lip can meet
    // the next letter at that letter's fixed seam when both flanks agree.
    return sending.requiresSharedEntry && receiving.acceptsCloseSharedJoin &&
        std::max(upperGap, lowerGap) <= penWidth &&
        QPointF::dotProduct(a.p2() - a.p1(), b.p2() - b.p1()) > 0;
}

bool canResolveOverlappingAnchors(const AnchoredBody& sending, const AnchoredBody& receiving,
                                  Orientation orientation, QChar following) {
    if (sending.requiresSharedExit || receiving.requiresSharedEntry) return false;
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    if (exit.counterEntry.pathIndex > 0 || entry.counterEntry.pathIndex > 0 ||
        sameSection(exit, entry) || orderedSections(exit, entry, orientation)) return false;
    return entryLimitAtJoin(sending, receiving).has_value() ||
           (sending.minimumExitAtTurn && sending.minimumExit) ||
           (exitLimitAtJoin(sending, receiving, following) &&
            (receiving.acceptsCloseSharedJoin || receiving.keepsEntryJunction));
}

bool withinBodyLimit(const Stretching& cut, const std::optional<Stretching>& minimum,
                     Orientation orientation, bool entry) {
    if (!minimum) return true;
    const auto side = getJoinTime(cut, orientation);
    const auto retains = [&](const PathExtrema& point) {
        return entry ? side.isLeft(point.time) : side.isRight(point.time);
    };
    return retains(minimum->topBaseLine) && retains(minimum->bottomBaseLine);
}

std::optional<Stretching> resolveOverlap(const SearchContext& source, const AnchoredBody& sending,
                                         const AnchoredBody& receiving, QChar following, bool atLoopTurn = false) {
    const auto& exit = *sending.exit;
    const auto& entry = *receiving.entry;
    const auto& minimumExit = atLoopTurn ? sending.minimumExit :
        exitLimitAtJoin(sending, receiving, following);
    const bool fixedExit = !minimumExit;
    const auto& minimumEntry = entryLimitAtJoin(sending, receiving);
    const bool fixedEntry = !minimumEntry || receiving.fixedEntryJunction || receiving.keepsEntryJunction;
    if (fixedExit && fixedEntry) return {};
    auto seam = fixedExit ? exit : entry;
    if (!fixedExit && !fixedEntry) {
        const auto exitSide = getJoinTime(exit, source.orientation);
        if (exitSide.isRight(entry.topBaseLine.time)) {
            seam.topBaseLine = exit.topBaseLine;
            seam.topEndpointTolerance = exit.topEndpointTolerance;
        }
        if (exitSide.isRight(entry.bottomBaseLine.time)) seam.bottomBaseLine = exit.bottomBaseLine;
    }
    if (!withinBodyLimit(seam, minimumExit, source.orientation, false) ||
        !withinBodyLimit(seam, minimumEntry, source.orientation, true) ||
        seam.topBaseLine.time == seam.bottomBaseLine.time ||
        !interiorChord(source.transformedPath, seam.topBaseLine, seam.bottomBaseLine, seam.topEndpointTolerance) ||
        (sending.entry && !orderedSections(*sending.entry, seam, source.orientation)) ||
        (receiving.exit && !orderedSections(seam, *receiving.exit, source.orientation)) ||
        !retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &seam, source.orientation) ||
        !retainsLandmarks(receiving, &seam, receiving.exit ? &*receiving.exit : nullptr, source.orientation)) return {};
    return seam;
}

bool hasShortConnector(SearchContext& source, const Stretching& exit, const Stretching& entry) {
    if (!orderedSections(exit, entry, source.orientation)) return false;
    const auto parts = cutRegions(source.transformedPath, {}, {exit, entry}, source.orientation, false);
    return parts.size() == 3 && toSubpaths(parts[1].path).size() == 1 &&
        followsOneStroke(parts[1].path, sectionLine(exit), sectionLine(entry), ConnectorEndpointMatch::NearestVertex);
}

struct JoinSections {
    Stretching exit, entry;
    bool shared, usesReceivingEntry;
    QString terminalJunction;
    PartitionRank rank = PartitionRank::Preferred;
};

std::optional<JoinSections> resolveJoin(SearchContext& source, const AnchoredBody& sending,
        const AnchoredBody& receiving, QChar preceding, QChar following, QChar last, bool terminal, bool sharesFinalFold,
        const QJsonObject& config, bool continuation, bool requiredConnector, double tightTolerance, double penWidth,
        bool allowNeckJoin) {
    auto exit = *sending.exit;
    auto entry = *receiving.entry;
    if (!fitsAnchorGap(sending, receiving, source.orientation, penWidth, continuation)) return {};
    if (const auto* flank = exitBeforeFollowingTooth(sending, receiving, source.orientation, following))
        return JoinSections{*flank, entry, false, false, {}};
    const bool sharedCounter = !terminal &&
        sharesCounterSection(exit, receiving, source.orientation);
    if (sharedCounter) entry = exit;
    const bool sharesRim = receiving.sharesRimEntry &&
        (!orderedSections(exit, entry, source.orientation) ||
         QLineF(sectionLine(exit).center(), sectionLine(entry).center()).length() <= tightTolerance);
    bool sharedAtEntry = false;
    QString terminalJunction;
    if (!sameSection(exit, entry) && !orderedSections(exit, entry, source.orientation) &&
        hasOverlapLimits(sending, receiving, following, tightTolerance, penWidth)) {
        if (requiredConnector) return {};
        // Retreat along the overlapping fork flank and keep its connector if the limits leave room.
        if (receiving.forkEntry)
            if (const auto& limit = entryLimitAtJoin(sending, receiving);
                limit && (!receiving.exit || orderedSections(*limit, *receiving.exit, source.orientation)) &&
                retainsLandmarks(receiving, &*limit, receiving.exit ? &*receiving.exit : nullptr, source.orientation) &&
                hasShortConnector(source, exit, *limit))
                return JoinSections{exit, *limit, false, false, {}};
        if (sending.forkEntry)
            if (const auto& limit = exitLimitAtJoin(sending, receiving, following);
                limit && (!sending.entry || orderedSections(*sending.entry, *limit, source.orientation)) &&
                retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &*limit, source.orientation) &&
                hasShortConnector(source, *limit, entry))
                return JoinSections{*limit, entry, false, false, {}};
        const auto seam = resolveOverlap(source, sending, receiving, following);
        if (seam && !sending.footExitCandidate && !keepsStrokeAtEntry(sending, receiving, source, penWidth))
            return JoinSections{*seam, *seam, true, sameSection(*seam, entry), {}};
        if (entryLimitAtJoin(sending, receiving) || (!sending.minimumExitAtTurn && sending.minimumExit &&
            (sending.minimumExitBefore.isEmpty() || sending.minimumExitBefore.contains(following)))) return {};
    }
    const bool preserveHollow = continuation && sending.keepsExitHollow &&
        !leavesEntryClearance(exit, entry, source.orientation, tightTolerance) &&
        hasShortConnector(source, exit, entry);
    const bool useFixedJunction = !requiredConnector && sharesDeclaredFixedEntry(source, sending, receiving, penWidth);
    if (!preserveHollow &&
        (useFixedJunction || sharesRim ||
        sharesCornerExit(sending, entry, source.orientation, tightTolerance) ||
        canUseReceivingEntry(sending, receiving, source.orientation, preceding, penWidth) ||
        (allowNeckJoin && sharesLoopNeck(sending, receiving, source.orientation, tightTolerance)) ||
        (receiving.sharesJunctionEntry && !orderedSections(exit, entry, source.orientation)) ||
        (!receiving.requiresSharedEntry &&
            fitsDeclaredSharedEntry(sending, receiving, source, last, penWidth)) ||
        (terminal &&
            (sharesFinalFold || sharesTightEntry(sending, receiving, tightTolerance))))) {
        if (!retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &entry, source.orientation))
            return {};
        if (receiving.prefersTightIncoming && !sending.counters.isEmpty() && !sending.entry &&
            !fitsDeclaredSharedEntry(sending, receiving, source, last, penWidth) &&
            getJoinTime(exit, source.orientation).isStrictLeft(entry.topBaseLine.time)) return {};
        if (terminal && !sending.sharedReturnJunction.isEmpty() &&
            sending.sharedReturnWith.contains(last)) {
            SearchContext geometry = source;
            Stretching junction{};
            bool corners = false;
            const auto form = resolvedLetterForm(config, sending.sharedReturnJunction);
            if (!form.isEmpty() && declaredArmRoot(geometry, form, junction, &corners) && corners &&
                (!sending.entry || orderedSections(*sending.entry, junction, source.orientation)) &&
                retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &junction, source.orientation) &&
                retainsLandmarks(receiving, &junction, nullptr, source.orientation)) {
                entry = junction;
                terminalJunction = sending.sharedReturnJunction;
            }
        }
        // A stacked join can have two normal projections through the same
        // corner. Use the head-to-stroke attachment when the bodies already
        // meet there and the horizontal section keeps their landmarks.
        if (!requiredConnector && !sending.entry && receiving.upperAttachment &&
            exit.bottomBaseLine.time == entry.bottomBaseLine.time &&
            !orderedSections(exit, entry, source.orientation) &&
            retainsLandmarks(sending, nullptr, &*receiving.upperAttachment, source.orientation))
            entry = *receiving.upperAttachment;
        exit = entry;
        sharedAtEntry = true;
    }
    const bool shared = sameSection(exit, entry) || sharesIncomingSection(exit, receiving, source.orientation);
    const auto& actualEntry = shared ? exit : entry;
    if ((receiving.fixedEntryJunction && !sameSection(actualEntry, *receiving.entry)) ||
        !withinBodyLimit(exit, exitLimitAtJoin(sending, receiving, following), source.orientation, false) ||
        !withinBodyLimit(actualEntry, entryLimitAtJoin(sending, receiving), source.orientation, true)) return {};
    if ((sending.exitLimit && !retainsLandmarks(sending, sending.entry ? &*sending.entry : nullptr, &exit,
                                                source.orientation)) ||
        (entryLimitAtJoin(sending, receiving) && !retainsLandmarks(receiving, &actualEntry,
            receiving.exit ? &*receiving.exit : nullptr, source.orientation))) return {};
    if (shared && receiving.keepsEntryConnector && !sending.entry &&
        !leavesEntryClearance(*sending.exit, *receiving.entry, source.orientation, tightTolerance)) {
        const auto chord = sectionLine(*receiving.entry);
        const auto original = sectionLine(*sending.exit);
        if (chord.length() < .75 * original.length() ||
            QLineF(chord.center(), original.center()).length() > .65 * penWidth) return {};
    }
    if (shared && keepsStrokeAtEntry(sending, receiving, source, penWidth)) return {};
    if (!shared && !sending.entry &&
        connectorContainsLeadingStroke(source.subword.extrema, exit, entry, source.orientation)) return {};
    if (receiving.requiresSharedEntry && !sameSection(exit, entry)) return {};
    if (shared && requiredConnector) return {};
    if (sending.footExitCandidate && (shared || !receiving.keepsEntryJunction)) return {};
    if (shared && ((receiving.keepsEntryJunction && !sameSection(exit, entry)) ||
                   (sending.keepsExitHollow && !useFixedJunction && !sameSection(exit, *sending.exit)))) return {};
    if (!shared && (sending.keepsExitHollow || receiving.keepsEntryJunction) &&
        !leavesEntryClearance(exit, entry, source.orientation, tightTolerance) &&
        !preserveHollow && !(continuation && receiving.keepsEntryJunction &&
                             hasShortConnector(source, exit, entry))) return {};
    if (!shared && receiving.keepsEntryJunction && !leavesSendingStroke(source, exit, penWidth) &&
        !(continuation && hasShortConnector(source, exit, entry))) return {};
    return JoinSections{exit, entry, shared, sharedAtEntry, terminalJunction};
}

bool validConnector(const QPainterPath& connector, const Apath& shape, const QLineF& right, const QLineF& left,
                    const AnchoredBody& sending, const AnchoredBody& receiving, double penWidth) {
    const double nominalWidth = receiving.connectorStrokeWidthAfter.contains(sending.formId) ? penWidth : 0;
    return !connector.boundingRect().isEmpty() && toSubpaths(connector).size() == 1 &&
        followsOneStroke(shape.transform.map(connector).translated(shape.pos), right, left,
                         ConnectorEndpointMatch::NearestVertex, nominalWidth);
}

std::optional<Subword> assemble(const Word& word, const Subword& sub, const Apath& shape,
        SearchContext& source, const QString& letters, BodySequence bodies, bool sharesFinalFold,
        const QJsonObject& config, bool continuation, const std::vector<bool>& requiredConnectors,
        double tightTolerance, double penWidth,
        std::span<const JoinSections* const> selectedJoins = {}) {
    Subword trial{};
    trial.text = sub.text;
    trial.paths = sub.paths;
    trial.secondaryBodyPath = sub.secondaryBodyPath;
    trial.bodyCounter = sub.bodyCounter;
    trial.extrema = source.subword.extrema;
    trial.basePathOrientation = source.orientation;
    trial.kashidaPaths.resize(bodies.size() - 1);
    trial.kashidaCuts.resize(bodies.size() - 1);
    std::vector<Stretching> sections;
    sections.reserve(2 * (bodies.size() - 1));
    std::vector<bool> sharedAtEntry(bodies.size() - 1);
    std::vector<std::vector<QBezier>> outer;
    QString terminalJunction;

    for (size_t slot = 1; slot < bodies.size(); ++slot) {
        const auto& sending = *bodies[slot - 1];
        const auto& receiving = *bodies[slot];
        std::optional<JoinSections> computed;
        const auto* seam = slot <= selectedJoins.size() ? selectedJoins[slot - 1] : nullptr;
        if (!seam) {
            computed = resolveJoin(source, sending, receiving, letters[int(slot) - 1], letters[int(slot)], letters.back(),
                slot + 1 == bodies.size(), sharesFinalFold, config, continuation, requiredConnectors[slot - 1], tightTolerance,
                penWidth, false);
            if (!computed) return {};
            seam = &*computed;
        }
        auto exit = seam->exit;
        const auto& entry = seam->entry;
        const bool shared = seam->shared;
        sharedAtEntry[slot - 1] = seam->usesReceivingEntry;
        if (!seam->terminalJunction.isEmpty()) terminalJunction = seam->terminalJunction;
        if (!shared && bodies[slot]->retainsCounterRim) {
            // The saved rim geometry uses the sending cap's source curve
            // parameter. Rebind it without moving either endpoint.
            if (outer.empty()) outer.push_back(baseContourCurves(source.subPaths.front()));
            if (!sourceLandmark(exit.topBaseLine, outer) || !sourceLandmark(exit.bottomBaseLine, outer)) return {};
        }
        sections.push_back(exit);
        if (!shared) {
            sections.push_back(entry);
            trial.kashidaCuts[slot - 1] = {sectionLine(exit), sectionLine(entry)};
        }
        auto join = exit;
        join.topRight = exit.topBaseLine;
        join.bottomRight = exit.bottomBaseLine;
        join.topLeft = shared ? exit.topBaseLine : entry.topBaseLine;
        join.bottomLeft = shared ? exit.bottomBaseLine : entry.bottomBaseLine;
        join.length = 0;
        join.readingOrder = int(slot - 1);
        join.fixedRightBoundary = true;
        join.fixedLeftBoundary = !shared;
        join.kashidaOverrides.insert("enabled", !shared);
        trial.joins.insert(join);
    }

    // Partition only after choosing a compatible sequence. Each section
    // comes directly from the chosen body, including the two ends of a connector.
    auto regions = cutRegions(shape.path, sub.text, sections, source.orientation);
    if (regions.size() != sections.size() + 1) return {};
    const auto inPage = [&](const QPainterPath& path) { return shape.transform.map(path).translated(shape.pos); };
    size_t region = 0;
    QStringList bodyForms;
    for (size_t slot = 0; slot < bodies.size(); ++slot) {
        const auto& body = *bodies[slot];
        const auto mask = inPage(regions[region].path);
        if (mask.boundingRect().isEmpty() || std::any_of(body.samples.begin(), body.samples.end(),
                [&](const auto& point) { return !mask.contains(point); })) return {};
        if (continuation && slot == 0 && bodies[1]->requiresSharedEntry &&
            body.counters.isEmpty()) {
            const auto validated = cutRegions(source.transformedPath, {}, {*body.exit}, source.orientation, false);
            if (validated.size() != 2 || mask.boundingRect().height() + .5 * penWidth <
                validated.front().path.boundingRect().height()) return {};
        }
        if (slot == 0 && body.sharedEntryBody &&
            body.sharedEntryBody->value("body").toObject().value("shape") == "contour-route" &&
            bodies.size() > 2 && sharedAtEntry.front()) {
            const auto proposal = sectionLine(*body.exit);
            const auto assembled = sectionLine(*trial.joins.begin());
            if (QLineF(proposal.center(), assembled.center()).length() > 1.5 * penWidth) return {};
        }
        if (!body.counters.isEmpty()) {
            const auto contours = toSubpaths(mask);
            if (std::any_of(body.counters.begin(), body.counters.end(), [&](size_t counter) {
                    return !contours.contains(source.subPaths[counter]);
                })) return {};
        }
        trial.baseGlyphs.push_back(std::move(regions[region++]));
        bodyForms.append(body.formId);
        auto diagnostic = QJsonObject{{"letterIndex", int(slot + 1)}, {"character", letters.mid(int(slot), 1)},
            {"position", slot == 0 ? "initial" : slot + 1 == bodies.size() ? "final" : "medial"},
            {"status", "matched"}, {"template", body.formId}, {"candidates", QJsonArray{body.formId}}};
        if (slot > 0 && body.coversSharedEntry && sharedAtEntry[slot - 1])
            diagnostic.insert("entryPolicy", "counter-bridge");
        if (body.forkEntry) {
            diagnostic.insert("bodyForm", "lam-fork");
            diagnostic.insert("cutPolicy", "lam-fork-flanks");
        }
        trial.letterDiagnostics.append(diagnostic);

        if (slot + 1 == bodies.size() || trial.kashidaCuts[slot].first.isNull()) continue;
        const auto& [right, left] = trial.kashidaCuts[slot];
        auto& connector = regions[region++].path;
        if (!validConnector(connector, shape, right, left, body, *bodies[slot + 1], penWidth)) return {};
        trial.kashidaPaths[slot] = std::move(connector);
    }
    for (size_t slot = 0; slot < sharedAtEntry.size(); ++slot)
        if (sharedAtEntry[slot])
            recordBoundaryTemplate(trial, int(slot), false, bodies[slot + 1]->formId);
    if (trial.kashidaPaths.back().isEmpty() &&
        bodies[bodies.size() - 2]->sharedReturnWith.contains(letters.back())) {
        const auto sender = int(bodies.size()) - 2;
        auto diagnostic = trial.letterDiagnostics[sender].toObject();
        diagnostic.insert("bodyForm", "lam-terminal-return");
        diagnostic.insert("cutPolicy", "lam-alef-shared");
        if (sender > 0 && !trial.kashidaPaths[sender - 1].isEmpty() && bodies[sender]->forkEntry) {
            const auto [tip, walk] = *bodies[sender]->forkEntry;
            diagnostic.insert("entryPolicy", "lam-fork-flank");
            diagnostic.insert("entryFork", tip);
            diagnostic.insert("entryWalk", walk);
        }
        trial.letterDiagnostics[sender] = diagnostic;
        if (!terminalJunction.isEmpty()) {
            diagnostic.insert("junctionPolicy", "inward-corners");
            trial.letterDiagnostics[sender] = diagnostic;
            recordBoundaryTemplate(trial, sender, false, terminalJunction);
            recordBoundaryTemplate(trial, sender + 1, true, terminalJunction);
        }
    }
    if (!coversOriginalInk(word, trial)) return {};
    trial.correct = true;
    trial.templateTrace.append("Fixed anchors · " + bodyForms.join(" → "));
    return trial;
}

bool appliesBeforeFinal(const QJsonObject& form, QStringView letters, int slot) {
    return hasFollowingLetter(letters, slot, form.value("fixedAnchorsBeforeFinal").toString(), true);
}

bool restrictedEnding(const QJsonObject& form, QStringView letters, int slot) {
    const auto ending = form.value("fixedAnchorsEnding").toString();
    return !ending.contains(letters.back()) && (form.contains("fixedAnchorsBeforeFinal") || !ending.isEmpty()) &&
        !appliesBeforeFinal(form, letters, slot);
}

bool allowsInitialAttachment(const QJsonObject& form, const QString& letters, int slot, bool continuation) {
    return slot == 1 && continuation &&
        form.value("cuts").toObject().value("sharedEntryAfter").toString().contains(letters.front());
}

bool applies(const QJsonObject& form, const QString& letters, int slot, bool continuation) {
    if (!form.value("current").toString().contains(letters[slot]) ||
        !form.value("kashida").toObject().isEmpty()) return false;
    const auto preceding = form.value("ligatureAfter").toString();
    if (!preceding.isEmpty() && (slot == 0 || !preceding.contains(letters[slot - 1]))) return false;
    const auto sharedFold = form.value("sharedFoldWithFinal").toString();
    if (!sharedFold.isEmpty() && !hasFollowingLetter(letters, slot, sharedFold, true)) return false;
    const auto maxLetters = form.value("fixedAnchorsMaxLetters");
    if (maxLetters.isDouble() && letters.size() > maxLetters.toInt()) return false;
    if (restrictedEnding(form, letters, slot) && !allowsInitialAttachment(form, letters, slot, continuation)) return false;
    const auto exitTemplate = form.value("requiresExitTemplate").toString();
    if (continuation && !exitTemplate.isEmpty() &&
        slot + 2 != letters.size()) return false;
    const auto attachment = form.value("cuts").toObject().value("attachment").toObject();
    const auto terminal = form.value("cuts").toObject().value("beforeFinal").toObject();
    if (slot > 0 && slot + 2 == letters.size() && terminal.value("letters").toString().contains(letters.back()) &&
        terminal.value("boundaries").toObject().value("right") == "preserve") return false;
    if (slot > 0 && form.value("body").toObject().value("shape") == "counter-attachment" &&
        !hasFollowingLetter(letters, slot, attachment.value("letters").toString())) return false;
    if (!attachment.isEmpty() && slot + 1 == letters.size() &&
        (!slot || !attachment.value("previous").toString().contains(letters[slot - 1]) ||
         (slot > 1) != (attachment.value("previousPosition") == "medial"))) return false;
    const auto following = slot == 0 ? form.value("ligatureWith").toString() : medialFinalLetters(form);
    return following.isEmpty() || hasFollowingLetter(letters, slot, following, slot > 0);
}

struct AnchorBranch {
    const QJsonObject& config;
    FormChoices forms;
    SearchContext& source;
    int limit;
    QString stemReturnEndings;
    AnchorPass pass;
    double tightTolerance;

    bool continuation() const { return pass == AnchorPass::Continuation; }
};

struct AnchorTraversal {
    const Word& word;
    const Subword& sub;
    const Apath& shape;
    const QString& letters;
    double penWidth;
    const std::vector<bool>& requiredConnectors;
    const AnchorPartitionCheck& accept;
    const QString& attachedStroke;
    AnchorSearchMemo memo;
    std::vector<const AnchoredBody*> selected;
    size_t partitionAttempts = 0;
    std::set<std::tuple<size_t, const AnchoredBody*, PartitionRank>> deadSuffixes;
    std::vector<const JoinSections*> selectedJoins;
    std::map<std::tuple<const QJsonObject*, const AnchoredBody*, const AnchoredBody*, size_t, bool>,
             std::optional<JoinSections>> resolvedJoins;
    std::map<std::tuple<const QJsonObject*, const QJsonObject*, size_t, bool>,
             std::vector<const AnchoredBody*>> choices;

    Subword refinedGeometry;
    OutlineSearchCache refinedOutline;
    std::optional<SearchContext> refinedSource;
    std::set<const AnchoredBody*> refinedCandidates;

    const std::vector<const AnchoredBody*>& readBodies(const AnchorBranch& branch, SearchContext& source,
            const QJsonObject& form, size_t slot, bool precise) {
        const auto& forms = branch.forms;
        const bool continuation = branch.continuation();
        auto [saved, inserted] = choices.try_emplace({&branch.config, &form, slot, precise});
        auto& result = saved->second;
        if (!inserted) return result;
        const bool entryTurn = appliesBeforeFinal(form, letters, int(slot));
        const auto terminal = form.value("cuts").toObject().value("beforeFinal").toObject();
        const auto exitTemplate = form.value("requiresExitTemplate").toString();
        const bool folded = form.contains("sharedFoldWithFinal");
        const bool sharedReturn = continuation && slot > 0 && slot + 2 == forms.size() && (!exitTemplate.isEmpty() ||
            (form.value("body").toObject().value("shape") == "forked-stem" &&
             terminal.value("letters").toString().contains(letters.back()) &&
             terminal.value("boundaries").toObject().value("left") == "shared-return"));
        const auto add = [&](const Stretching* seam) {
            for (const auto& body : memo.candidates(source, form, letterPosition(slot, forms.size()), slot,
                                                   forms.size(), penWidth, precise, entryTurn, branch.pass, seam))
                result.push_back(&body);
        };
        if (!folded) add(nullptr);
        if (folded || sharedReturn) {
            const auto finalId = folded ? form.value("sharedFoldForm").toString() : exitTemplate;
            for (const auto* final : forms.back()) {
                if (!finalId.isEmpty() && final->value("id") != finalId) continue;
                if (!folded && finalId.isEmpty() && !final->value("sharedBoundary").toBool()) continue;
                const size_t last = forms.size() - 1;
                for (const auto& receiving : memo.candidates(source, *final, 2, last, forms.size(), penWidth,
                                                             precise, appliesBeforeFinal(*final, letters, int(last)), branch.pass))
                    add(&*receiving.entry);
            }
        }
        return result;
    }

    const std::vector<const AnchoredBody*>& bodiesFor(const AnchorBranch& branch, const QJsonObject& form,
            size_t slot, bool precise) {
        if (!cutGeometryMemo) return readBodies(branch, branch.source, form, slot, false);
        const bool previous = cutGeometryMemo->preciseGeometry;
        const auto restore = qScopeGuard([&] {
            cutGeometryMemo->preciseGeometry = previous;
        });
        cutGeometryMemo->preciseGeometry = precise;
        if (!precise) return readBodies(branch, branch.source, form, slot, false);
        if (!refinedSource) {
            refinedSource.emplace(SearchContext{.transformedPath = branch.source.transformedPath,
                .bbox = branch.source.bbox, .subword = refinedGeometry, .outlineCache = &refinedOutline});
            if (!preparePassOutline(*refinedSource, refinedOutline, true)) {
                refinedSource.reset();
                static const std::vector<const AnchoredBody*> empty;
                return empty;
            }
        }
        // Use tighter tangent and intersection calculations for additional body
        // candidates. The original anchors and candidate order stay available.
        const auto& candidates = readBodies(branch, *refinedSource, form, slot, true);
        refinedCandidates.insert(candidates.begin(), candidates.end());
        return candidates;
    }

    bool sharedLigatureCandidates(const AnchorBranch& branch, const AnchoredBody& sender, const QJsonObject& form,
                                    size_t slot, std::vector<const AnchoredBody*>& attachments) {
        auto& source = branch.source;
        const auto& forms = branch.forms;
        const auto policy = form.value("cuts").toObject();
        const bool counter = sender.exit->counterEntry.pathIndex > 0 && policy.value("counterJoin") == "preserve-entry";
        const bool stacked = !sender.entry && sender.sharedEntryBody && sender.requiresSharedExit &&
            sender.sharedEntryBody->value("body").toObject().value("shape") == "descending-stem" &&
            form.value("tightJoin").toBool();
        const bool rim = form.contains("rimOffset") && form.contains("requiresExitTemplate") &&
            form.value("body").toObject().value("shape") == "counter-outline";
        if (form.contains("_headForms") &&
            (slot != 1 || form.value("assembly").toObject().value("entry").toObject().value("form") != sender.formId))
            return false;
        if (!counter && !stacked && !rim) return false;
        const bool precise = cutGeometryMemo && cutGeometryMemo->preciseGeometry;
        auto [saved, inserted] = memo.sharedEntries.try_emplace({&sender, memo.key(form), slot, precise});
        std::vector<const AnchoredBody*> intrinsic;
        if (inserted || rim) {
            const auto& ordinary = bodiesFor(branch, form, slot, false);
            const auto& refined = bodiesFor(branch, form, slot, true);
            intrinsic.assign(ordinary.begin(), ordinary.end());
            intrinsic.insert(intrinsic.end(), refined.begin(), refined.end());
        }
        if (inserted) {
            saved->second = anchoredBodiesAtSharedEntry(source, form, intrinsic, *sender.exit, penWidth, counter);
            std::erase_if(saved->second, [&](const auto& body) { return !fitsBody(body, source, slot, forms.size()); });
        }
        for (const auto& body : saved->second) attachments.push_back(&body);
        if (rim) attachments.insert(attachments.end(), intrinsic.begin(), intrinsic.end());
        return true;
    }

    bool sharesFinalFold(const AnchorBranch& branch) {
        auto& source = branch.source;
        const auto& config = branch.config;
        const auto& sending = *selected[selected.size() - 2];
        const auto& receiving = *selected.back();
        const auto& policy = sending.sharedFold;
        if (!policy.value("letters").toString().contains(letters.back())) return false;
        const auto descent = sectionLine(*receiving.entry).center() - sectionLine(*sending.exit).center();
        if (descent.y() < penWidth || std::abs(descent.x()) > policy.value("maxSlope").toDouble() * descent.y())
            return false;
        const auto id = policy.value("template").toString();
        const bool precise = cutGeometryMemo && cutGeometryMemo->preciseGeometry;
        auto [saved, inserted] = memo.folds.try_emplace({&config, id, precise});
        if (inserted) {
            const auto receiver = namedLetterForm(config, id);
            source.res = {};
            source.nextIter = std::next(source.xSorted.begin());
            if (!receiver.isEmpty() && declaredSharedBoundary(source, resolvedBodyForm(config, receiver)))
                saved->second = source.res;
        }
        return saved->second && sameSection(*saved->second, *receiving.entry);
    }

    JoinCompatibility follows(const AnchorBranch& branch, size_t slot, const AnchoredBody& sending,
            const AnchoredBody& body, bool checkFoldedTooth) {
        auto& source = branch.source;
        const auto tightTolerance = branch.tightTolerance;
        const auto continuation = branch.continuation();
        const bool terminal = slot + 1 == letters.size();
        const auto compatible = [&]() -> JoinCompatibility {
            const auto& exit = *sending.exit;
            if (body.requiresDeclaredReturn && sending.sharedReturnWith.contains(letters.back()) &&
                sending.sharedReturnJunction.isEmpty()) return {false};
            const bool sharedCounter = !terminal && sharesCounterSection(exit, body, source.orientation);
            const bool declaredPair = !sending.requiredExitTemplate.isEmpty();
            if (!sharedCounter && sending.requiresSharedExit && (!sameSection(exit, *body.entry) ||
                (declaredPair ? sending.requiredExitTemplate != body.formId : !terminal && !body.requiresSharedEntry)))
                return {false};
            if ((body.requiresSharedSource && !sending.requiresSharedExit) ||
                (checkFoldedTooth && separatesFoldedTooth(source, sending, body))) return {false};
            if (sharedCounter || sameSection(exit, *body.entry) ||
                orderedSections(exit, *body.entry, source.orientation) ||
                exitBeforeFollowingTooth(sending, body, source.orientation, letters[int(slot)]) ||
                (!requiredConnectors[slot - 1] &&
                    (hasOverlapLimits(sending, body, letters[int(slot)], tightTolerance, penWidth) ||
                     canResolveOverlappingAnchors(sending, body, source.orientation, letters[int(slot)]) ||
                     sharesDeclaredFixedEntry(source, sending, body, penWidth))) ||
                canUseReceivingEntry(sending, body, source.orientation, letters[int(slot) - 1], penWidth) ||
                fitsDeclaredSharedEntry(sending, body, source, letters.back(), penWidth) ||
                sharesLoopNeck(sending, body, source.orientation, tightTolerance) ||
                sharesIncomingSection(exit, body, source.orientation) || body.sharesRimEntry ||
                body.sharesJunctionEntry || sharesCornerExit(sending, *body.entry, source.orientation, tightTolerance))
                return {true};
            return {false, sending.preservesTerminalBody && terminal && body.stemmedBowl};
        };
        if (cutGeometryMemo && cutGeometryMemo->memoizeIntersections) {
            auto [entry, inserted] = memo.joins.try_emplace(
                {&sending, &body, slot, continuation, checkFoldedTooth, cutGeometryMemo->preciseGeometry, tightTolerance},
                JoinCompatibility{false});
            if (inserted) entry->second = compatible();
            return entry->second;
        }
        return compatible();
    }

    std::optional<Subword> finish(const AnchorBranch& branch) {
        auto& source = branch.source;
        const auto& config = branch.config;
        const auto tightTolerance = branch.tightTolerance;
        auto partition = assemble(word, sub, shape, source, letters, selected, sharesFinalFold(branch),
                                  config, branch.continuation(), requiredConnectors, tightTolerance, penWidth,
                                  selectedJoins);
        if (partition && (!accept || accept(*partition))) return partition;
        return {};
    }

    template<typename TryBody>
    std::optional<Subword> bodiesAfter(const AnchorBranch& branch, size_t slot, const AnchoredBody* sending,
                                      CandidateBudget& budget, bool refinedPrefix, const PartitionRank& bestRank,
                                      TryBody tryBody) {
        const bool terminal = slot + 1 == letters.size();
        if (terminal && !attachedStroke.isEmpty()) {
            if (!sending || !sending->exit || requiredConnectors[slot - 1]) return {};
            const AnchoredBody stroke{.formId = attachedStroke, .entry = sending->exit, .requiresSharedEntry = true};
            return tryBody(stroke);
        }
        const QChar last = letters.back();
        std::vector<const AnchoredBody*> rimAttachments;
        for (const auto* declaration : branch.forms[slot]) {
            const auto& form = *declaration;
            if (restrictedEnding(form, letters, int(slot)) && !allowsInitialAttachment(form, letters, int(slot), branch.continuation()) &&
                (!sending || !sending->requiresSharedExit || sending->requiredExitTemplate != form.value("id").toString()))
                continue;
            std::vector<const AnchoredBody*> paired;
            const bool hasAttachment = sending && !terminal && branch.continuation() &&
                sharedLigatureCandidates(branch, *sending, form, slot, paired);
            const auto ownedCrestEndings = form.value("cuts").toObject().value("useOwnedCrest").toString();
            const bool checkFoldedTooth = sending && terminal &&
                form.value("_foldedToothPreceders").toString().contains(letters.at(int(slot) - 1));
            const bool requiresSharedExit = branch.continuation() && form.contains("requiresExitTemplate");
            for (const bool precise : {false, true}) {
                if (precise && (hasAttachment || budget.refined == 0 || bestRank <= PartitionRank::RefinedBody)) break;
                const auto& options = hasAttachment ? paired : bodiesFor(branch, form, slot, precise);
                for (const auto* body : options) {
                    const bool usesRefinement = refinedPrefix || refinedCandidates.contains(body);
                    if (usesRefinement && bestRank <= PartitionRank::RefinedBody) continue;
                    auto& remaining = budget.forEvidence(usesRefinement);
                    if (remaining == 0) continue;
                    if (remaining > 0) --remaining;
                    if (sending && body->stemFootSkipAfter.contains(letters[int(slot) - 1])) continue;
                    if (requiresSharedExit && !body->requiresSharedExit) continue;
                    if (body->hasStemReturn && !branch.stemReturnEndings.contains(last) &&
                        !(branch.continuation() && body->sharedReturnWith.contains(last))) continue;
                    if (body->alternateAnchor && !body->hasStemReturn &&
                        !ownedCrestEndings.isEmpty() && !ownedCrestEndings.contains(last)) continue;
                    if (sending) {
                        const auto join = follows(branch, slot, *sending, *body, checkFoldedTooth);
                        if (join.stopAtBowl) {
                            if (!branch.continuation()) return {};
                            continue;
                        }
                        if (!join.allowed || !fitsAnchorGap(*sending, *body, branch.source.orientation, penWidth,
                                                           branch.continuation())) continue;
                        // Prefer bodies whose own entry leaves room for a connector.
                        // A rim attachment is useful only where the preferred caps overlap.
                        if (sending->rimExitLimit && body->acceptsCounterRim && body->entryLimit &&
                            !sameSection(*sending->exit, *body->entry) &&
                            !orderedSections(*sending->exit, *body->entry, branch.source.orientation)) {
                            rimAttachments.push_back(body);
                            continue;
                        }
                    }
                    if (auto complete = tryBody(*body)) return complete;
                }
            }
        }
        for (const auto* body : rimAttachments) {
            if (!budget.forEvidence(refinedPrefix || refinedCandidates.contains(body))) continue;
            if (auto complete = tryBody(*body)) return complete;
        }
        return {};
    }

    struct BestPartition {
        PartitionRank rank = PartitionRank::None;
        std::optional<Subword> result;
    };

    std::optional<Subword> match(const AnchorBranch& branch, CandidateBudget& budget,
                                 BestPartition& best, PartitionRank rank = PartitionRank::Preferred) {
        const bool refinedPrefix = rank == PartitionRank::RefinedBody;
        if (budget.forEvidence(refinedPrefix) == 0 || rank >= best.rank) return {};
        if (selected.size() == letters.size()) {
            ++partitionAttempts;
            auto partition = finish(branch);
            if (rank == PartitionRank::Preferred) return partition;
            // Keep the first complete partition at each rank while looking for a better one.
            if (partition) {
                best.rank = rank;
                best.result = std::move(partition);
            }
            return {};
        }
        const auto* sending = selected.empty() ? nullptr : selected.back();
        const auto suffix = std::tuple{selected.size(), sending, rank};
        if (sending && deadSuffixes.contains(suffix)) return {};
        const auto before = partitionAttempts;
        auto result = bodiesAfter(branch, selected.size(), sending, budget, refinedPrefix, best.rank,
            [&](const AnchoredBody& body) -> std::optional<Subword> {
                const bool refined = refinedCandidates.contains(&body);
                const auto bodyRank = refined ? PartitionRank::RefinedBody : body.alternateAnchor && !body.hasStemReturn
                    ? PartitionRank::SecondaryBody : PartitionRank::Preferred;
                auto candidateRank = std::max(rank, bodyRank);
                if (candidateRank >= best.rank) return {};
                const bool previous = cutGeometryMemo && cutGeometryMemo->preciseGeometry;
                if (cutGeometryMemo) cutGeometryMemo->preciseGeometry = previous || refined;
                const auto restore = qScopeGuard([&] {
                    if (cutGeometryMemo) cutGeometryMemo->preciseGeometry = previous;
                });
                selected.push_back(&body);
                bool addedJoin = false;
                const auto pop = qScopeGuard([&] {
                    selected.pop_back();
                    if (addedJoin) selectedJoins.pop_back();
                });
                std::optional<JoinSections> computed;
                if (sending) {
                    const size_t slot = selected.size() - 1;
                    const bool terminal = slot + 1 == letters.size();
                    const auto resolve = [&](PartitionRank joinRank) -> std::optional<JoinSections> {
                        const bool neckJoin = joinRank == PartitionRank::SecondaryBody;
                        auto seam = resolveJoin(branch.source, *sending, body, letters[int(slot) - 1],
                            letters[int(slot)], letters.back(),
                            terminal, terminal && sharesFinalFold(branch), branch.config, branch.continuation(),
                            requiredConnectors[slot - 1],
                            branch.tightTolerance, penWidth, neckJoin);
                        if (!seam) return {};
                        seam->rank = joinRank;
                        if (seam->shared || body.retainsCounterRim || branch.limit <= 0 ||
                            seam->exit.counterEntry.pathIndex > 0 || seam->entry.counterEntry.pathIndex > 0) return seam;
                        const auto parts = cutRegions(shape.path, {}, {seam->exit, seam->entry},
                                                      branch.source.orientation);
                        if (parts.size() != 3 || !validConnector(parts[1].path, shape, sectionLine(seam->exit),
                            sectionLine(seam->entry), *sending, body, penWidth)) return {};
                        return seam;
                    };
                    const auto chooseJoin = [&]() -> std::optional<JoinSections> {
                        if (auto preferred = resolve(PartitionRank::Preferred)) return preferred;
                        if (!requiredConnectors[slot - 1] &&
                            sharesLoopNeck(*sending, body, branch.source.orientation, branch.tightTolerance)) {
                            if (auto neck = resolve(PartitionRank::SecondaryBody)) return neck;
                        }
                        const auto following = letters[int(slot)];
                        if (!requiredConnectors[slot - 1] &&
                            canResolveOverlappingAnchors(*sending, body, branch.source.orientation, following)) {
                            if (const auto seam = resolveOverlap(branch.source, *sending, body, following, sending->minimumExitAtTurn))
                                return JoinSections{*seam, *seam, true, sameSection(*seam, *body.entry), {},
                                                    PartitionRank::AdjustedJoin};
                        }
                        return {};
                    };
                    const std::optional<JoinSections>* seam;
                    if (branch.limit > 0) {
                        auto [known, inserted] = resolvedJoins.try_emplace({&branch.config, sending, &body, slot,
                            previous || refined});
                        if (inserted) known->second = chooseJoin();
                        seam = &known->second;
                    } else {
                        computed = chooseJoin();
                        seam = &computed;
                    }
                    if (!*seam) return {};
                    candidateRank = std::max(candidateRank, (**seam).rank);
                    if (candidateRank >= best.rank) return {};
                    selectedJoins.push_back(&**seam);
                    addedJoin = true;
                }
                if (auto complete = sharedCounterPartition(branch)) {
                    if (candidateRank == PartitionRank::Preferred) return complete;
                    best.rank = candidateRank;
                    best.result = std::move(complete);
                }
                if (auto complete = match(branch, budget, best, candidateRank)) return complete;
                return {};
            });
        // A failed final partition can depend on any earlier letter.
        // Reuse only dead ends that reached no partition at all.
        if (!result && sending && budget.ordinary != 0 && budget.refined != 0 && before == partitionAttempts)
            deadSuffixes.insert(suffix);
        return result;
    }

    std::optional<Subword> sharedCounterPartition(const AnchorBranch& branch) {
        if (selected.size() + 1 != letters.size() || requiredConnectors.back()) return {};
        const auto& body = *selected.back();
        if (!body.sharedCounterReceiver.contains(letters.back())) return {};
        ++partitionAttempts;
        const AnchoredBody receiving{.formId = body.formId, .entry = body.exit, .requiresSharedEntry = true};
        selected.push_back(&receiving);
        auto partition = assemble(word, sub, shape, branch.source, letters, selected, false, branch.config,
                                  branch.continuation(), requiredConnectors, branch.tightTolerance, penWidth, selectedJoins);
        selected.pop_back();
        if (partition && (!accept || accept(*partition))) return partition;
        return {};
    }

    std::optional<AnchorBranch> prepareBranch(const AnchorPriority& priority, SearchContext& source) const {
        const auto& [config, templates, pass] = priority;
        const bool continuation = pass == AnchorPass::Continuation;
        const auto settings = config.value("kashidaDefaults").toObject();
        FormChoices forms(letters.size());
        for (int slot = 0; slot < letters.size(); ++slot) {
            if (slot == letters.size() - 1 && !attachedStroke.isEmpty()) continue;
            const auto position = letterPosition(slot, letters.size());
            const auto& byLetter = templates[position];
            const auto found = byLetter.constFind(letters[slot]);
            const auto blocked = [&](const QString& reason) -> std::optional<AnchorBranch> {
                if (cutGeometryMemo && cutGeometryMemo->profiling &&
                    cutGeometryMemo->anchorSearch == "ineligible") {
                    const std::array<const char*, 3> names{"initial", "medial", "final"};
                    cutGeometryMemo->anchorBlocker = QString("%1 %2 · %3")
                        .arg(QLatin1String(names[position]), letters[slot], reason);
                }
                return {};
            };
            if (found == byLetter.cend()) return blocked("no enabled template");
            for (const auto& form : found.value())
                if (applies(form, letters, slot, continuation)) forms[slot].push_back(&form);
            if (forms[slot].empty()) return blocked("template selectors excluded this context");
        }
        if (continuation) {
            std::stable_partition(forms.front().begin(), forms.front().end(), [&](const auto* form) {
                const auto body = form->value("body").toObject();
                const auto restore = form->value("cuts").toObject().value("restoreStackedExit").toObject();
                const bool sharedAttachment = body.value("shape") == "counter-attachment" ||
                    form->value("preferSharedAttachment").toBool() || !restore.isEmpty();
                return sharedAttachment &&
                    hasFollowingLetter(letters, 0, form->value("ligatureWith").toString());
            });
        }
        const auto search = config.value("anchorSearch").toObject();
        return AnchorBranch{config, std::move(forms), source, search.value("maxCandidates").toInt(),
                            search.value("stemReturnEndings").toString(), pass,
                            penWidth * settings.value("sharedCutTolerance").toDouble(.25)};
    }

    std::optional<Subword> search(const QPainterPath& ink, std::span<const AnchorPriority> priorities) {
        Subword geometry;
        OutlineSearchCache outline;
        SearchContext source{.transformedPath = ink, .bbox = ink.boundingRect(), .subword = geometry,
                             .outlineCache = &outline};
        if (cutGeometryMemo) cutGeometryMemo->preciseGeometry = false;
        selected.reserve(letters.size());
        std::optional<Subword> refinedPartition;
        for (const auto& priority : priorities) {
            const auto branch = prepareBranch(priority, source);
            if (!branch) continue;
            if (cutGeometryMemo) {
                if (cutGeometryMemo->profiling) {
                    cutGeometryMemo->anchorSearch = "search-exhausted";
                    cutGeometryMemo->anchorBlocker.clear();
                }
                cutGeometryMemo->memoizeIntersections = &priority != &priorities.front();
            }
            if (!outline.outline && !preparePassOutline(source, outline)) return {};
            const int limit = branch->limit > 0 ? branch->limit : -1;
            CandidateBudget budget{limit, limit};
            deadSuffixes.clear();
            BestPartition best;
            if (refinedPartition) best.rank = PartitionRank::RefinedBody;
            if (auto complete = match(*branch, budget, best)) return complete;
            if (!best.result) continue;
            if (best.rank != PartitionRank::RefinedBody) return std::move(best.result);
            if (!refinedPartition) refinedPartition = std::move(best.result);
        }
        // Prefer ordinary contour evidence across all configured passes.
        return refinedPartition;
    }
};

} // namespace

std::optional<WordResultInfo::SubWordInfo> segmentWithAnchors(const Word& word, const Subword& sub,
        std::span<const AnchorPriority> priorities, const AnchorPartitionCheck& accept, const QString& attachedStroke) {
    if (priorities.empty() || sub.paths.empty() || sub.paths.front() < 0 ||
        sub.paths.front() >= int(word.paths.size()) || (sub.secondaryBodyPath >= 0 && !sub.bodyCounter)) return {};
    const auto letters = PageAnalysisResult::bodyLetters(sub.text);
    if (letters.size() < 2) return {};
    const auto connectors = explicitConnectors(sub.text, letters);
    if (!connectors) return {};
    const auto shape = word.bodyShape(sub);
    auto ink = shape.transform.map(shape.path).translated(shape.pos);
    ink.setFillRule(Qt::WindingFill);
    const bool savedPrecision = cutGeometryMemo && cutGeometryMemo->preciseGeometry;
    const bool savedMemoize = cutGeometryMemo && cutGeometryMemo->memoizeIntersections;
    const auto restoreGeometry = qScopeGuard([&] {
        if (!cutGeometryMemo) return;
        cutGeometryMemo->preciseGeometry = savedPrecision;
        cutGeometryMemo->memoizeIntersections = savedMemoize;
    });

    AnchorTraversal traversal{word, sub, shape, letters, priorities.front().config.value("strokeWidth").toDouble(1),
                              *connectors, accept, attachedStroke};
    auto result = traversal.search(ink, priorities);
    if (result && cutGeometryMemo && cutGeometryMemo->profiling) cutGeometryMemo->anchorSearch = "matched";
    return result;
}

} // namespace MedinaCutter
