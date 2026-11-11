#pragma once

#include "../pageanalysisresult.h"
#include "../letterforminspection.h"
#include "../CustomOutputDev.h"
#include "PDFDoc.h"
#include "GlobalParams.h"
#include "../afont.h"
#include "../quran.h"
#include <format>
#include "qfile.h"
#include <private/qbezier_p.h>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <QCache>

namespace MedinaCutter {

struct CandidateSearchStats {
    qint64 requests = 0, builds = 0, bodies = 0, cutPairs = 0, partitions = 0;
    qint64 placementChecks = 0, placementRejected = 0;
};

extern thread_local QJsonObject completedSearchProfile;

// Pure geometry shared by all placement/scale retries of one subword. Keys
// retain exact coordinates; failed candidates are cached as well as successes.
// Bound memory and release it when the outer segmentation call finishes.
struct CutGeometryMemo {
    std::vector<QPainterPath> outlines;
    QCache<QByteArray, std::vector<GlyphInfo>> partitions{16 * 1024 * 1024};
    QCache<QByteArray, bool> inkSegments{65536};
    size_t partitionHits = 0, partitionMisses = 0, segmentHits = 0, segmentMisses = 0;
    bool profiling = qEnvironmentVariableIsSet("MEDINA_PROFILE");
    QMap<QString, CandidateSearchStats> forms;
    CandidateSearchStats* activeForm = nullptr;

    int outlineId(const QPainterPath& ink) {
        for (size_t i = outlines.size(); i > 0; --i)
            if (outlines[i - 1].fillRule() == ink.fillRule() && outlines[i - 1] == ink) return int(i - 1);
        if (outlines.size() == 64) return -1;
        outlines.push_back(ink);
        return int(outlines.size() - 1);
    }
};

extern thread_local CutGeometryMemo* cutGeometryMemo;

struct CutGeometryMemoScope {
    std::unique_ptr<CutGeometryMemo> owned;
    CutGeometryMemoScope() {
        if (!cutGeometryMemo) {
            owned = std::make_unique<CutGeometryMemo>();
            cutGeometryMemo = owned.get();
        }
    }
    ~CutGeometryMemoScope() {
        if (!owned) return;
        completedSearchProfile = {};
        if (owned->profiling) {
            QJsonObject forms;
            for (auto it = owned->forms.cbegin(); it != owned->forms.cend(); ++it) {
                const auto& s = it.value();
                forms.insert(it.key(), QJsonObject{{"requests", s.requests}, {"builds", s.builds},
                    {"reused", s.requests - s.builds}, {"bodyCandidates", s.bodies},
                    {"cutPairs", s.cutPairs}, {"partitionChecks", s.partitions},
                    {"placementChecks", s.placementChecks}, {"placementRejected", s.placementRejected}});
            }
            completedSearchProfile = {{"forms", forms},
                {"partitionRequests", qint64(owned->partitionHits + owned->partitionMisses)},
                {"partitionComputations", qint64(owned->partitionMisses)}};
        }
        cutGeometryMemo = nullptr;
    }
};

struct CandidateSearchScope {
    CandidateSearchStats* stats = nullptr;
    CandidateSearchStats* previous = nullptr;
    explicit CandidateSearchScope(const QJsonObject& form) {
        if (!cutGeometryMemo || !cutGeometryMemo->profiling) return;
        stats = &cutGeometryMemo->forms[form.value("id").toString()];
        previous = cutGeometryMemo->activeForm;
        cutGeometryMemo->activeForm = stats;
        ++stats->requests;
    }
    ~CandidateSearchScope() { if (stats) cutGeometryMemo->activeForm = previous; }
};

template<class T> void appendGeometryKey(QByteArray& key, const T& value) {
    key.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendGeometryPoint(QByteArray& key, const QPointF& point);

void appendGeometryAnchor(QByteArray& key, const PathExtrema& point);

extern thread_local LetterFormInspection* formInspection;

extern thread_local bool inspectingForm;

extern thread_local bool inspectionCoordinates;

struct InspectFormScope {
    bool previous = inspectingForm;
    bool previousCoordinates = inspectionCoordinates;
    explicit InspectFormScope(const QString& id) {
        inspectingForm = formInspection && id == formInspection->formId;
        if (inspectingForm) formInspection->attempted = true;
    }
    explicit InspectFormScope(const QJsonObject& form)
        : InspectFormScope(form.value("_finalRetryFor").toString(form.value("id").toString())) {}
    InspectFormScope(const QJsonObject& form, const QPainterPath& ink) : InspectFormScope(form) {
        if (inspectingForm) {
            inspectionCoordinates = ink == formInspection->ink;
            if (!inspectionCoordinates)
                ++formInspection->reasons["Preview: Alternative recovery outline; its landmarks/proposals are omitted. Assembled cuts use the original page coordinates."];
        }
    }
    ~InspectFormScope() { inspectingForm = previous; inspectionCoordinates = previousCoordinates; }
};

void inspectReason(const char* stage, const QString& reason);

void inspectReason(const char* stage, const char* reason);

void inspectLandmark(const char* name, const PathExtrema& point);

void inspectCandidate(const QString& label, const Stretching& left,
                             const std::optional<Stretching>& right = {});

struct ByContourTime {
    bool operator()(const PathExtrema& a, const PathExtrema& b) const {
        return a.time > b.time;
    }
};
using timesortedtype = std::set<PathExtrema, ByContourTime>;

struct ByDescendingX {
    bool operator()(const PathExtrema& a, const PathExtrema& b) const {
        return a.point.x() == b.point.x() ? a.point.y() > b.point.y() : a.point.x() > b.point.x();
    }
};
using xsortedtype = std::set<PathExtrema, ByDescendingX>;

struct ByDescendingY {
    bool operator()(const PathExtrema& a, const PathExtrema& b) const {
        return a.point.y() == b.point.y() ? a.point.x() > b.point.x() : a.point.y() > b.point.y();
    }
};
using ysortedtype = std::set<PathExtrema, ByDescendingY>;

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

struct PreparedPassOutline;

struct ToothCandidateMemo;

struct LamCandidateMemo;

struct PreparedPassRules {
    QJsonObject config;
    bool recoverTeeth;
    QJsonArray boundaries, finals, alefTurns;
    QMap<QString, QJsonObject> constraints;
};

struct ContourRouteSearch {
    size_t position = 0;
    size_t count = 1;
};

} // namespace MedinaCutter

// Shared by retries on a single, fixed outline in page coordinates.
struct OutlineSearchCache {
    std::shared_ptr<const MedinaCutter::PreparedPassOutline> outline;
    std::vector<std::unique_ptr<MedinaCutter::PreparedPassRules>> rules;
    std::map<std::pair<double, double>, MedinaCutter::StrokeNormalSample> normalsByCenter;
    std::map<std::pair<QString, bool>, std::optional<Stretching>> terminalCuts;
    std::map<QByteArray, std::vector<std::optional<Stretching>>> routeCuts;
    std::map<std::pair<QString, double>, std::vector<std::pair<Stretching, Stretching>>> contourBodies;
    // Interior points on the two lips found in an adjoining connector.
    std::map<int, QLineF> misplacedHeads;
    std::map<std::pair<QByteArray, int>, std::shared_ptr<const MedinaCutter::LamMedialBodyEvidence>> medialLamBodies;
    std::map<std::pair<QByteArray, int>, std::shared_ptr<const MedinaCutter::SharedShapeEvidence>> sharedBodies;
    std::shared_ptr<MedinaCutter::ToothCandidateMemo> toothCandidates;
    std::shared_ptr<MedinaCutter::LamCandidateMemo> lamCandidates;
};

namespace MedinaCutter {

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

InkOutline inkOutline(const SearchContext &context);

struct PreparedPassOutline {
    QPainterPath ink;
    std::vector<QPainterPath> contours;
    QVector<PathExtrema> extrema;
    xsortedtype x;
    ysortedtype y;
    timesortedtype time;
    Orientation orientation;
};

bool preparePassOutline(SearchContext& context, OutlineSearchCache& cache);

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

JoinTime getJoinTime(Stretching join);

JoinTime getJoinTime(Stretching join, Orientation pathOrient);

bool sharesLowerForkTip(const Stretching& right, const Stretching& left);

bool cutPrecedes(const Stretching& right, const Stretching& left, Orientation orientation);

JoinTime getJoinTime(SearchContext& context);

std::span<double> cubicRoots(double a, double b, double c, double d, std::array<double, 3>& storage);

struct ProbeBounds {
    double left, right, top, bottom;
};

void FindInterections(const QBezier& b, const QLineF& line, int pathIndex, int curveIndex,
                             std::vector<PathExtrema>& intersections, const ProbeBounds* probeBounds);

std::vector<PathExtrema> FindInterections(const QPainterPath& path, const QLineF& line, bool segment = false);

QJsonObject medialLamShortExit(const QJsonObject& config);

QJsonObject withOverrides(QJsonObject defaults, const QJsonObject& overrides);

QString invalidWalkSettings(const QJsonObject& route, const QString& location);

QJsonObject namedLetterForm(const QJsonObject& config, const QString& id);

bool toothPairDescription(const QJsonObject& body);

bool bentToothDescription(const QJsonObject& body);

bool toothDescription(const QJsonObject& body);

bool hasToothBody(const QJsonObject& form);

QJsonObject toothRecognitionSettings(const QJsonObject& form);

QJsonObject toothPlacementSettings(const QJsonObject& form);

QJsonObject formDescription(const QJsonObject& config, const QJsonObject& form,
        const QString& field, QString* error = nullptr, QSet<QString> parents = {});

QJsonObject resolvedBodyForm(const QJsonObject& config, QJsonObject form);

bool lamDescription(const QJsonObject &body);

struct SharedShapeSchema {
    QString section;
    QStringList boundaries;
};

const QHash<QString, SharedShapeSchema> &sharedShapeSchemas();

bool sharedShapeDescription(const QJsonObject &body);

QJsonObject mergeShapeSettings(QJsonObject body, const QJsonObject &cuts);

QJsonObject shapeSettings(const QJsonObject &form, const QString &key);

bool hasSeenBody(const QJsonObject &form);

QJsonObject seenSettings(const QJsonObject &form);

QJsonObject loopSettings(const QJsonObject &form);

QJsonObject loopHeadSettings(const QJsonObject &form);

QJsonObject pairedRouteSettings(const QJsonObject &form);

QJsonObject pairedCounterSettings(const QJsonObject &form);

QJsonObject openHeadSettings(const QJsonObject &form);

bool hasCounterOutline(const QJsonObject &form);

QJsonObject counterOutlineSettings(const QJsonObject &form);

QJsonObject descendingStemDescription(const QJsonObject& config, const QJsonObject& form);

QJsonObject bodyConstraintForm(const QJsonObject& config, QJsonObject form);

QJsonArray finalRetryCandidates(const QJsonArray& forms, QHash<QString, QString>& parents);

const PreparedPassRules& preparePassRules(const QJsonObject& config, bool recoverTeeth,
                                                OutlineSearchCache& cache);

QJsonObject bodyCutPolicy(const QJsonObject& description, const QString& letters, int slot);

bool within(const QJsonObject& bounds, const char* min, const char* max, double value);

bool matchesOffset(const QJsonObject& bounds, const QPointF& delta);

bool matchesNeighbors(const QJsonArray& relations, const Stretching& join);

bool matchesJoinFeatures(SearchContext& context);

extern std::function<bool(xsortedtype::iterator)> defaultCheck;

struct JoinCheck {
	bool bottomCheck = true;
	bool topCheck = true;
	bool sameDirectionCheck = true;
	std::function<bool(xsortedtype::iterator)> customCheck = defaultCheck;
};

struct BowlProbeBody {
    PathExtrema extremum;
    std::optional<PathExtrema> smooth;
};

struct NeckBody {
    PathExtrema cursor;
    Stretching section;
    std::optional<BowlProbeBody> probe;
    std::vector<PathExtrema> edges;
};

std::vector<NeckBody> recognizeNeckBodies(const InkOutline &outline, const QJsonObject &bounds,
                                                 const JoinCheck &joinCheck = {});

std::optional<BowlProbeBody> recognizeBowlProbe(const InkOutline &outline, const Stretching &section,
                                                       int lookahead);

bool cutBowlNecks(SearchContext &context, const std::vector<NeckBody> &bodies, const QJsonObject &policy);

bool interiorChord(const QPainterPath &ink, const PathExtrema &top, const PathExtrema &bottom,
                          double topTolerance = 0);

std::vector<NeckBody> recognizeProfileNecks(const InkOutline &outline, const QJsonObject &body);

bool cutProfileNeck(SearchContext &context, const std::vector<NeckBody> &candidates,
                           const NeckBody **selected = nullptr, const QJsonObject &probe = {});

// The free end locates a bowl's own descending-to-rising turn. Probes are
// deferred to cut policy, so changing probe width cannot move this landmark.
struct BowlTurnBody {
    PathExtrema tip, turn;
    std::optional<std::pair<PathExtrema, PathExtrema>> junction;
};

std::optional<BowlTurnBody> recognizeBowlTurn(const InkOutline &outline, const QJsonObject &junction);

bool cutBowlTurn(SearchContext &context, const BowlTurnBody &body, const QJsonObject &cuts);

struct OpenBowlBody {
    PathExtrema tip, floor, shoulder;
    double stroke, tangentSample;
    bool forward;
    std::optional<PathExtrema> crest, exit;
};

std::optional<OpenBowlBody> recognizeOpenBowl(const InkOutline& outline, const QJsonObject& body);
bool retainsOpenBowl(const SearchContext& context, const OpenBowlBody& body, const Stretching& cut);
bool cutOpenBowl(SearchContext& context, const OpenBowlBody& body, const QJsonObject& cuts);

bool refineContourJunction(SearchContext& context, const QJsonObject& policy);

struct LayeredHeadBody {
    std::vector<NeckBody> necks;
    std::vector<QRectF> envelopes;
};

LayeredHeadBody recognizeLayeredHeads(const InkOutline &outline, const QJsonObject &body);

bool cutLayeredHead(SearchContext &context, const LayeredHeadBody &body, const QJsonObject &policy);

bool strokeNormalCut(const SearchContext &context, Stretching &cut, double offsetX = 0);

bool anchoredStrokeNormalCut(const SearchContext &context, Stretching &cut, bool forward, bool anchorTop = false);

std::vector<QBezier> baseContourCurves(const QPainterPath &path);

bool contourNormalCut(const SearchContext& context, const PathExtrema& anchor,
                      bool increasingTime, bool atStepEnd, Stretching& cut, bool aboveOnly = false,
                      double tangentSample = 0);

bool advanceOnContour(const QPainterPath& path, const PathExtrema& start,
                             const PathExtrema& end, bool increasingTime,
                             double distance, PathExtrema& point, QPointF& tangent);

bool alefTurnCut(const SearchContext& context, const PathExtrema& limit,
                        double angle, double stroke, Stretching& cut,
                        const PathExtrema* crest = nullptr, int traversal = 0, bool oppositeFlank = false);

std::optional<Stretching> strokeFlankTurn(const SearchContext& context, const PathExtrema& start,
        double stroke, int horizontal, bool descending, double angle, double minTravel);

// Locate the shaft and its lower turns before choosing the exit normal.
struct UprightStemBody {
    PathExtrema tip;
    double y, stroke;
    std::vector<PathExtrema> turns;
};

std::optional<UprightStemBody> recognizeUprightStem(const InkOutline &outline);

bool cutUprightStem(const SearchContext &context, const UprightStemBody &body, const QJsonObject &form,
                           Stretching &result);

std::optional<PathExtrema> horizontalOpposite(const SearchContext& context,
                                                     const PathExtrema& anchor, bool left);

struct ForkRefinement {
    Stretching originalExit;
    PathExtrema corner;
};

LamBodyEvidence recognizeLamBodies(const SearchContext& context, double flankSpan, bool allowLevelCrest);

std::optional<Stretching> cutLamForkFlank(const SearchContext& context, const PathExtrema& tip,
                                               std::optional<PathExtrema> end, bool forward, int side, double distance);

bool lamAnchorWithinWalk(const SearchContext& context, const PathExtrema& tip,
                               const Stretching& cut, bool forward, double distance);

std::optional<Stretching> levelForkEntry(SearchContext& context, const PathExtrema& corner,
                                               const Stretching& exit);

// Landmark recognition and cut projection share contour arithmetic, but not
// state: a route can be recognized once and cut with different policies.
using ContourAdvance = timesortedtype::iterator (SearchContext::*)(timesortedtype::iterator) const;

struct ContourRouteEvidence {
    xsortedtype::iterator start;
    timesortedtype::iterator origin;
    ContourAdvance forward, topDirection;
    PathExtrema anchor{}, topBefore{}, topEnd{};
    double endpointTolerance = 0;
};

std::optional<ContourRouteEvidence> recognizeContourRoute(SearchContext& context,
        const QJsonObject& form, const PathExtrema* crest = nullptr, bool reverseTop = false);

bool projectContourRoute(SearchContext& context, const QJsonObject& form,
        const ContourRouteEvidence& evidence, std::optional<ForkRefinement>* fork = nullptr);

bool searchContourWalk(SearchContext& context, const PathExtrema* crest = nullptr,
                              bool reverseTop = false, bool anchorOnly = false,
                              std::optional<ForkRefinement>* fork = nullptr);

bool declaredContourRouteCut(SearchContext& context, const QJsonObject& bodyRoute,
        const QJsonObject& cutPolicy, const PathExtrema* crest = nullptr,
        bool reverse = false, std::optional<ForkRefinement>* fork = nullptr);

// A route is saved by value, so recognition can run before any neighboring
// boundary exists. Projection rebinds these landmarks to the same source.
struct SavedContourRoute {
    PathExtrema start, origin, anchor, topBefore, topEnd;
    bool forwardPrevious, topPrevious;
    double endpointTolerance;
};

std::optional<SavedContourRoute> recognizeSavedRoute(SearchContext &geometry, const QJsonObject &route,
                                                            const PathExtrema *crest = nullptr, bool reverse = false);

bool projectSavedRoute(SearchContext &context, const std::optional<SavedContourRoute> &stored,
                              const QJsonObject &route, const QJsonObject &policy,
                              std::optional<ForkRefinement> *fork = nullptr);

QVector<QPainterPath> toSubpaths(const QPainterPath& path);

std::vector<GlyphInfo> computeCutRegions(const QPainterPath& path, const QString& text,
                                         const std::vector<Stretching>& cuts, Orientation orientation,
                                         bool expanded);

std::vector<GlyphInfo> cutRegions(const QPainterPath& path, const QString& text,
                                       const std::vector<Stretching>& cuts, Orientation orientation,
                                       bool expanded = false);

bool computeInteriorInkSegment(const QPainterPath& ink, const QPointF& top, const QPointF& bottom,
                                     double topTolerance);

bool interiorInkSegment(const QPainterPath& ink, const QPointF& top, const QPointF& bottom,
                               double topTolerance = 0);

bool findJoinEnds(const QPainterPath &ink, const Stretching &join, double minX, double maxX,
                         const QJsonObject &settings, Stretching &left, Stretching &right);

bool medialChordAt(const SearchContext& context, double x, double y,
                          double tolerance, Stretching& cut, bool useBottom = false,
                          double minY = -std::numeric_limits<double>::infinity());

bool containsHeadLips(const QPainterPath& body, const QJsonObject& lips, QLineF* evidence = nullptr);

bool matchesBoundaryBody(SearchContext& context, const QJsonObject& rule,
                                const Stretching& boundary, bool initial, const QJsonObject& initialLips = {},
                                QString* reason = nullptr);

std::optional<Stretching> seenEntryCornerPair(SearchContext& context, const PathExtrema& tip,
                                                    bool increasingTime, double maxDistance);

bool seenTipMeetsFold(SearchContext& context, const PathExtrema& tip, bool increasingTime,
                             const Stretching& cut);

enum class SeenExitMode { Configured, ShiftedBend, AlongContour };

} // namespace MedinaCutter

struct SeenExitRetry {
    int slot;
    MedinaCutter::SeenExitMode mode;
    bool wideLastPitch = false;

    static QJsonValue offset(const QJsonObject& settings, MedinaCutter::SeenExitMode mode) {
        if (mode == MedinaCutter::SeenExitMode::AlongContour && settings.value("exitBendFirst").toBool())
            return settings.value("exitOffset");
        return settings.value("retryBendOffset");
    }

    bool supports(const QJsonObject& settings) const {
        return (!wideLastPitch || settings.value("retryMaxLastPitch").isDouble()) &&
               (mode == MedinaCutter::SeenExitMode::Configured || offset(settings, mode).isDouble());
    }
};

namespace MedinaCutter {

// Find the three prominent tips and their intervening valleys on the
// same contour, with an open (counter-free) body between the cuts.
struct ToothClusterBody {
    std::array<PathExtrema, 5> turns;
    bool advancePrevious, ordinary, wide;
    double minimumX;
};

std::vector<ToothClusterBody> recognizeToothClusters(const InkOutline &outline, const QJsonObject &form);

struct ToothClusterAttachment {
    std::optional<Stretching> entry, exit, receiving;
    std::optional<Stretching> followingFold;
    bool sameContour = false;
    bool separateConnector = false;
    bool requiresFold = false;
    bool canFitHollow = false;
};

struct ToothClusterExit {
    Stretching cut;
    bool atHollow = false;
};

struct ToothClusterFit {
    QString error;
    std::vector<ToothClusterExit> exits;
};

ToothClusterFit fitToothCluster(SearchContext& context, const QJsonObject& body, const QJsonObject& cuts,
                               const QPainterPath& mask, bool terminal, double penWidth,
                               const ToothClusterAttachment& attachment, bool proposeExit);

bool cutToothClusters(SearchContext &context, const std::vector<ToothClusterBody> &bodies,
                             const QJsonObject &form, double minX, double maxX, Stretching *right, Stretching *left,
                             const Stretching *finalFold = nullptr, std::array<PathExtrema, 3> *matchedTips = nullptr,
                             const SeenExitRetry *retry = nullptr);

bool seenTeethBoundaries(SearchContext &context, const QJsonObject &form, double minX, double maxX,
                                Stretching *right, Stretching *left, const Stretching *finalFold = nullptr,
                                std::array<PathExtrema, 3> *matchedTips = nullptr,
                                const SeenExitRetry *retry = nullptr);

struct HorizontalCounterPair {
    QRectF right, left;
    double height;
};

std::optional<HorizontalCounterPair> recognizeHorizontalPair(const InkOutline &outline);

bool cutHorizontalPair(SearchContext &context, const HorizontalCounterPair &body, const QJsonObject &policy);

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
    bool sampled = false;
};

struct CounterCornerEvidence {
    QRectF counter;
    double maxDistance = 0, upperLimit = 0;
    bool upper = false, top = false, paired = false;
    std::vector<CounterCornerBody> corners;
};

CounterCornerEvidence recognizeCounterCorners(const InkOutline &outline, const QRectF &counter,
                                                     const QJsonObject &settings);

bool counterEntryCorner(SearchContext &context, const QRectF &counter, const QJsonObject &settings,
                               Stretching &cut);

// The outgoing neck is the reflected version of the same intrinsic corner
// feature. Reflection preserves Bezier times, so the resulting cut still
// addresses the original contour. No neighboring boundary supplies its anchor.
struct CounterHole {
    size_t index;
    QRectF box;
};

std::vector<CounterHole> recognizeCounterHoles(const InkOutline &outline);

std::vector<Stretching> counterRimEntryCuts(SearchContext &context, const QRectF &counter,
                                           const Stretching &entry);

bool cutFinalCounterBody(SearchContext &context, const QRectF &counter, const CounterCornerEvidence &entry,
                                const QJsonObject &bounds);

// The initial loop ends at the inward corner below and to the left of its
// counter. Locate that corner from the loop itself, independent of the next letter.
struct InitialCounterBody {
    QRectF counter;
    std::vector<PathExtrema> valleys, normalAnchors;
};

std::optional<InitialCounterBody> recognizeInitialCounterBody(const InkOutline &outline,
                                                                     const QJsonObject &settings);

bool cutInitialCounterBody(SearchContext &context, const InitialCounterBody &body, const QJsonObject &settings);

// Initial feh and qaf share a loop suspended above their outgoing stroke.
// Its own counter identifies the head; its outer left extremum locates the
// outgoing boundary. Neither the following letter nor its cut supplies a
// landmark, baseline, or measurement scale.
struct LoopHeadBody {
    QRectF counter;
    std::optional<PathExtrema> edge;
    struct Neck {
        PathExtrema crest;
        double distance;
        std::optional<SavedContourRoute> route;
    };
    std::vector<Neck> necks;
};

std::optional<LoopHeadBody> recognizeLoopHead(const InkOutline &outline, const QJsonObject &settings);

bool cutLoopHead(SearchContext &context, const LoopHeadBody &evidence, const QJsonObject &bodySettings,
                        const QJsonObject &policy);

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

Stretching loopToothExit(const SearchContext& context, const ToothBody& tooth);

struct ToothBodyEvidence;

using LoopTeeth = std::vector<ToothBodyEvidence>;

std::shared_ptr<const LoopTeeth> recognizeLoopTeeth(const InkOutline &outline, const QJsonObject &body);

std::vector<ToothBody> cutLoopTeeth(SearchContext &context, const std::shared_ptr<const LoopTeeth> &bodies,
                                           const QJsonObject &policy);

struct LoopEntryRoute {
    PathExtrema crest;
    double gap;
    std::optional<SavedContourRoute> route;
};

// These corners belong to the lower counter of the same stacked pair,
// independently of the neighboring bodies and cuts.
struct CounterFoot {
    PathExtrema anchor;
    QPointF tangent;
    double distance;
    bool hasFoot;
};

struct StackedCounterPair {
    QRectF upper, lower;
    std::vector<CounterFoot> right, left;
};

struct LoopBodyEvidence {
    CounterHole anchor;
    QRectF hole;
    double bandY = -1;
    bool valid = false;
    CounterCornerEvidence entry, exit;
    std::optional<CounterCornerEvidence> lowerEntry;
    std::vector<LoopEntryRoute> entryRoutes;
    std::shared_ptr<const LoopTeeth> teeth;
    std::optional<StackedCounterPair> stackedFeet;
};

std::vector<LoopBodyEvidence> recognizeLoopBodies(const InkOutline &outline, const QJsonObject &body);

std::optional<std::pair<Stretching, Stretching>>
cutLoopBody(SearchContext &context, const LoopBodyEvidence &body, const QJsonObject &settings, bool needEntry);

const LoopBodyEvidence *selectLoopBody(const SearchContext &context, const std::vector<LoopBodyEvidence> &bodies,
                                              double minX, double maxX, const QPainterPath *availableInk);

std::optional<std::pair<Stretching, Stretching>>
medialLoopBoundaries(SearchContext &context, const QJsonObject &settings, double minX, double maxX,
                     bool needEntry = true, const QPainterPath *availableInk = nullptr);

// The right face of a tall tooth descends to its own baseline turn. Use the
// local pen thickness to distinguish that face from a shallow joining stroke.
struct ToothEntryLandmark {
    PathExtrema foot;
    bool forward;
};

void extendToothToBaseline(SearchContext& context, ToothBody& body,
        const QJsonObject& entry, const QJsonObject& policy);

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

// Rounded shoulders and curled hooks include extrema at cubic junctions.
// Keep the resulting landmark inventory with the body evidence so projection
// uses exactly that contour vocabulary, even after a cut policy is edited.
struct BentToothRecognition {
    bool refined = false;
    timesortedtype landmarks;
    std::vector<BentToothBodyEvidence> bodies;
};

BentToothRecognition recognizeBentToothBodies(SearchContext& context, const QJsonObject& body);

std::vector<ToothBody> cutBentToothBodies(SearchContext& context,
        const BentToothRecognition& evidence, const QJsonObject& policy);

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

std::vector<ToothBodyEvidence> recognizeToothBodies(
        SearchContext& context, const QJsonObject& settings);

std::vector<ToothBody> cutToothBodies(SearchContext& context,
        const std::vector<ToothBodyEvidence>& bodies, const QJsonObject& settings, bool bothSides = true);

std::vector<ToothBody> computeToothBoundaries(SearchContext& context, const QJsonObject& form);

struct ToothCandidateMemo {
    QPainterPath source;
    Orientation orientation;
    struct Entry { QJsonObject form; std::vector<ToothBody> candidates; };
    QMap<QString, Entry> forms;
};

std::vector<ToothBody> declaredToothBoundaries(SearchContext& context, const QJsonObject& form);

std::vector<StackedCounterPair> recognizeStackedPairs(const InkOutline &outline, const QJsonObject &settings);

std::vector<std::pair<Stretching, Stretching>>
cutStackedPairs(SearchContext &context, const std::vector<StackedCounterPair> &bodies, const QJsonObject &policy);

timesortedtype::iterator headFlank(SearchContext& context, timesortedtype::iterator crest,
                                         bool increasingTime);

// The open head has a crest flanked by a rightmost and leftmost contour
// extremum, without an enclosed counter. Project those two edges onto the
// joining stroke below the head. All proportions are relative to this head.
struct OpenHeadBody {
    PathExtrema crest, rightEdge, leftEdge;
    double height;
};

std::vector<OpenHeadBody> recognizeOpenHeads(const InkOutline &outline, const QJsonObject &settings);

std::vector<OpenHeadBody> selectOpenHeads(const std::vector<OpenHeadBody> &bodies, double minX, double maxX);

std::vector<std::pair<Stretching, Stretching>>
cutOpenHeads(SearchContext &context, const std::vector<OpenHeadBody> &bodies, const QJsonObject &policy);

// A medial feh/qaf head encloses one counter. Its crest and side edges locate
// that counter, which supplies the scale and baseline for both neck cuts.
// The resulting body must retain the entire loop and the crest. Neighboring
// letters supply neither landmarks nor stroke-width/height estimates.
struct CrestedLoopBody {
    QRectF counter;
    PathExtrema crest, rightEdge, leftEdge;
};

std::vector<CrestedLoopBody> recognizeCrestedLoops(const InkOutline &outline, const QJsonObject &settings);

std::vector<std::pair<Stretching, Stretching>>
cutCrestedLoops(SearchContext &context, const std::vector<CrestedLoopBody> &bodies, const QJsonObject &policy);

std::optional<PathExtrema> contourFlank(SearchContext& context, timesortedtype::iterator anchor,
                                               bool previous, const QJsonObject& bounds, double stepTolerance = 0);

// Reusable paired contour routes, anchored on a feature of the body itself.
struct PairedRouteBody {
    PathExtrema anchor;
    std::vector<double> retained;
    std::optional<SavedContourRoute> right, left;
};

std::vector<PairedRouteBody> recognizePairedRoutes(const InkOutline &outline, const QJsonObject &settings);

std::vector<std::pair<Stretching, Stretching>> cutPairedRoutes(SearchContext &context,
                                                                      const std::vector<PairedRouteBody> &bodies,
                                                                      const QJsonObject &body, const QJsonObject &cuts,
                                                                      const Stretching *sharedExit = nullptr);

std::vector<std::pair<Stretching, Stretching>>
contourBodyBoundaries(SearchContext &context, const QJsonObject &settings, const Stretching *sharedExit = nullptr);

// Locate a tall stem from its own crest and width; a bowl may extend to its left.
struct TerminalStemBody {
    PathExtrema crest, limit, bowlEdge;
    QPointF bowlBottom, sample;
    double stroke;
    int traversal;
};

std::vector<TerminalStemBody> recognizeTerminalStemBodies(const InkOutline &outline, const QJsonObject &body);

bool cutTerminalStemBodies(SearchContext& context, const std::vector<TerminalStemBody>& bodies,
        const QJsonObject& body, const QJsonObject& policy);

bool medialBodyChord(SearchContext& context, const PathExtrema& start,
                            const QJsonObject& boundary, Stretching& cut);

bool connectorContainsLeadingStroke(const QVector<PathExtrema>& extrema,
        const Stretching& right, const Stretching& left, Orientation orientation);

bool assembleBodyPartition(const Word& word, WordResultInfo::SubWordInfo& subword,
                                  const QJsonObject& config);

void applyTerminalJunction(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config);

// Start halfway down the visible terminal shaft. Its cap can have the same
// tangent as the turn at its foot, but only the foot retains this shaft.
struct ShaftFlank {
    PathExtrema start;
    bool forward;
};

struct DescendingShaftBody {
    PathExtrema tip;
    QLineF section;
    double y, width;
    std::vector<ShaftFlank> flanks;
    std::vector<PathExtrema> feet;
};

std::optional<DescendingShaftBody> recognizeDescendingShaft(const InkOutline &outline);

bool cutDescendingShaft(SearchContext &context, const DescendingShaftBody &body, const QJsonObject &form,
                               Stretching &result);

// Follow the underside of the terminal tail to the fork where the incoming
// stroke branches off. Measure the flanks over a local stroke-scaled arc,
// independently of the number of extrema used to describe that arc.
struct TailBranch {
    PathExtrema corner;
    bool forward;
    double width;
    bool recovered = false;
};

struct TailBody {
    PathExtrema tip;
    int curveCount;
    std::vector<TailBranch> branches;
};

TailBody recognizeTailBranches(const InkOutline &outline, const QJsonObject &settings);

bool cutTailBranches(SearchContext &context, const TailBody &body, const QJsonObject &settings,
                            Stretching &result, PathExtrema* shoulder = nullptr);

std::vector<std::optional<Stretching>> contourShoulderCuts(SearchContext& context, const TailBody& body,
        const QJsonObject& shoulder, const QJsonObject& range);

bool terminalTailEntry(SearchContext &context, const QJsonObject &settings, Stretching &result);

std::optional<Stretching> stackedAttachment(SearchContext& context, const QJsonObject& settings,
                                                   const QRectF& cap, const QPainterPath& head,
                                                   const Stretching& incoming, const Stretching& outgoing,
                                                   bool clippedCap = false);

std::vector<Stretching> cornerSections(const SearchContext& context, const PathExtrema& corner);

void orientSection(const SearchContext& ctx, Stretching& cut, double witness, bool left);

std::optional<Stretching> cutAfterEntryCrest(SearchContext& ctx, Stretching cut,
                                                  const Stretching* preceding = nullptr,
                                                  bool allowOblique = false);

std::optional<Stretching> restoreNoonBowlEntry(SearchContext& ctx, Stretching entry,
                                                     Stretching sending);

// Follow both flanks from the far end of the bowl. On each flank the long
// sweep returns left, then turns right into the diagonal of the upper letter.
struct ReturnSeamBody {
    PathExtrema tip, first, second;
    double shoulderX = 0;
};

bool cutReturnSeam(SearchContext &ctx, const ReturnSeamBody &body, const QJsonObject &policy, Stretching &cut,
                          double *shoulderX = nullptr);

std::optional<ReturnSeamBody> recognizeFoldedReturn(const InkOutline &outline, const QJsonObject &spec);

bool readTerminalOutline(SearchContext& context);

bool declaredArmRoot(SearchContext &ctx, const QJsonObject &form, Stretching &result,
                            bool *cornerPair = nullptr);

using LamShaftEdges = std::array<std::optional<PathExtrema>, 2>;

std::vector<size_t> lamShaftForks(const SearchContext& context, const PathExtrema& crest,
                                        const std::vector<LamForkBody>& forks,
                                        const std::vector<QBezier>& curves);

struct LamBodyRecognition {
    LamBodyEvidence lamBodies;
    std::vector<std::vector<size_t>> shaftForks;
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
    QJsonObject body, cuts;
    QByteArray key;
    QString nextLetters;
    QString sharedFoldWithFinal;
};

struct BodyCutProgram {
    QMap<QString, DeclaredBodyForm> forms;
    QMap<QString, DeclaredBodyForm> recoveryBodies;
    std::array<QHash<QChar, QVector<QString>>, 3> formsByPosition;
    QString toothClusterLetters;
};

inline bool hasFollowingLetter(QStringView letters, int slot, QStringView allowed, bool mustBeFinal = false) {
    const int next = slot + 1;
    return next < letters.size() && (!mustBeFinal || next + 1 == letters.size()) && allowed.contains(letters[next]);
}

inline bool hasTatweelBeforeFinal(QStringView text) {
    bool pastFinal = false;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        if (it->isMark()) continue;
        if (pastFinal) return *it == QChar(0x0640);
        if (*it != QChar(0x0640)) pastFinal = true;
    }
    return false;
}

const BodyCutProgram& bodyCutProgram(const QJsonObject& config);

QJsonObject terminalArmRecipe(const QJsonObject& config, const QJsonObject& form);

QByteArray bodyDescriptionKey(const QJsonObject& body);

const LamBodyRecognition* recognizedLamBody(const SubwordBodyRecognition& recognition,
                                                   const QJsonObject& description);

SubwordBodyRecognition recognizeSubwordBodies(const Word& word,
        const WordResultInfo::SubWordInfo& sub, const QJsonObject& config);

std::optional<bool> segmentDeclaredLigature(const Word& word, WordResultInfo::SubWordInfo& sub,
                                             const QJsonObject& config);
QJsonObject ligatureForm(const QJsonObject& config, const QJsonObject& selection);

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

SubwordCutPlan makeSubwordCutPlan(WordResultInfo::SubWordInfo proposals, const QJsonObject& config);

bool verifyBodyLandmarkOwnership(const Word& word, WordResultInfo::SubWordInfo& sub,
                                 const QJsonObject& config);

std::optional<ReturnSeamBody> recognizeRisingReturn(const InkOutline &outline, const QJsonObject &spec);

std::optional<QLineF> singleHorizontalStroke(const QPainterPath& body, const QRectF& bounds, double y);

// Measure each attachment from the loop's own counter. A cut is eligible only
// when the resulting body contains this entire counter and the required tail.
struct RimBodyEvidence {
    CounterHole counter;
    std::vector<PathExtrema> anchors;
};

std::vector<RimBodyEvidence> recognizeRimBodies(const InkOutline &outline);

std::vector<std::pair<Stretching, Stretching>>
cutRimBodies(SearchContext &ctx, const std::vector<RimBodyEvidence> &bodies, const QJsonObject &settings);

std::vector<std::pair<Stretching, Stretching>> rimSections(SearchContext &ctx, const QJsonObject &settings);

bool descendingStemExit(SearchContext& ctx, const QJsonObject& spec, Stretching& best);

std::vector<PathExtrema> recognizeLoopShoulders(const InkOutline &outline, const QRectF &hole,
                                                       const QJsonObject &body);

bool cutLoopShoulder(SearchContext &ctx, const QRectF &hole, const std::vector<PathExtrema> &anchors,
                            const QJsonObject &settings, double witness, bool final, Stretching &best);

// A medial open hah has a pointed re-entrant mouth. Its upper and lower
// lips run left from the same notch and diverge, each ending at a turn.
struct MouthLips {
    PathExtrema notch, upper, lower;
};

struct OpenMouthBody {
    MouthLips lips;
    PathExtrema nose;
    std::vector<PathExtrema> entryCorners;
};

std::vector<OpenMouthBody> recognizeOpenMouths(const InkOutline &outline, const QJsonObject &form);

std::vector<MouthLips> mouthLips(SearchContext &ctx, const QJsonObject &form);

std::vector<std::pair<Stretching, Stretching>>
cutOpenMouths(SearchContext &ctx, const std::vector<OpenMouthBody> &bodies, const QJsonObject &form);

// The bent medial kaf has an ascending blade and a diagonal return. On the
// return the incoming stroke forks to the right before its lower turn.
struct BladeBody {
    PathExtrema elbow, tip, turn, fork;
    bool forward;
    double width;
};

std::vector<BladeBody> recognizeBlades(const InkOutline &outline, const QJsonObject &form);

std::vector<std::pair<Stretching, Stretching>>
cutBlades(SearchContext &ctx, const std::vector<BladeBody> &bodies, const QJsonObject &form);

// In the enclosed hah the counter tapers to the right, under the protruding
// nose. The receiving and departing strokes attach below the two lips.
struct EnclosedLipsBody {
    QRectF counter;
    PathExtrema nose;
    std::array<std::vector<PathExtrema>, 2> corners;
};

std::vector<EnclosedLipsBody> recognizeEnclosedLips(const InkOutline &outline, const QJsonObject &settings);

std::vector<std::pair<Stretching, Stretching>> cutEnclosedLips(SearchContext &ctx,
                                                                      const std::vector<EnclosedLipsBody> &bodies,
                                                                      const QJsonObject &policy, bool initial);

// Locate the cap of a tall narrow shaft and follow both flanks from the
// cap. A change from descending to lateral motion gives one attachment per
// flank. Both sections must enclose the same cap and keep the shaft intact.
struct TurningShaftBody {
    PathExtrema crest;
    double stroke;
    std::array<std::optional<PathExtrema>, 2> turns;
};

std::vector<TurningShaftBody> recognizeTurningShafts(const InkOutline &outline, const QJsonObject &body);

std::vector<std::pair<Stretching, Stretching>> cutTurningShafts(SearchContext& ctx,
        const std::vector<TurningShaftBody>& bodies, const QJsonObject& shape);

std::vector<std::pair<Stretching, Stretching>> shaftSections(SearchContext& ctx, const QJsonObject& form);

// The free arm of a folded lam-alef is broad at the nib and tapers down
// and right into the returning bowl. The two inward corners delimit the
// contact when both are present; a shorter cross-section can clip its tip.
struct TaperedArmBody {
    PathExtrema first, second;
    std::array<QLineF, 3> crossSections;
    QRectF bounds;
    double width;
    bool junction;
    double fit;
};

std::vector<TaperedArmBody> recognizeTaperedArms(const InkOutline &outline, const QJsonObject &form);

bool cutTaperedArms(const std::vector<TaperedArmBody> &bodies, const QJsonObject &policy, Stretching &result,
                           bool *cornerPair = nullptr);

struct MedialCandidate {
    Stretching right;
    Stretching left;
    bool sharedJoins = false;
    std::optional<std::pair<Stretching, Stretching>> hollowCuts;
    std::optional<PathExtrema> lamCrest;
    std::optional<PathExtrema> lamFork;
};

bool declaredLamBoundary(SearchContext& context, const QJsonObject& config, const QJsonObject& form);

// One intrinsic stem occurrence, including both contour routes even when a
// neighbor will later make one of them unusable. No boundary is an input.
struct LamStemBody {
    PathExtrema crest;
    std::array<std::optional<PathExtrema>, 2> descendingFlanks;
    std::array<std::optional<SavedContourRoute>, 2> entryRoutes;
    std::vector<std::array<std::optional<SavedContourRoute>, 2>> exitRoutes;
    std::vector<size_t> forks;
};

struct LamMedialBodyEvidence {
    QPainterPath source;
    LamBodyEvidence features;
    std::vector<LamStemBody> stems;
    std::vector<TurningShaftBody> turningShafts;
};

LamMedialBodyEvidence recognizeMedialLamBodies(const InkOutline &outline, const QJsonObject &body);

std::shared_ptr<const LamMedialBodyEvidence> recognizedMedialLamBodies(
        SearchContext& context, const QJsonObject& body);

void inspectLamBodyEvidence(const LamMedialBodyEvidence& evidence);

std::vector<MedialCandidate> computeMedialLamCuts(SearchContext& context,
        const QJsonObject& config, const QJsonObject& form, const LamMedialBodyEvidence& evidence,
        const Stretching& incoming,
        const QPointF& entry, const QPointF& exit, double stroke, bool open, bool recoverTeeth,
        bool allowFork, const std::optional<Stretching>& sharedExit);

// A cached assembly is specific to its complete geometry, policies and adjacent
// boundary. It does not promote a template or skip any distinct placement.
struct LamCandidateMemo {
    QPainterPath source;
    Orientation orientation;
    std::vector<QJsonObject> objects;
    QCache<QByteArray, std::vector<MedialCandidate>> candidates{4 * 1024 * 1024};
    int objectId(const QJsonObject& object) {
        for (size_t i = 0; i < objects.size(); ++i)
            if (objects[i] == object) return int(i);
        objects.push_back(object);
        return int(objects.size() - 1);
    }
};

void appendCandidateBoundary(QByteArray& key, const Stretching& cut);

std::vector<MedialCandidate> assembleMedialLamCuts(SearchContext& context,
        const QJsonObject& config, const QJsonObject& form, const LamMedialBodyEvidence& evidence,
        const Stretching& incoming, const QPointF& entry, const QPointF& exit, double stroke,
        bool open, bool recoverTeeth, bool allowFork, const std::optional<Stretching>& sharedExit);

// At a tight loop-to-stroke junction, both edges of the bridge can end at
// corners. Join those corners while retaining the counter and the complete tail.
struct LoopBridgeBody {
    QRectF hole;
    PathExtrema tip;
    std::vector<std::pair<PathExtrema, PathExtrema>> pairs;
};

std::optional<LoopBridgeBody> recognizeLoopBridge(const InkOutline &outline);

bool cutLoopBridge(SearchContext &ctx, const LoopBridgeBody &body, const QJsonObject &settings,
                          Stretching &best);

struct CounterOutlineBody {
    std::vector<CounterHole> counters;
    QRectF eye;
};

std::vector<CounterOutlineBody> recognizeCounterOutlines(const InkOutline &outline, const QJsonObject &body);

struct ParallelReturnBody {
    PathExtrema elbow, edge, tip;
    std::pair<QLineF, QLineF> first, second;
    double stroke, slope;
    std::vector<PathExtrema> corners;
};

std::vector<ParallelReturnBody> recognizeParallelReturns(const InkOutline &outline,
                                                                const QJsonObject &settings);

std::vector<std::pair<Stretching, Stretching>>
cutParallelReturns(SearchContext &ctx, const std::vector<ParallelReturnBody> &bodies, const QJsonObject &settings);

struct BowlForkBody {
    PathExtrema tail, upper, lower;
    bool forward;
    double width;
    std::vector<PathExtrema> caps;
};

std::vector<BowlForkBody> recognizeBowlForks(const InkOutline &outline);

bool cutBowlForks(SearchContext &ctx, const std::vector<BowlForkBody> &bodies, const QJsonObject &form,
                         Stretching &result);

struct CounterAttachmentLoop {
    int index;
    QRectF box;
    std::vector<PathExtrema> inner, forks, shoulders;
};

struct CounterAttachmentBody {
    bool fork;
    std::optional<PathExtrema> outer;
    std::vector<CounterAttachmentLoop> loops;
};

CounterAttachmentBody recognizeCounterAttachments(const InkOutline &outline, const QJsonObject &form);

bool cutCounterAttachments(SearchContext &context, const CounterAttachmentBody &body, const QJsonObject &form,
                                  const CounterAttachmentLoop **selected = nullptr);

struct ComponentArmBody {
    int index;
    QRectF bounds;
    PathExtrema top, bottom;
};

std::optional<ComponentArmBody> recognizeArmEnvelope(const QPainterPath &ink, double minRise, bool leaning);

std::vector<ComponentArmBody> recognizeComponentArms(const InkOutline &outline, const QJsonObject &body);

// Source components are evidence before any letter boundary is assembled.
struct InkComponentBody {
    QPainterPath ink;
    std::vector<int> indices;
};

std::vector<InkComponentBody> recognizeSourceComponents(const QPainterPath &ink);

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
    std::optional<OpenBowlBody> openBowl;
    std::vector<NeckBody> bowlNecks;
    std::vector<ToothClusterBody> clusters;
    std::vector<EnclosedLipsBody> enclosedLips;
    std::vector<OpenMouthBody> mouths;
    std::vector<BladeBody> blades;
    std::vector<ParallelReturnBody> parallelReturns;
    std::vector<TerminalStemBody> terminalStems;
    std::vector<BowlForkBody> bowlForks;
    std::optional<CounterAttachmentBody> attachment;
    std::optional<LayeredHeadBody> layeredHead;
    std::optional<UprightStemBody> uprightStem;
    std::optional<DescendingShaftBody> descendingShaft;
    std::vector<TaperedArmBody> taperedArms;
    std::vector<ComponentArmBody> componentArms;
    std::vector<InkComponentBody> components;
    std::vector<OpenHeadBody> openHeads;
    std::optional<HorizontalCounterPair> horizontalPair;
    std::vector<StackedCounterPair> stackedPairs;
    std::optional<TailBody> tail;
    std::optional<ReturnSeamBody> returnSeam;
    std::vector<PairedRouteBody> pairedRoutes;
    std::vector<CounterOutlineBody> outlines;
};

SharedShapeEvidence recognizeSharedShape(const InkOutline &outline, const QJsonObject &body);

std::shared_ptr<const SharedShapeEvidence> recognizedSharedShape(SearchContext &context,
                                                                        const QJsonObject &body);

void inspectSharedShape(const SharedShapeEvidence &evidence);

std::vector<std::pair<Stretching, Stretching>>
declaredOpenHeadCuts(SearchContext &context, const QJsonObject &form, double minX, double maxX);

bool declaredSharedBoundary(SearchContext &context, const QJsonObject &form,
                                   const SeenExitRetry *retry = nullptr, bool recovering = false,
                                   const Stretching *finalFold = nullptr, ContourRouteSearch* routeSearch = nullptr);

} // namespace MedinaCutter

// Enumerate distinct, geometrically valid readings of a prefix only when a
// subsequent tooth is unresolved. Curves meeting at the same seam are one choice.
struct ToothSearchPlan {
    bool enumerate = false;
    int stage = 1;
    QMap<QString, std::vector<MedinaCutter::ToothBody>> teeth;
    QMap<QString, std::vector<std::pair<Stretching,Stretching>>> intrinsic;
    QVector<int> skip, choices;
    QVector<std::set<std::array<int, 4>>> encountered;
    QVector<MedinaCutter::ContourRouteSearch> routeSearches;
    explicit ToothSearchPlan(int size) : skip(size), choices(size), encountered(size), routeSearches(size) {}
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
    bool advanceRoute() {
        for (auto it = routeSearches.rbegin(); it != routeSearches.rend(); ++it) {
            if (it->position + 1 >= it->count) continue;
            ++it->position;
            for (auto following = routeSearches.rbegin(); following != it; ++following)
                following->position = 0;
            return true;
        }
        return false;
    }
};

namespace MedinaCutter {

bool proposeTemplateBoundary(SearchContext& context, const QJsonObject& config,
                                    const QJsonObject& rule, bool terminal, bool recoverTeeth,
                                    const SeenExitRetry* seenRetry, const Stretching* finalFold = nullptr,
                                    ContourRouteSearch* routeSearch = nullptr);

bool supportsOpenMedialBoundary(const QJsonObject& config, const QJsonObject& form);

bool hasIntrinsicMedialBody(const QJsonObject& config, const QJsonObject& form);

std::optional<std::vector<MedialCandidate>> proposeMedialBodies(
        SearchContext& context, const QJsonObject& form, ToothSearchPlan* plan, QChar nextLetter,
        const Stretching* precedingBody, double bodyMinX, double entryX);

void recoverIncomingToothShoulder(SearchContext& context, Stretching& incoming);

void appendMedialWalks(SearchContext& context, const QJsonObject& form,
                              const QPointF& entry, const QPointF& exit, double stroke,
                              bool open, bool allowWalk, std::vector<MedialCandidate>& candidates);

bool coversOriginalInk(const Word& word, const WordResultInfo::SubWordInfo& sub);

bool followsOneStroke(const QPainterPath& ribbon, const QLineF& right, const QLineF& left);

std::map<int, QLineF> missingHeadInk(const Word& word, const WordResultInfo::SubWordInfo& sub,
                                            const QString& letters, const QJsonObject& settings);

bool plausibleRecoveredBodies(const Word& word, const WordResultInfo::SubWordInfo& sub,
                                     const QJsonObject& settings);

bool sourceLandmark(PathExtrema& mark, const std::vector<std::vector<QBezier>>& paths);

bool isSeparateFoldedArm(const QPainterPath& body, const QPainterPath& arm, bool allowPointContact);

bool recoverExposedHead(const Word& word, WordResultInfo::SubWordInfo& sub,
                               const QJsonObject& config, const QJsonObject& checks);

bool detachedHeadEntry(Word& word, WordResultInfo::SubWordInfo& result, const QJsonObject& config);

bool stableTemplateCuts(const Word& word, WordResultInfo::SubWordInfo& sub, QJsonObject config,
                               const QJsonArray& shapeFrames = {});

bool recoverSharedTerminal(const Word& word, WordResultInfo::SubWordInfo& sub,
                              const QJsonObject& config, const QJsonObject& checks);

bool confirmCounterOutlines(const Word& word, WordResultInfo::SubWordInfo& sub, const QJsonObject& config, const QJsonObject& recoveryChecks);

bool resolveSubwordCutPlan(const Word& word, const SubwordBodyRecognition& recognition,
                                  SubwordCutPlan& plan, const QJsonObject& config);

} // namespace MedinaCutter
