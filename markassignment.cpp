#include "markassignment.h"
#include "markassignment_p.h"
#include "pageanalysisresult.h"
#include "constants.h"
#include <QChar>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDir>

namespace NativeMarks {
static const std::map<int, std::string> marks = {{0x64e, "fatha"},
                                                 {0x650, "kasra"},
                                                 {0x64f, "damma"},
                                                 {0x64b, "tanwin-fath"},
                                                 {0x64d, "tanwin-kasr"},
                                                 {0x64c, "tanwin-damm"},
                                                 {0x8f0, "tanwin-fath"},
                                                 {0x8f2, "tanwin-kasr"},
                                                 {0x8f1, "tanwin-damm"},
                                                 {0x651, "shadda"},
                                                 {0x652, "sukun"},
                                                 {0x6e1, "sukun"},
                                                 {0x653, "maddah"},
                                                 {0x654, "hamza-above"},
                                                 {0x655, "hamza-below"},
                                                 {0x670, "small-alef"},
                                                 {0x6e2, "small-meem"},
                                                 {0x6ed, "small-low-meem"},
                                                 {0x6e5, "small-waw"},
                                                 {0x8f3, "small-waw"},
                                                 {0x6e6, "small-yeh"},
                                                 {0x6e7, "small-high-yeh"},
                                                 {0x6e8, "small-noon"},
                                                 {0x6dc, "small-seen"},
                                                 {0x6e3, "small-low-seen"},
                                                 {0x6d9, "waqf-lam"},
                                                 {0x6d7, "waqf-qaf"},
                                                 {0x6da, "waqf-jeem"},
                                                 {0x6d6, "waqf-sad"},
                                                 {0x6d8, "waqf-meem"},
                                                 {0x6db, "waqf-three-dots"},
                                                 {0x6df, "rounded-zero"},
                                                 {0x6e0, "rectangular-zero"},
                                                 {0x6ec, "rounded-filled-high"},
                                                 {0x65c, "rounded-filled-low"}};
static const std::map<int, std::string> wordSigns = {{0x6de, "hizb-sign"}, {0x6e9, "quranic-sign"}};
static const std::map<std::string, int> capacities = {
    {"triple-dot", 2}, {"tanwin-fath", 2}, {"tanwin-kasr", 2},     {"tanwin-damm", 2},
    {"waqf-qaf", 2},   {"waqf-jeem", 2},   {"waqf-three-dots", 3}, {"small-noon", 2}};
static QString qstr(const J &j) { return QString::fromStdString(str(j)); }
static bool base(QChar c) { return c.isLetter() && !marks.count(c.unicode()); }
static bool inside(QChar c, const char *text) { return QString::fromUtf8(text).contains(c); }
static J expectedSlots(const J &word) {
    J out = arr();
    int offset = 0;
    for (const auto &record : word["characters"]) {
        auto text = qstr(record["text"]);
        require(text.size() == 1, "Expected one BMP transcript character");
        QChar c = text[0];
        auto position = s(record, "position");
        std::string name;
        if (marks.count(c.unicode()))
            name = marks.at(c.unicode());
        else if (inside(c, "بجخذزضظغفن"))
            name = "single-dot";
        else if (inside(c, "تقة") ||
                 (c.unicode() == 0x64a && (position == "initial" || position == "medial")))
            name = "double-dot";
        else if (inside(c, "ثش"))
            name = "triple-dot";
        else if (inside(c, "أؤئ"))
            name = "hamza-above";
        else if (c.unicode() == 0x625)
            name = "hamza-below";
        else if (c.unicode() == 0x622)
            name = "maddah";
        else if (c.unicode() == 0x671)
            name = "wasla";
        else if (c.unicode() == 0x643 && (position == "isolated" || position == "final"))
            name = "small-kaf-body";
        if (!name.empty()) {
            J side;
            const std::set<std::string> below = {"kasra", "tanwin-kasr", "hamza-below",
                                                 "small-low-meem", "small-low-seen"};
            const std::set<std::string> above = {"fatha",       "damma",  "tanwin-fath",
                                                 "tanwin-damm", "shadda", "sukun",
                                                 "maddah",      "wasla",  "hamza-above"};
            if (below.count(name) || (name == "single-dot" && inside(c, "بج")) ||
                (name == "double-dot" && c.unicode() == 0x64a))
                side = "below";
            else if (ends(name, "-dot") || above.count(name))
                side = "above";
            out.push_back({{"type", name},
                           {"characterOffset", offset},
                           {"capacity", capacities.count(name) ? capacities.at(name) : 1},
                           {"side", side}});
        }
        if (wordSigns.count(c.unicode()))
            out.push_back({{"type", wordSigns.at(c.unicode())},
                           {"characterOffset", offset},
                           {"capacity", record["nativeContourCount"]},
                           {"side", J()}});
        ++offset;
    }
    return out;
}
J expectedLetters(const J &word) {
    QString text = qstr(word["text"]), joined;
    for (const auto &c : word["characters"])
        joined += qstr(c["text"]);
    require(text == joined, "Transcript characters do not match word text");
    J buckets = arr();
    std::map<int, int> ownerAt;
    int owner = -1, ordinal = 0, offset = 0;
    for (const auto &record : word["characters"]) {
        auto letter = qstr(record["text"]);
        QChar c = letter[0];
        require(record.contains("nativeContourCount"),
                "Re-export QuranTextByWord with nativeContourCount from charProps");
        auto count = record["nativeContourCount"];
        J lower = record.value("nativeLowerContourCount", J());
        require(count.is_number_integer() && count.get<int>() >= 0, "Invalid native contour count");
        require(lower.is_null() || (lower.is_number_integer() && lower.get<int>() >= 0 &&
                                    lower.get<int>() <= count.get<int>()),
                "Invalid native lower-contour count");
        if (base(c)) {
            ++ordinal;
            owner = buckets.size();
            J bucket = {{"id", s(word, "id") + ":c" + std::to_string(offset)},
                        {"character", record["text"]},
                        {"ordinal", ordinal},
                        {"offset", offset},
                        {"text", record["text"]},
                        {"role", "letter"},
                        {"expectedContours", count.get<int>() - 1},
                        {"expectedLowerContours", lower},
                        {"expectedSlots", arr()},
                        {"characterOffsets", J::array({offset})}};
            if (c.unicode() == 0x640)
                bucket["carrier"] = true;
            buckets.push_back(bucket);
        } else if (marks.count(c.unicode()) || wordSigns.count(c.unicode())) {
            if (wordSigns.count(c.unicode()) ||
                (marks.count(c.unicode()) && marks.at(c.unicode()).rfind("waqf-", 0) == 0)) {
                ownerAt[offset] = buckets.size();
                buckets.push_back({{"id", s(word, "id") + ":sign" + std::to_string(offset)},
                                   {"character", record["text"]},
                                   {"ordinal", J()},
                                   {"offset", offset},
                                   {"text", record["text"]},
                                   {"role", "word-sign"},
                                   {"expectedContours", count},
                                   {"expectedLowerContours", lower},
                                   {"expectedSlots", arr()},
                                   {"characterOffsets", J::array({offset})}});
                ++offset;
                continue;
            }
            require(owner >= 0, "Transcript mark has no preceding base letter");
            auto &b = buckets[owner];
            b["text"] = s(b, "text") + s(record, "text");
            b["characterOffsets"].push_back(offset);
            b["expectedContours"] = n(b, "expectedContours") + count.get<int>();
            b["expectedLowerContours"] = b["expectedLowerContours"].is_null() || lower.is_null()
                                             ? J()
                                             : J(n(b, "expectedLowerContours") + lower.get<int>());
        } else
            require(count.get<int>() == 0 || c.isDigit(),
                    "Unsupported counted transcript character");
        ownerAt[offset] = owner;
        ++offset;
    }
    for (auto slot : expectedSlots(word)) {
        int at = n(slot, "characterOffset");
        require(ownerAt[at] >= 0, "Expected mark has no transcript owner");
        auto &owner = buckets[ownerAt[at]];
        bool equivalence =
            s(slot, "type") == "maddah" && at > 0 && text.mid(at - 1, 2) == QString::fromUtf8("أٓ");
        if (equivalence)
            slot["type"] = "fatha";
        slot["sourceCharacter"] = QString(text[at]).toStdString();
        slot["codePoint"] =
            QString("U+%1").arg(text[at].unicode(), 4, 16, QChar('0')).toUpper().toStdString();
        slot["equivalence"] = equivalence ? J("أٓ ≡ ـَٔا") : J();
        owner["expectedSlots"].push_back(slot);
    }
    for (auto &b : buckets) {
        int sum = 0;
        J inventory = obj();
        for (const auto &slot : b["expectedSlots"]) {
            sum += n(slot, "capacity");
            auto name = s(slot, "type");
            if (name == "small-kaf-body")
                name = "body-ink";
            inventory[name] = inventory.value(name, 0) + 1;
        }
        require(sum == n(b, "expectedContours"),
                s(b, "id") + ": inventory differs from native contour quota");
        b["expectedMarkCount"] = b["expectedSlots"].size();
        b["expectedInventory"] = inventory;
    }
    return buckets;
}
J selectBody(const J &paths, const J &owners, const J &nativeBody) {
    J result = {{"method", "single-alef-hamza-below-geometry-v1"},
                {"status", "not-applicable"},
                {"reason", "not-single-alef-with-hamza-below"},
                {"nativeBodyPathId", nativeBody},
                {"selectedBodyPathId", nativeBody}};
    J letters = arr();
    for (auto o : owners)
        if (s(o, "role") == "letter")
            letters.push_back(o);
    if (letters.size() != 1 ||
        (s(letters[0], "character") != "إ" && s(letters[0], "character") != "ا") ||
        letters[0]["expectedInventory"].value("hamza-below", 0) != 1)
        return result;
    result["status"] = "fallback";
    result["thresholds"] = {{"compactAspectMin", .5},
                            {"compactAspectMax", 1.5},
                            {"stemAspectMin", 3.},
                            {"stemToCompactHeightMin", 2.},
                            {"horizontalOffsetMaxWidths", 1.}};
    auto keep = [&](const char *why) {
        result["reason"] = why;
        return result;
    };
    J byId = index(paths);
    std::string id = str(nativeBody);
    if (!byId.contains(id))
        return keep("native-body-unavailable");
    int contours = 0;
    for (const auto &p : paths) {
        if (!p["contourCount"].is_number_integer() || n(p, "contourCount") < 1)
            return keep("invalid-contour-count");
        contours += n(p, "contourCount");
    }
    int quota = 1;
    for (const auto &o : owners)
        quota += n(o, "expectedContours");
    if (contours != quota)
        return keep("contour-totals-disagree");
    J boxes = obj();
    for (const auto &p : paths) {
        if (!p.contains("bbox") || !p["bbox"].is_array() || p["bbox"].size() != 4 ||
            !std::all_of(p["bbox"].begin(), p["bbox"].end(),
                         [](const J &v) { return v.is_number(); }))
            return keep("invalid-bounding-box");
        auto b = box(p);
        if (!std::all_of(b.begin(), b.end(), [](double v) { return std::isfinite(v); }) ||
            b[2] <= 0 || b[3] <= 0)
            return keep("invalid-bounding-box");
        boxes[s(p, "id")] = {{"width", b[2]},
                             {"height", b[3]},
                             {"aspect", b[3] / b[2]},
                             {"centerX", b[0] + b[2] / 2},
                             {"centerY", b[1] + b[3] / 2},
                             {"bottom", b[1] + b[3]}};
    }
    auto native = byId[id];
    if (n(native, "contourCount") != 1 || n(native, "legacyType") != 0)
        return keep("native-body-not-single-untyped-contour");
    auto compact = boxes[id];
    result["nativeGeometry"] = compact;
    if (d(compact, "aspect") < .5 || d(compact, "aspect") > 1.5) {
        result["status"] = "unchanged";
        return keep("native-body-not-compact");
    }
    J candidates = arr();
    for (const auto &p : paths) {
        if (s(p, "id") == id || n(p, "contourCount") != 1 || n(p, "legacyType") != 0)
            continue;
        auto stem = boxes[s(p, "id")];
        if (d(stem, "aspect") >= 3 && d(stem, "height") >= 2 * d(compact, "height") &&
            d(stem, "bottom") < d(compact, "centerY") &&
            std::abs(d(stem, "centerX") - d(compact, "centerX")) <=
                std::max(d(stem, "width"), d(compact, "width")))
            candidates.push_back(p["id"]);
    }
    std::sort(candidates.begin(), candidates.end());
    result["candidatePathIds"] = candidates;
    if (candidates.size() != 1)
        return keep(candidates.empty() ? "no-clear-upright-candidate"
                                       : "multiple-upright-candidates");
    result.update({{"status", "applied"},
                   {"reason", "unique-upright-alef-above-compact-hamza"},
                   {"selectedBodyPathId", candidates[0]},
                   {"selectedGeometry", boxes[str(candidates[0])]}});
    return result;
}
J consume(const J &paths, const J &buckets) {
    J ordered = paths;
    std::stable_sort(ordered.begin(), ordered.end(), [](const J &a, const J &b) {
        auto aa = box(a), bb = box(b);
        double ar = a.value("legacyType", 0) == 1 ? 0 : aa[0] + aa[2],
               br = b.value("legacyType", 0) == 1 ? 0 : bb[0] + bb[2];
        return ar != br ? ar > br : s(a, "id") < s(b, "id");
    });
    for (const auto &p : ordered)
        require(p["contourCount"].is_number_integer() && n(p, "contourCount") >= 1,
                "Native path contour count must be positive");
    J initial = pathIds(ordered), owners = arr(), swaps = arr();
    int cursor = 0;
    for (auto bucket : buckets) {
        int quota = n(bucket, "expectedContours"), used = 0, start = cursor;
        J selected = arr(), trace = arr(), errors = arr();
        require(quota >= 0, "Negative letter mark quota");
        while (used < quota && cursor < int(ordered.size())) {
            int step = cursor + 1;
            while (used + n(ordered[cursor], "contourCount") > quota &&
                   step < int(ordered.size())) {
                swaps.push_back({{"ownerId", bucket["id"]},
                                 {"at", cursor},
                                 {"withIndex", step},
                                 {"deferredPathId", ordered[cursor]["id"]},
                                 {"replacementPathId", ordered[step]["id"]},
                                 {"remainingQuota", quota - used}});
                std::swap(ordered[cursor], ordered[step]);
                ++step;
            }
            auto p = ordered[cursor];
            int count = n(p, "contourCount");
            used += count;
            selected.push_back(p["id"]);
            auto b = box(p);
            trace.push_back({{"pathId", p["id"]},
                             {"contours", count},
                             {"cumulativeContours", used},
                             {"rightEdge", b[0] + b[2]},
                             {"nativeWaqfLast", p.value("legacyType", 0) == 1}});
            ++cursor;
        }
        if (used > quota)
            errors.push_back("contour-overflow");
        if (used < quota)
            errors.push_back("contour-shortfall");
        bucket.update({{"pathIds", selected},
                       {"assignedContours", used},
                       {"startIndex", start},
                       {"endIndex", cursor},
                       {"trace", trace},
                       {"issues", errors}});
        owners.push_back(bucket);
    }
    J unused = arr();
    for (int i = cursor; i < int(ordered.size()); ++i)
        unused.push_back(ordered[i]["id"]);
    return {{"owners", owners},
            {"initialOrder", initial},
            {"consumedOrder", pathIds(ordered)},
            {"swaps", swaps},
            {"unusedPathIds", unused}};
}
static bool overlapHorizontally(const J &first, const J &second) {
    auto a = box(first), b = box(second);
    return std::max(a[0], b[0]) < std::min(a[0] + a[2], b[0] + b[2]);
}

// At the RTL boundary, the next ink can fill a slot that the current ink cannot.
// Only propose the exchange here. The complete vertical allocation must still pass.
static J preferAmbiguousAtBoundary(J &owners, const J &paths, const J &sides) {
    const J byId = index(paths);
    auto remainingOnSide = [&](const J &owner, const std::string &side) {
        int below = n(owner, "expectedLowerContours");
        int remaining = side == "below" ? below : n(owner, "expectedContours") - below;
        for (const auto &id : owner["pathIds"])
            if (sides.at(str(id)) == side)
                remaining -= n(byId.at(str(id)), "contourCount");
        return remaining;
    };
    J preferences = arr();
    for (size_t i = 1; i < owners.size(); ++i) {
        auto &earlier = owners[i - 1], &later = owners[i];
        auto &first = earlier["pathIds"], &next = later["pathIds"];
        if (first.empty() || next.empty() || s(earlier, "role") == "word-sign" ||
            s(later, "role") == "word-sign")
            continue;
        const J &clear = byId.at(str(first.back())), &unknown = byId.at(str(next.front()));
        auto side = s(sides, s(clear, "id"));
        int contours = n(clear, "contourCount");
        if (side == "ambiguous" || sides.at(s(unknown, "id")) != "ambiguous" ||
            clear.value("legacyType", 0) == 1 || unknown.value("legacyType", 0) == 1 ||
            contours != n(unknown, "contourCount") || !overlapHorizontally(clear, unknown))
            continue;
        if (remainingOnSide(earlier, side) >= 0 || remainingOnSide(later, side) < contours)
            continue;
        preferences.push_back({{"ownerId", earlier["id"]}, {"nextOwnerId", later["id"]},
                               {"preferredPathId", unknown["id"]}, {"deferredPathId", clear["id"]},
                               {"deferredSide", side}});
        std::swap(first.back(), next.front());
    }
    return preferences;
}

static J reserveAmbiguous(const J &paths, const J &owners, const J &sides) {
    J byId = index(paths);
    std::map<std::string, int> observed;
    for (const auto &p : paths)
        observed[str(sides[s(p, "id")])] += n(p, "contourCount");
    int low = 0, upper = 0;
    for (const auto &b : owners) {
        low += n(b, "expectedLowerContours");
        upper += n(b, "expectedContours") - n(b, "expectedLowerContours");
    }
    int needed = low - observed["below"];
    J detail = {
        {"status", "fallback"},
        {"observedClearContours", {{"above", observed["above"]}, {"below", observed["below"]}}}};
    if (needed < 0 || upper - observed["above"] < 0)
        return merged(detail, {{"reason", "clear-contours-exceed-side-quotas"}});
    J locked = arr();
    for (const auto &b : owners) {
        J list = arr();
        for (const auto &id : b["pathIds"])
            if (sides[str(id)] == "ambiguous")
                list.push_back(id);
        if (list.empty())
            continue;
        std::set<int> possible{0};
        int total = 0;
        for (const auto &id : list) {
            int count = n(byId[str(id)], "contourCount");
            total += count;
            auto next = possible;
            for (int n : possible)
                next.insert(n + count);
            possible = next;
        }
        J options = arr();
        for (int n : possible)
            if (n <= NativeMarks::n(b, "expectedLowerContours") &&
                total - n <= NativeMarks::n(b, "expectedContours") -
                                 NativeMarks::n(b, "expectedLowerContours"))
                options.push_back(n);
        locked.push_back({{"ownerId", b["id"]},
                          {"pathIds", list},
                          {"contours", total},
                          {"possibleBelowReservations", options}});
    }
    detail["owners"] = locked;
    std::map<int, std::vector<std::vector<int>>> states{{0, {{}}}};
    for (const auto &owner : locked) {
        decltype(states) following;
        for (const auto &[used, witnesses] : states)
            for (const auto &value : owner["possibleBelowReservations"]) {
                int n = value;
                if (used + n > needed)
                    continue;
                auto &options = following[used + n];
                for (auto witness : witnesses)
                    if (options.size() < 2) {
                        witness.push_back(n);
                        options.push_back(witness);
                    }
            }
        states = following;
    }
    const auto solutions = states[needed];
    if (solutions.size() != 1)
        return merged(detail,
                      {{"reason", solutions.empty() ? "ambiguous-contours-cannot-fill-side-quotas"
                                                    : "ambiguous-side-reservations-not-unique"}});
    for (size_t i = 0; i < locked.size(); ++i) {
        locked[i]["reservedBelowContours"] = solutions[0][i];
        locked[i]["reservedAboveContours"] = n(locked[i], "contours") - solutions[0][i];
    }
    detail.update(
        {{"owners", locked}, {"status", "reserved"}, {"reason", "unique-owner-side-reservations"}});
    return detail;
}
static J fillVerticalQuotas(const J &paths, const J &original, const J &sides,
                            const J &reservation, J detail);

static std::string verticalSide(const J &path, double baseline, double band) {
    auto b = box(path);
    if (b[1] + b[3] < baseline - band)
        return "above";
    return b[1] > baseline + band ? "below" : "ambiguous";
}

J consumeVertical(const J &paths, const J &buckets, J baseline, J bodyBox, double scale) {
    J original = consume(paths, buckets), detail = {{"method", "native-lower-contour-quota"},
                                                    {"status", "fallback"},
                                                    {"baseline", baseline},
                                                    {"baselineBandHalfHeight", .5 * scale},
                                                    {"changedOwnerIds", arr()}};
    auto keep = [&](const char *why) {
        detail["reason"] = why;
        original["verticalAssignment"] = detail;
        return original;
    };
    int lower = 0, upper = 0, active = 0;
    for (const auto &b : buckets) {
        if (b.value("expectedLowerContours", J()).is_null())
            return keep("native-lower-quota-unavailable");
        int lo = n(b, "expectedLowerContours"), all = n(b, "expectedContours");
        require(lo >= 0 && lo <= all, "Lower quota exceeds total mark quota");
        lower += lo;
        upper += all - lo;
        active += all > 0;
    }
    detail["expectedContours"] = {{"above", upper}, {"below", lower}};
    if (active < 2 || !lower || !upper) {
        detail["status"] = "unchanged";
        return keep("no-two-sided-owner-boundary");
    }
    if (issues(original))
        return keep("original-contour-quota-difference");
    if (baseline.is_null() || !std::isfinite(baseline.get<double>()) || scale <= 0)
        return keep("baseline-unavailable");
    double base = baseline, band = .5 * scale;
    if (bodyBox.is_null() || bodyBox[1].get<double>() > base + band ||
        bodyBox[1].get<double>() + bodyBox[3].get<double>() < base - band)
        return keep("body-does-not-intersect-native-baseline-band");
    J sides = obj();
    bool ambiguous = false;
    for (const auto &p : paths) {
        auto side = verticalSide(p, base, band);
        sides[s(p, "id")] = side;
        ambiguous |= side == "ambiguous";
    }
    detail["pathSides"] = sides;
    if (!ambiguous)
        return fillVerticalQuotas(paths, original, sides, J(), std::move(detail));

    J proposedOwners = original["owners"];
    J preferences = preferAmbiguousAtBoundary(proposedOwners, paths, sides);
    if (!preferences.empty()) {
        auto proposed = fillVerticalQuotas(paths, original, sides,
            reserveAmbiguous(paths, proposedOwners, sides),
            merged(detail, {{"boundaryPreferences", preferences}}));
        if (proposed["verticalAssignment"]["status"] == "applied") {
            const J owners = index(proposed["owners"]);
            // An earlier deficit in the clear stream could claim the deferred path.
            // Commit only when each proposed exchange actually reaches its neighbor.
            const bool delivered = std::all_of(preferences.begin(), preferences.end(),
                [&](const J &choice) {
                    const auto &received = owners.at(s(choice, "nextOwnerId")).at("pathIds");
                    return std::find(received.begin(), received.end(), choice["deferredPathId"])
                           != received.end();
                });
            if (delivered)
                return proposed;
        }
    }
    return fillVerticalQuotas(paths, original, sides,
                              reserveAmbiguous(paths, original["owners"], sides), std::move(detail));
}

static J fillVerticalQuotas(const J &paths, const J &original, const J &sides,
                            const J &reservation, J detail) {
    const auto &buckets = original["owners"];
    auto keep = [&](const char *why) {
        detail["reason"] = why;
        return merged(original, {{"verticalAssignment", detail}});
    };
    J reserved = obj();
    if (!reservation.is_null()) {
        detail["ambiguousReservation"] = reservation;
        if (s(reservation, "status") != "reserved")
            return keep("ink-intersects-native-baseline-band");
        reserved = index(reservation["owners"], "ownerId");
    }
    std::map<std::string, int> observed;
    for (const auto &p : paths)
        observed[str(sides[s(p, "id")])] += n(p, "contourCount");
    detail["observedContours"] = {{"above", observed["above"]}, {"below", observed["below"]}};
    int reservedUpper = 0, reservedLower = 0;
    for (const auto &o : reserved) {
        reservedUpper += n(o, "reservedAboveContours");
        reservedLower += n(o, "reservedBelowContours");
    }
    if (observed["above"] + reservedUpper != n(detail["expectedContours"], "above") ||
        observed["below"] + reservedLower != n(detail["expectedContours"], "below"))
        return keep("vertical-contour-totals-disagree");
    J streams = obj();
    for (std::string side : {"above", "below"}) {
        J quotas = arr(), ink = arr();
        for (auto b : buckets) {
            auto r = reserved.value(s(b, "id"), obj());
            b["expectedContours"] =
                (side == "below" ? n(b, "expectedLowerContours")
                                 : n(b, "expectedContours") - n(b, "expectedLowerContours")) -
                r.value(side == "below" ? "reservedBelowContours" : "reservedAboveContours", 0);
            quotas.push_back(b);
        }
        for (const auto &p : paths)
            if (sides[s(p, "id")] == side)
                ink.push_back(p);
        streams[side] = consume(ink, quotas);
        if (issues(streams[side]))
            return keep("vertical-stream-contour-overflow-or-shortfall");
    }
    J pathMap = index(paths), owners = arr(), consumed = arr(), changes = arr();
    std::map<std::string, int> rank;
    int at = 0;
    for (const auto &id : original["initialOrder"])
        rank[str(id)] = at++;
    for (size_t i = 0; i < buckets.size(); ++i) {
        auto bucket = buckets[i], previous = original["owners"][i];
        J list = streams["above"]["owners"][i]["pathIds"];
        append(list, streams["below"]["owners"][i]["pathIds"]);
        append(list, reserved.value(s(bucket, "id"), obj()).value("pathIds", arr()));
        std::stable_sort(list.begin(), list.end(),
                         [&](const J &a, const J &b) { return rank[str(a)] < rank[str(b)]; });
        if (ids(list) == ids(previous["pathIds"]))
            list = previous["pathIds"];
        else
            changes.push_back(
                {{"ownerId", bucket["id"]}, {"before", previous["pathIds"]}, {"after", list}});
        J trace = arr();
        int count = 0;
        for (const auto &id : list) {
            auto p = pathMap[str(id)];
            count += n(p, "contourCount");
            auto b = box(p);
            trace.push_back({{"pathId", id},
                             {"contours", p["contourCount"]},
                             {"cumulativeContours", count},
                             {"rightEdge", b[0] + b[2]},
                             {"nativeWaqfLast", p.value("legacyType", 0) == 1},
                             {"geometricSide", sides[str(id)]}});
        }
        bucket.update({{"pathIds", list},
                       {"assignedContours", count},
                       {"startIndex", consumed.size()},
                       {"endIndex", consumed.size() + list.size()},
                       {"trace", trace},
                       {"issues", arr()}});
        owners.push_back(bucket);
        append(consumed, list);
    }
    if (!reserved.empty() && !changes.empty()) {
        auto overlap = [&](const std::string &p, const std::string &q) {
            return overlapHorizontally(pathMap.at(p), pathMap.at(q));
        };
        J nonlocal = arr();
        for (const auto &c : changes) {
            auto before = ids(c["before"]), after = ids(c["after"]);
            std::vector<std::string> removed, added;
            std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                                std::back_inserter(removed));
            std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                                std::back_inserter(added));
            bool okay = true;
            for (const auto &p : removed)
                okay &= std::any_of(added.begin(), added.end(),
                                    [&](const auto &q) { return overlap(p, q); });
            for (const auto &p : added)
                okay &= std::any_of(removed.begin(), removed.end(),
                                    [&](const auto &q) { return overlap(p, q); });
            if (!okay)
                nonlocal.push_back({{"ownerId", c["ownerId"]},
                                    {"removedPathIds", removed},
                                    {"addedPathIds", added}});
        }
        detail["localExchangeCheck"] = {{"method", "bidirectional-horizontal-overlap"},
                                        {"status", nonlocal.empty() ? "passed" : "fallback"},
                                        {"rejectedChanges", nonlocal}};
        if (!nonlocal.empty())
            return keep("partial-vertical-exchange-not-local");
    }
    if (changes.empty()) {
        detail["status"] = "unchanged";
        return keep("vertical-order-agrees-with-original");
    }
    J changed = arr(), summary = obj(), swaps = arr();
    for (const auto &c : changes)
        changed.push_back(c["ownerId"]);
    for (std::string side : {"above", "below"}) {
        summary[side] = subset(streams[side], {"initialOrder", "consumedOrder", "swaps"});
        for (auto swap : streams[side]["swaps"]) {
            swap["verticalSide"] = side;
            swaps.push_back(swap);
        }
    }
    const char *reason = reserved.empty() ? "clear-vertical-order-with-complete-native-quotas"
                                          : "clear-vertical-order-with-locked-ambiguous-paths";
    if (detail.contains("boundaryPreferences"))
        reason = "ambiguous-before-incompatible-side";
    detail.update(
        {{"status", "applied"},
         {"reason", reason},
         {"changedOwnerIds", changed},
         {"changes", changes},
         {"streams", summary}});
    return {{"owners", owners},          {"initialOrder", original["initialOrder"]},
            {"consumedOrder", consumed}, {"swaps", swaps},
            {"unusedPathIds", arr()},    {"verticalAssignment", detail}};
}

static J normalizeDetached(const J &subwords) {
    J result = arr();
    for (auto sub : subwords) {
        auto text = qstr(sub["text"]);
        bool detached = !text.isEmpty();
        for (auto c : text)
            detached &= marks.count(c.unicode()) || wordSigns.count(c.unicode());
        bool previousBody = false;
        if (!result.empty())
            for (auto c : qstr(result.back()["text"]))
                previousBody |= base(c);
        if (detached && previousBody) {
            auto &parent = result.back();
            if (!parent.contains("nativeSubwordIds"))
                parent["nativeSubwordIds"] = J::array({parent["id"]});
            parent["nativeSubwordIds"].push_back(sub["id"]);
            parent["text"] = s(parent, "text") + s(sub, "text");
            append(parent["pathIds"], sub["pathIds"]);
        } else
            result.push_back(sub);
    }
    return result;
}
static void validateDecomposition(const J &original, const J &parts, const J &inkPaths) {
    auto unionRuns = maskUnion(parts);
    int totalPixels = 0;
    std::set<std::string> seen;
    for (const auto &p : parts)
        totalPixels += pixels(p["runs"]);
    require(unionRuns == maskUnion(J::array({original})) && totalPixels == pixels(unionRuns) &&
                totalContours(parts) == n(original, "contourCount"),
            "Decomposition did not conserve disjoint source ink and contours");
    for (const auto &p : parts)
        require(!inkPaths.contains(s(p, "id")) && seen.insert(s(p, "id")).second,
                "Duplicate assignment component ID");
}

// Keep the exact vowel masks already identified by the native geometric grouper.
// A real tanwin or an unsplittable shape cannot supply two individual vowels.
static J separateVowelGroups(const J &owner, const J &paths, const std::string &packedId,
                             int width, double baseline, const J &geometry, const J &bank) {
    auto partition = geometricPartition(owner, paths, width, baseline, geometry, bank);
    J parts = arr();
    for (const auto &group : partition["groups"]) {
        if (group.size() != 1 || s(group.front(), "sourcePathId") != packedId)
            continue;
        const auto &part = group.front();
        if (part.value("stroke", J()).is_array() || part.at("templateScores").contains("damma"))
            parts.push_back(part);
    }
    return parts;
}

static J dotTemplateMatches(const J &paths, const J &geometry, double baseline, double scale,
                            bool single, bool pair, const J &bank, int width) {
    J dots = obj();
    for (const auto &p : paths) {
        auto shape = geometry.find(s(p, "id"));
        auto b = box(p);
        if (shape == geometry.end() || !shape->contains("templateScores") || n(p, "contourCount") != 1 || b[3] <= 0)
            continue;
        // Same area threshold and aspect limits as the native subword single-dot matcher.
        double distance = (*shape)["templateScores"].value("onedot", 1.);
        if (single && b[2] > .8 * b[3] && b[2] < 1.2 * b[3] && distance < constants::MATCHING_ERROR_RATIO)
            dots[s(p, "id")] = {{"distance", distance}, {"side", verticalSide(p, baseline, .5 * scale)}};
        else if (pair && b[2] > 1.2 * b[3]) {
            distance = (*shape)["templateScores"].value("twodots", 1.);
            if (distance >= constants::MATCHING_ERROR_RATIO) {
                const auto evidence = doubleDotEvidence(merged(p, subset(*shape, {"signature"})), bank, width);
                if (evidence.is_null()) continue;
                distance = d(evidence, "distance");
            }
            dots[s(p, "id")] = {{"type", "double-dot"}, {"distance", distance},
                                {"side", verticalSide(p, baseline, .5 * scale)}};
        }
    }
    return dots;
}

static bool dotAndVowelSlotsMatch(const J &owners, J &dots, const J &vowels) {
    std::set<std::string> vowelOwners;
    for (const auto &owner : owners) {
        std::multiset<std::pair<std::string, std::string>> requiredSides, observedSides;
        for (const auto &slot : owner["expectedSlots"])
            if (slot["type"] == "single-dot" || slot["type"] == "double-dot")
                requiredSides.emplace(s(slot, "type"), s(slot, "side"));
        for (const auto &id : owner["pathIds"]) {
            auto dot = dots.find(str(id));
            if (dot != dots.end()) {
                observedSides.emplace(dot->value("type", std::string("single-dot")), s(*dot, "side"));
                (*dot)["ownerId"] = owner["id"];
            }
            if (vowels.contains(str(id))) {
                if (!owner["expectedInventory"].contains(str(vowels.at(str(id)))))
                    return false;
                vowelOwners.insert(s(owner, "id"));
            }
        }
        if (requiredSides != observedSides)
            return false;
    }
    return vowelOwners.size() == 2;
}

static void retryPackedVowels(J &paths, J &result, const J &expected,
                                           const J &baseline, const J &bodyBox, double scale,
                                           const J &geometry, const J &bank, int width) {
    if (expected.size() < 2 || !baseline.is_number() || !bodyBox.is_array() || scale <= 0)
        return;
    const J *packed = nullptr;
    for (const auto &p : paths) {
        if (p.contains("sourcePathId") || p.value("legacyType", 0) == 1)
            return;
        if (n(p, "contourCount") == 1)
            continue;
        if (packed || n(p, "contourCount") != 2)
            return;
        packed = &p;
    }
    if (!packed)
        return;
    int singles = 0, pairs = 0;
    for (const auto &owner : expected) {
        // Multipart slots such as tanwin and triple dots have their own groupers.
        if (s(owner, "role") != "letter" || n(owner, "expectedContours") != n(owner, "expectedMarkCount"))
            return;
        singles += owner["expectedInventory"].value("single-dot", 0);
        pairs += owner["expectedInventory"].value("double-dot", 0);
    }
    if (!singles && !pairs)
        return;
    J dots = dotTemplateMatches(paths, geometry, baseline, scale, singles > 0, pairs > 0, bank, width);
    if (int(dots.size()) != singles + pairs)
        return;

    const J *packedOwner = nullptr;
    const auto packedId = s(*packed, "id");
    bool misplaced = issues(result);
    for (const auto &owner : result["owners"]) {
        int observedSingle = 0, observedPair = 0;
        for (const auto &id : owner["pathIds"]) {
            auto dot = dots.find(str(id));
            if (dot != dots.end()) {
                (*dot)["previousOwnerId"] = owner["id"];
                if (dot->contains("type")) ++observedPair;
                else ++observedSingle;
            }
            if (id == packedId)
                packedOwner = &owner;
        }
        misplaced |= observedSingle != owner["expectedInventory"].value("single-dot", 0) ||
                     observedPair != owner["expectedInventory"].value("double-dot", 0);
    }
    if (!misplaced || !packedOwner)
        return;
    auto parts = separateVowelGroups(*packedOwner, index(paths), packedId, width, baseline, geometry, bank);
    if (parts.size() != 2)
        return;
    J trial = arr(), vowels = obj();
    for (const auto &part : parts) {
        auto side = verticalSide(part, baseline, .5 * scale);
        bool damma = part["templateScores"].contains("damma");
        if (side == "ambiguous" || (damma && side != "above"))
            return;
        vowels[s(part, "id")] = damma ? "damma" : side == "above" ? "fatha" : "kasra";
    }
    for (const auto &p : paths) {
        if (p["id"] == packedId)
            append(trial, parts);
        else
            trial.push_back(p);
    }
    auto proposed = consumeVertical(trial, expected, baseline, bodyBox, scale);
    if (issues(proposed) || !dotAndVowelSlotsMatch(proposed["owners"], dots, vowels))
        return;
    proposed[pairs ? "dotTemplate" : "singleDotTemplate"] = {
        {"name", pairs ? (singles ? "onedot+twodots" : "twodots") : "onedot"}, {"matches", dots},
        {"packedPathId", packedId}, {"vowelParts", vowels}};
    paths = std::move(trial);
    result = std::move(proposed);
}

// Only distinct, well-matched native outlines can overturn the RTL order.
// Fatha and kasra share an outline; a stroke crossing the baseline stays unknown.
static std::string boundaryMarkType(const J &path, const J &shape, const J &bank, double baseline) {
    const auto [x, y, w, h] = box(path);
    if (n(path, "contourCount") != 1 || path.value("legacyType", 0) != 0 || h <= 0 ||
        !shape.contains("signature") || (y < baseline && y + h > baseline)) return {};
    const bool above = y + h <= baseline;
    if (above && shape.value("templateScores", obj()).value("damma", 1.) < .1) return "damma";
    const auto matches = [&](const char *type) {
        auto prototypes = bank.find(type);
        if (prototypes == bank.end()) return false;
        const J outline = {{"bbox", path["bbox"]}, {"signature", shape["signature"]}};
        return std::any_of(prototypes->begin(), prototypes->end(),
            [&](const J &prototype) { return shapeDistance(outline, prototype) < .08; });
    };
    if (above && w < .6 * h && matches("small-alef")) return "small-alef";
    if (above && w < 1.7 * h && matches("sukun")) return "sukun";
    if (w > 1.5 * h && (matches("fatha") || matches("kasra"))) return above ? "fatha" : "kasra";
    return {};
}

static void exchangeBoundaryPaths(J &result, J &a, size_t j, J &b, size_t k, const J &types) {
    auto &first = a["pathIds"][j], &second = b["pathIds"][k];
    result["templateBoundarySwaps"].push_back({
        {"ownerIds", {a["id"], b["id"]}}, {"before", {first, second}}, {"nativeTypes", types}});
    first.swap(second);
    auto &u = a["trace"][j], &v = b["trace"][k];
    u["cumulativeContours"].swap(v["cumulativeContours"]);
    u.swap(v);
    result["consumedOrder"][n(a, "startIndex") + j].swap(result["consumedOrder"][n(b, "startIndex") + k]);
}

static std::string localDotSide(const J &dot, const J &body, int width, double band) {
    const auto [x,y,w,h] = box(dot);
    int top = INT_MAX, bottom = -1;
    for (size_t i = 0; i < body.size(); i += 2) {
        int start = body[i], count = body[i+1];
        while (count > 0) {
            const int col = start % width, len = std::min(count, width - col);
            if (col < x+w && col+len > x) {
                top = std::min(top, start / width);
                bottom = std::max(bottom, start / width);
            }
            start += len;
            count -= len;
        }
    }
    if (bottom < 0) return {};
    if (y+h < top-band) return "above";
    if (y > bottom+1+band) return "below";
    return {};
}

// A dot can cross the line baseline while lying entirely above or below its local body.
static void resolveCrossedDots(J &result, J &a, J &b, const J &paths, const J &geometry,
                               const J &body, int width, double band) {
    const auto &vertical = result["verticalAssignment"];
    if (vertical.value("reason", "") != "ink-intersects-native-baseline-band" || body.empty()) return;
    auto expectedSide = [](const J &owner) -> std::string {
        if (owner["expectedInventory"].value("single-dot", 0) == 1)
            for (const auto &slot : owner["expectedSlots"])
                if (slot["type"] == "single-dot") return s(slot, "side");
        return {};
    };
    const auto as = expectedSide(a), bs = expectedSide(b);
    if (as.empty() || bs.empty() || as == bs) return;
    const auto findDot = [&](const J &owner) {
        int found = -1;
        for (size_t i = 0; i < owner["pathIds"].size(); ++i) {
            const auto id = str(owner["pathIds"][i]);
            auto shape = geometry.find(id);
            if (shape == geometry.end() || shape->value("templateScores", obj()).value("onedot", 1.) >= .1) continue;
            if (n(paths.at(id), "contourCount") != 1 || found >= 0) return -1;
            found = int(i);
        }
        return found;
    };
    const int j = findDot(a), k = findDot(b);
    if (j < 0 || k < 0) return;
    const auto aid = str(a["pathIds"][j]), bid = str(b["pathIds"][k]);
    const auto &sides = vertical["pathSides"];
    if (!((sides.at(aid) == "ambiguous" && sides.at(bid) == as) ||
          (sides.at(bid) == "ambiguous" && sides.at(aid) == bs))) return;
    const auto &p = paths.at(aid), &q = paths.at(bid);
    const auto l = box(p), r = box(q);
    const double overlap = std::min(l[0]+l[2],r[0]+r[2]) - std::max(l[0],r[0]);
    if (overlap <= .5 * std::min(l[2], r[2]) || localDotSide(p, body, width, band) != bs ||
        localDotSide(q, body, width, band) != as) return;
    exchangeBoundaryPaths(result, a, j, b, k, {"single-dot-" + bs, "single-dot-" + as});
}

static void resolveTemplateBoundaries(J &result, const J &paths, const J &geometry, const J &bank,
                                      const J &baseline, const J &body, int width, double scale) {
    if (!baseline.is_number() || bank.empty() || result["owners"].size() < 2 || issues(result)) return;
    const auto byId = index(paths);
    std::map<std::string, std::string> labels;
    const auto label = [&](const J &id) -> const std::string& {
        const auto key = str(id);
        auto [entry, fresh] = labels.try_emplace(key);
        if (fresh && geometry.contains(key))
            entry->second = boundaryMarkType(byId.at(key), geometry.at(key), bank, baseline);
        return entry->second;
    };
    const auto distinguishable = [](const J &own, const J &other) {
        for (auto name : {"damma","sukun","small-alef","fatha","kasra"})
            if (own.contains(name) && !other.contains(name)) return true;
        return false;
    };
    auto &owners = result["owners"];
    for (size_t i = 1; i < owners.size(); ++i) {
        auto &a = owners[i - 1], &b = owners[i];
        const auto &first = a["expectedInventory"], &second = b["expectedInventory"];
        if (a["role"] != "letter" || b["role"] != "letter") continue;
        resolveCrossedDots(result, a, b, byId, geometry, body, width, .5 * scale);
        if (!distinguishable(first, second) || !distinguishable(second, first)) continue;
        auto &aa = a["pathIds"], &bb = b["pathIds"];
        std::vector<std::pair<size_t, size_t>> choices;
        for (size_t j = 0; j < aa.size(); ++j) for (size_t k = 0; k < bb.size(); ++k) {
            const auto l = box(byId.at(str(aa[j]))), r = box(byId.at(str(bb[k])));
            const double overlap = std::min(l[0]+l[2],r[0]+r[2]) - std::max(l[0],r[0]);
            if (overlap <= .5 * std::min(l[2],r[2]) || (l[1] < r[1]+r[3] && r[1] < l[1]+l[3])) continue;
            const auto &left = label(aa[j]), &right = label(bb[k]);
            if (first.contains(left) || !second.contains(left) || second.contains(right) || !first.contains(right))
                continue;
            const auto needsOne = [&](const J &ids, const J &inventory, const std::string &mark) {
                return 1 + std::count_if(ids.begin(), ids.end(), [&](const J &id) { return label(id) == mark; })
                       == inventory.at(mark).get<int>();
            };
            if (needsOne(aa, first, right) && needsOne(bb, second, left)) choices.emplace_back(j, k);
        }
        if (choices.size() != 1) continue;
        const auto [j,k] = choices.front();
        exchangeBoundaryPaths(result, a, j, b, k, {label(aa[j]), label(bb[k])});
    }
}

static QRegion sourceRegion(const J &runs, int width) {
    QRegion region;
    for (size_t i = 0; i < runs.size(); i += 2) {
        int start = runs[i], remaining = runs[i + 1];
        while (remaining) {
            const int x = start % width, length = std::min(remaining, width - x);
            region += QRect(x, start / width, length, 1);
            start += length;
            remaining -= length;
        }
    }
    return region;
}

static J compoundBodyMask(ShapeException exception, const QString &text, const J &members, const J &owners,
                          double baseline, double scale, int width) {
    int quota = 0;
    for (const auto &owner : owners) quota += n(owner, "expectedContours");
    std::vector<BodyContourInput> contours;
    for (const auto &member : members) {
        const auto [x,y,w,h] = box(member);
        contours.push_back({{x,y,w,h}, n(member, "contourCount"), n(member, "legacyType") == 0});
    }
    const auto region = [&](int i) { return sourceRegion(members[i]["runs"], width); };
    const int candidate = PageAnalysisResult::secondaryBodyContour(exception, text, quota, baseline, scale, contours, region);
    if (candidate < 0) return {};
    const bool counter = exception == ShapeException::InnerContourCollidingProblem;
    const auto body = counter ? region(0).subtracted(region(candidate)) : region(0).united(region(candidate));
    J spans = arr();
    for (const auto &rect : body)
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            spans.push_back(y * width + rect.left());
            spans.push_back(rect.width());
        }
    const auto runs = maskUnion(J::array({J{{"runs", spans}}}));
    const auto &primary = members[0]["id"], &secondary = members[candidate]["id"];
    return {{"primaryPathId", primary}, {"secondaryPathId", secondary}, {"pathIds", {primary, secondary}},
            {"operation", counter ? "subtract" : "add"}, {"bbox", inkBox(runs, width)}, {"runs", runs}};
}

J assignPage(const J &doc, const J &transcript, const J &sourceGeometry, const J &bank) {
    require(doc["pageId"] == transcript["pageId"],
            "Native segmentation and transcript pages differ");
    std::string pid = s(doc, "pageId");
    int width = n(doc["raster"], "width"), height = n(doc["raster"], "height");
    require(width > 0 && height > 0, "Invalid raster dimensions");
    J paths = obj(), sourcePaths = arr();
    std::vector<unsigned char> pageInk(size_t(width) * height, 0);
    for (const auto &p : doc["paths"]) {
        auto id = s(p, "id");
        require(!paths.contains(id), "Duplicate native path IDs");
        auto path = subset(p, {"id", "bbox", "runs", "contourCount", "legacyType"});
        const auto &runs = path["runs"];
        require(runs.size() % 2 == 0, "Odd RLE");
        int previous = -1;
        for (size_t i = 0; i < runs.size(); i += 2) {
            int start = runs[i], length = runs[i + 1];
            require(start >= 0 && start >= previous && length > 0 &&
                        size_t(start) + length <= pageInk.size(),
                    "Invalid native RLE");
            previous = start + length;
        }
        paths[id] = path;
        sourcePaths.push_back(path);
    }
    J geometry = sourceGeometry.empty() ? index(doc["paths"]) : sourceGeometry,
      nativeWords = index(transcript["words"]), words = arr(), subs = arr(), ignored = arr(),
      inkPaths = paths, components = arr(), allGroups = arr();
    std::map<std::string, int> ownership;
    std::set<std::string> decomposedParents;
    std::map<std::string, J> bodyPaint;
    std::map<int, int> wordsPerLine;
    for (const auto &word : transcript["words"])
        ++wordsPerLine[word.value("line", 0)];
    const int pageNumber = qstr(doc["pageId"]).mid(4).toInt();
    const double scale = doc["provenance"].contains("pdfToRaster")
                           ? doc["provenance"]["pdfToRaster"][3].get<double>()
                           : d(doc["raster"], "scale");
    for (const auto &sourceWord : doc["words"]) {
        std::string wid = pid + ":" + s(sourceWord, "id");
        const auto &word = nativeWords.at(wid);
        const int line = n(sourceWord, "line");
        const auto exception = PageAnalysisResult::wordShapeException(pageNumber, line,
            sourceWord.value("index", word.value("ordinal", 0)), wordsPerLine[line]);
        std::string joined;
        for (const auto &s : sourceWord["subwords"])
            joined += NativeMarks::s(s, "text");
        require(sourceWord["text"] == word["text"] && joined == s(word, "text"),
                wid + ": native subword text does not align with QuranTextByWord");
        J buckets = expectedLetters(word);
        words.push_back({{"id", wid},
                         {"text", word["text"]},
                         {"line", sourceWord["line"]},
                         {"pathIds", sourceWord["pathIds"]}});
        int offset = 0;
        for (const auto &sourceSub : normalizeDetached(sourceWord["subwords"])) {
            std::string sid = pid + ":" + s(sourceSub, "id");
            const auto &list = sourceSub["pathIds"];
            int end = offset + qstr(sourceSub["text"]).size();
            J expected = arr(), members = arr();
            for (const auto &id : list) {
                ++ownership[str(id)];
                members.push_back(paths.at(str(id)));
            }
            for (const auto &b : buckets)
                if (offset <= n(b, "offset") && n(b, "offset") < end) {
                    for (const auto &ci : b["characterOffsets"])
                        require(ci.get<int>() < end,
                                sid + ": unsupported letter/mark span across native subwords");
                    expected.push_back(b);
                }
            if (expected.empty()) {
                ignored.push_back({{"id", sid},
                                   {"text", sourceSub["text"]},
                                   {"pathIds", list},
                                   {"reason", "No letter or mark quota"}});
                offset = end;
                continue;
            }
            bool letters = false;
            for (const auto &b : expected)
                letters |= s(b, "role") == "letter";
            J nativeBody = letters && !list.empty() ? list[0] : J();
            auto selection = selectBody(members, expected, nativeBody);
            J body = selection["selectedBodyPathId"], markPaths = arr();
            J compound;
            if (exception.subWordPosition > 0 && exception.subWordPosition == qstr(sourceSub["id"]).section("-s", -1).toInt()
                    && body == nativeBody && !body.is_null() && sourceWord["baseline"].is_number())
                compound = compoundBodyMask(exception.shapeException, qstr(sourceSub["text"]), members, expected,
                                            sourceWord["baseline"], scale, width);
            for (const auto &p : members)
                if (p["id"] != body && (compound.empty() || p["id"] != compound["secondaryPathId"]))
                    markPaths.push_back(p);
            markPaths = decomposeDotPairs(markPaths, width, bank);
            J bodyBox = body.is_null() ? J() : paths.at(str(body))["bbox"];
            if (!compound.empty()) {
                bodyBox = compound["bbox"];
                bodyPaint[str(body)] = compound["runs"];
                bodyPaint[s(compound, "secondaryPathId")] = arr();
            }
            J result = consumeVertical(markPaths, expected, sourceWord["baseline"], bodyBox, scale);
            retryPackedVowels(markPaths, result, expected, sourceWord["baseline"],
                                           bodyBox, scale, geometry, bank, width);
            J decomposition = arr();
            std::vector<std::string> parents;
            for (const auto &p : markPaths)
                if (p.contains("sourcePathId") && std::find(parents.begin(), parents.end(),
                                                            s(p, "sourcePathId")) == parents.end())
                    parents.push_back(s(p, "sourcePathId"));
            for (const auto &parent : parents) {
                J parts = arr();
                for (const auto &p : markPaths)
                    if (s(p, "sourcePathId") == parent)
                        parts.push_back(p);
                auto original = paths.at(parent);
                validateDecomposition(original, parts, inkPaths);
                J componentPixels = arr();
                for (const auto &part : parts) {
                    require(!inkPaths.contains(s(part, "id")), "Duplicate assignment component ID");
                    inkPaths[s(part, "id")] = part;
                    components.push_back(part);
                    componentPixels.push_back(pixels(part["runs"]));
                }
                decomposedParents.insert(parent);
                decomposition.push_back({{"sourcePathId", parent},
                                         {"componentIds", pathIds(parts)},
                                         {"sourceInkPixels", pixels(original["runs"])},
                                         {"componentInkPixels", componentPixels}});
            }
            result = reserveWaqf(markPaths, expected, result, sourceWord["baseline"], bodyBox,
                                 scale, geometry, width);
            if (!body.is_null())
                resolveTemplateBoundaries(result, markPaths, geometry, bank, sourceWord["baseline"],
                                          compound.empty() ? paths.at(str(body))["runs"] : compound["runs"], width, scale);
            auto conflicts = boundaryConflicts(result["owners"], markPaths, width,
                                               sourceWord["baseline"], geometry, bank);
            std::string precedingLetter;
            for (auto &owner : result["owners"]) {
                auto grouped = groupOwnedInk(owner, inkPaths, width, sourceWord["baseline"],
                                             geometry, bank, conflicts.value(s(owner, "id"), J()));
                linkTranscriptCharacters(grouped["groups"], owner, wid, inkPaths, geometry, bank, width);
                if (s(owner, "role") == "letter") {
                    precedingLetter = s(owner, "id");
                } else if (!precedingLetter.empty() && s(owner, "role") == "word-sign") {
                    for (auto &group : grouped["groups"]) {
                        auto &link = group["transcriptLink"];
                        if (s(link, "expectedType").rfind("waqf-", 0) == 0)
                            link["attachedLetterId"] = precedingLetter;
                    }
                }
                owner.update(grouped);
                append(allGroups, grouped["groups"]);
            }
            J sub = {{"id", sid},
                     {"wordId", wid},
                     {"text", sourceSub["text"]},
                     {"line", sourceWord["line"]},
                     {"pathIds", list},
                     {"bodyPathId", body},
                     {"nativeBodyPathId", nativeBody},
                     {"bodySelection", selection},
                     {"baseline", sourceWord["baseline"]},
                     {"nativeLegacyFlags", sourceWord["legacyFlags"]}};
            sub.update(result);
            if (!compound.empty())
                sub["compoundBody"] = std::move(compound);
            if (!decomposition.empty()) {
                const char *method = "existing-geometric-dot-pair-splitter";
                if (result.contains("singleDotTemplate")) method = "single-dot-template-and-packed-vowels";
                if (result.contains("dotTemplate")) method = "dot-templates-and-packed-vowels";
                sub["markDecomposition"] = {{"method", method},
                                            {"stage", "before-letter-ownership"},
                                            {"splits", decomposition}};
            }
            if (sourceSub.contains("nativeSubwordIds"))
                sub["nativeSubwordIds"] = sourceSub["nativeSubwordIds"];
            subs.push_back(sub);
            offset = end;
        }
    }
    J unowned = arr();
    for (auto it = paths.begin(); it != paths.end(); ++it) {
        require(ownership[it.key()] <= 1, "Native paths have duplicate subword membership");
        if (!ownership[it.key()])
            unowned.push_back(it.key());
    }
    std::map<std::string, int> claimed;
    for (const auto &sub : subs) {
        for (const auto &owner : sub["owners"]) {
            J owned = arr();
            for (const auto &id : owner["pathIds"]) {
                ++claimed[str(id)];
                owned.push_back(inkPaths.at(str(id)));
            }
            auto expectedInk = maskUnion(owned);
            require(maskUnion(owner["groups"]) == expectedInk,
                    "Semantic grouping changed assigned ink");
            int groupPixels = 0;
            for (const auto &g : owner["groups"])
                groupPixels += pixels(g["runs"]);
            require(groupPixels == pixels(expectedInk), "Semantic groups overlap assigned ink");
        }
        if (!sub["bodyPathId"].is_null())
            ++claimed[str(sub["bodyPathId"])];
        if (sub.contains("compoundBody"))
            ++claimed[s(sub["compoundBody"], "secondaryPathId")];
        for (const auto &id : sub["unusedPathIds"])
            ++claimed[str(id)];
    }
    for (const auto &group : ignored)
        for (const auto &id : group["pathIds"])
            ++claimed[str(id)];
    for (const auto &id : unowned)
        ++claimed[str(id)];
    std::set<std::string> units;
    for (auto it = paths.begin(); it != paths.end(); ++it)
        if (!decomposedParents.count(it.key()))
            units.insert(it.key());
    for (const auto &p : components)
        units.insert(s(p, "id"));
    require(units.size() == claimed.size(),
            "Assignment did not conserve native paths/components exactly");
    for (const auto &id : units)
        require(claimed[id] == 1, "Assignment did not conserve native paths/components exactly");
    // Keep raw contours for provenance. A counter removes ink from its own body only.
    for (const auto &p : sourcePaths) {
        const auto replacement = bodyPaint.find(s(p, "id"));
        const auto &runs = replacement == bodyPaint.end() ? p["runs"] : replacement->second;
        for (size_t i = 0; i < runs.size(); i += 2) {
            const int start = runs[i], length = runs[i + 1];
            std::fill_n(pageInk.begin() + start, length, 1);
        }
    }
    QByteArray bytes(reinterpret_cast<const char *>(pageInk.data()), pageInk.size());
    int pixelCount = std::count(pageInk.begin(), pageInk.end(), 1);
    J provenance = doc["provenance"];
    provenance["sourceSha256"] = doc["sourceSha256"];
    return {
        {"schemaVersion", 5},
        {"pageId", pid},
        {"raster", doc["raster"]},
        {"words", words},
        {"subwords", subs},
        {"paths", sourcePaths},
        {"assignmentComponents", components},
        {"groups", allGroups},
        {"ignoredSignGroups", ignored},
        {"unownedSourcePathIds", unowned},
        {"provenance", provenance},
        {"policy",
         {{"implementation", "medinamushafanalyzer native C++ mark assignment"},
          {"assignment", "Native right-edge sort; consume 1 + nbpath until each letter contour "
                         "quota is filled, then advance."},
          {"bodySelection",
           "Single-alef/hamza-below geometry; preserve source ink and ambiguous native choices."},
          {"verticalAssignment",
           "Native lower-contour quotas. At an overlapping letter boundary, prefer the next "
           "ambiguous path to an incompatible clear path when the next letter can take it. "
           "Reserve ambiguous paths before assigning the clear paths."},
          {"quotas", "QuranTextByWord + native charProps counts, minus each base body contour."},
          {"grouping",
           "Native geometric grouping after ownership; exact masks and frozen shape exemplars."},
          {"transcriptLinks", "Within each owner, link completed masks one-to-one to QuranTextByWord offsets "
                              "by contour count and native shape. Preserve unresolved alternatives."},
          {"singleDotTemplate",
           "When onedot matches have the wrong owners and one packed vowel pair is separable, "
           "retry RTL with its two existing masks. Require every letter's dot count and side, "
           "and a compatible vowel slot on each of two letters."},
          {"waqfReservation",
           "Preserve native WAQF flags. Unique template groups with balanced total and residual "
           "quotas; conservative highest-group fallback only with complete original quotas."},
          {"classifierUsedForAssignment", false},
          {"templatesUsedForAssignment", true},
          {"templatesUsedForOwnershipQuotas", false},
          {"templatesUsedForWaqfReservation", true},
          {"templatesUsedForGrouping", true},
          {"originalCandidateLabelsUsedForGrouping", false},
          {"corpusDatabaseUsed", false},
          {"letterSegmentationUsed", false},
          {"equivalence", "أٓ ≡ ـَٔا"},
          {"humanApprovalsWritten", 0}}},
        {"conservation",
         {{"sourcePaths", paths.size()},
          {"everyPathAccounted", true},
          {"masksUnchanged", true},
          {"inkPixels", pixelCount},
          {"inkSha256",
           QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString()}}}};
}
} // namespace NativeMarks

static std::string fileDigest(const QString &name) {
    QFile file(name);
    NativeMarks::require(file.open(QIODevice::ReadOnly), "Cannot hash " + name.toStdString());
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
static void nativeProvenance(NativeMarks::J &result, const std::string &rawHash,
                             const NativeMarks::J &transcript, const NativeMarks::J &geometryHash) {
    using namespace NativeMarks;
    static const J implementation = {
        {"producer", "medinamushafanalyzer-native-marks-v1"},
        {"binarySha256", fileDigest(QCoreApplication::applicationFilePath())},
        {"shapeBankSha256", fileDigest(":/templates/mark-shapes.json")},
        {"waqfTemplatesSha256", fileDigest(":/tools/waqf_templates.json")}};
    result["nativeAssignment"] = implementation;
    auto bytes = transcript.dump();
    result["nativeAssignment"].update(
        {{"rawInputSha256", rawHash},
         {"geometrySha256", geometryHash},
         {"transcriptSha256", QCryptographicHash::hash(QByteArray(bytes.data(), bytes.size()),
                                                       QCryptographicHash::Sha256)
                                  .toHex()
                                  .toStdString()}});
}

QJsonObject assignNativeMarks(const QJsonObject &page, const QJsonObject &transcript,
                              const QJsonObject &geometry) {
    auto convert = [](const QJsonObject &value) {
        auto data = QJsonDocument(value).toJson(QJsonDocument::Compact);
        return NativeMarks::J::parse(data.constData(), data.constData() + data.size());
    };
    auto result = NativeMarks::assignPage(convert(page), convert(transcript), convert(geometry),
                                          NativeMarks::shapeBank());
    nativeProvenance(result,
                     QCryptographicHash::hash(QJsonDocument(page).toJson(QJsonDocument::Compact),
                                              QCryptographicHash::Sha256)
                         .toHex()
                         .toStdString(),
                     convert(transcript), {});
    auto bytes = result.dump();
    return QJsonDocument::fromJson(QByteArray(bytes.data(), bytes.size())).object();
}

static QList<int> markPages(const QString &selection) {
    QSet<int> numbers;
    for (const auto &part : selection.split(',')) {
        auto limits = (part == "all" ? QString("1-604") : part).split('-');
        bool a = false, b = false;
        int first = limits[0].toInt(&a),
            last = limits.size() == 1 ? first : limits.value(1).toInt(&b);
        if (!a || (limits.size() != 1 && !b) || limits.size() > 2 || first < 1 || last > 604 ||
            last < first)
            throw std::runtime_error("Pages must be 1-604, ranges or all");
        for (int n = first; n <= last; ++n)
            numbers.insert(n);
    }
    auto result = numbers.values();
    std::sort(result.begin(), result.end());
    return result;
}
int exportNativeMarkAssignments(const QString &input, const QString &output,
                                const QString &selection) {
    using namespace NativeMarks;
    try {
        QDir().mkpath(output);
        auto transcript = readJson(input + "/transcript.json");
        require(s(transcript, "producer") == "medinamushafanalyzer-QuranTextByWord-v1",
                "Use the native QuranTextByWord export");
        auto pages = index(transcript["pages"], "pageId");
        J successes = arr(), failures = arr();
        for (int number : markPages(selection)) {
            auto pid = QString("page%1").arg(number, 3, 10, QChar('0'));
            try {
                auto file = input + "/" + pid + ".raw.json";
                auto doc = readJson(file);
                const auto rawHash = fileDigest(file);
                const auto geometryFile = input + "/geometry/" + pid + ".json";
                J geometry, geometryHash;
                if (QFile::exists(geometryFile)) {
                    auto frozen = readJson(geometryFile);
                    require(s(frozen, "rawInputSha256") == rawHash,
                            "Frozen geometry belongs to different native paths");
                    geometry = frozen["paths"];
                    geometryHash = fileDigest(geometryFile);
                } else {
                    // Full native exports already contain signatures and template distances.
                    // Older score-free exports require their separate frozen geometry file.
                    bool hasScores = false;
                    for (const auto& path : doc["paths"]) {
                        require(path.contains("signature") && path.contains("templateScores"),
                                "Native export lacks grouping geometry");
                        hasScores |= !path["templateScores"].empty();
                    }
                    require(hasScores, "Score-free exports require frozen inputs/geometry");
                    geometry = index(doc["paths"]);
                }
                auto result = assignPage(doc, pages.at(pid.toStdString()), geometry, shapeBank());
                nativeProvenance(result, rawHash, pages.at(pid.toStdString()), geometryHash);
                writeJson(output + "/" + pid + ".assignment.json", result);
                successes.push_back(pid.toStdString());
                std::cout << pid.toStdString() << ": native mark assignment complete" << std::endl;
            } catch (const std::exception &e) {
                failures.push_back({{"pageId", pid.toStdString()}, {"error", e.what()}});
                std::cerr << pid.toStdString() << ": " << e.what() << std::endl;
            }
        }
        writeJson(output + "/native-assignment-manifest.json",
                  {{"producer", "medinamushafanalyzer-native-marks-v1"},
                   {"pages", successes},
                   {"failures", failures},
                   {"pythonAssignmentUsed", false}});
        return failures.empty() ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
int testNativeMarkAssignment(const QString &input, const QString &output) {
    using namespace NativeMarks;
    try {
        auto requests = readJson(input);
        J results = arr();
        for (const auto &r : requests) {
            try {
                auto op = s(r, "operation");
                J result;
                if (op == "expectedLetters")
                    result = expectedLetters(r.at("word"));
                else if (op == "consume")
                    result = consume(r.at("paths"), r.at("owners"));
                else if (op == "consumeVertical")
                    result =
                        consumeVertical(r.at("paths"), r.at("owners"), r.value("baseline", J()),
                                        r.value("bodyBox", J()), r.value("scale", 4.));
                else if (op == "selectBody")
                    result = selectBody(r.at("paths"), r.at("owners"), r.at("nativeBody"));
                else if (op == "normalizeDetached")
                    result = normalizeDetached(r.at("subwords"));
                else if (op == "decomposeDotPairs")
                    result =
                        decomposeDotPairs(r.at("paths"), n(r, "width"), r.value("bank", obj()));
                else if (op == "doubleDotEvidence")
                    result = doubleDotEvidence(r.at("path"), r.value("bank", obj()), n(r, "width"));
                else if (op == "validateDecomposition") {
                    validateDecomposition(r.at("source"), r.at("parts"), r.at("paths"));
                    result = true;
                } else if (op == "groupOwnedInk")
                    result = groupOwnedInk(r.at("owner"), r.at("paths"), n(r, "width"),
                                           r.value("baseline", J()), r.value("geometry", obj()),
                                           r.value("bank", obj()), r.value("conflicts", J()));
                else if (op == "boundaryConflicts")
                    result = boundaryConflicts(r.at("owners"), r.at("paths"), n(r, "width"),
                                               r.value("baseline", J()), r.value("geometry", obj()),
                                               r.value("bank", obj()));
                else if (op == "geometricPartition")
                    result = geometricPartition(r.at("owner"), r.at("paths"), n(r, "width"),
                                                r.value("baseline", J()),
                                                r.value("geometry", obj()), r.value("bank", obj()));
                else if (op == "topWaqfCandidate")
                    result = topWaqfCandidate(r.at("paths"), n(r, "contours"));
                else if (op == "templateMatch")
                    result = templateMatch(r.at("path"), s(r, "name"),
                                           r["scale"].is_null()
                                               ? std::numeric_limits<double>::quiet_NaN()
                                               : d(r, "scale"));
                else if (op == "dilatedSignature")
                    result = hex(dilated(bits(r.at("signature"))));
                else if (op == "reserveWaqf")
                    result = reserveWaqf(r.at("paths"), r.at("owners"), r.at("original"),
                                         r.value("baseline", J()), r.value("bodyBox", J()),
                                         r.value("scale", 4.), r.value("geometry", obj()),
                                         r.value("width", 0));
                else if (op == "waqfCandidates")
                    result = waqfCandidates(r.at("paths"), r.value("geometry", obj()),
                                            n(r, "width"), r.at("baseline"), d(r, "scale"));
                else if (op == "assignPage")
                    result = assignPage(r.at("native"), r.at("transcript"),
                                        r.value("geometry", obj()), r.value("bank", obj()));
                else
                    throw std::runtime_error("Unknown native test operation");
                results.push_back({{"result", result}});
            } catch (const std::exception &e) {
                results.push_back({{"error", e.what()}});
            }
        }
        writeJson(output, results);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
