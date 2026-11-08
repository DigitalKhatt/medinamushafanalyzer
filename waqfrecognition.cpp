#include "markassignment_p.h"

namespace NativeMarks {
Bits dilated(Bits bits) {
    static const Bits left = []() {
        Bits x;
        for (int y = 0; y < 16; ++y)
            x.set(255 - 16 * y);
        return x;
    }();
    static const Bits right = []() {
        Bits x;
        for (int y = 0; y < 16; ++y)
            x.set(240 - 16 * y);
        return x;
    }();
    Bits h = bits | ((bits & ~left) << 1) | ((bits & ~right) >> 1);
    return h | (h << 16) | (h >> 16);
}
struct WaqfTemplate {
    std::string name;
    Bits signature, expanded;
    double aspect, height;
};
static const std::map<std::string, WaqfTemplate> &templates() {
    static const auto bank = []() {
        std::map<std::string, WaqfTemplate> out;
        for (const auto &p : waqfBank()["templates"]) {
            auto b = bits(p["signature"]);
            out.emplace(s(p, "name"), WaqfTemplate{s(p, "name"), b, dilated(b), d(p, "aspect"),
                                                   d(p, "heightPdf")});
        }
        return out;
    }();
    return bank;
}
J templateMatch(const J &path, const std::string &name, double scale) {
    auto it = templates().find(name);
    auto b = box(path);
    if (it == templates().end() || !std::isfinite(scale) || std::min({b[2], b[3], scale}) <= 0 ||
        !std::all_of(b.begin(), b.end(), [](double x) { return std::isfinite(x); }))
        return J();
    const auto &t = it->second;
    double tolerance = .12 + (path.value("rasterizedComponent", false) ? .5 / b[2] + .5 / b[3] : 0);
    if (std::abs(std::log((b[2] / b[3]) / t.aspect)) > tolerance ||
        b[3] / (scale * t.height) < .65 || b[3] / (scale * t.height) > 1.5)
        return J();
    Bits a;
    try {
        a = bits(path.value("signature", J("0")));
    } catch (const std::exception &) {
        return J();
    }
    if (a.none())
        return J();
    double iou = 1. - double((a & t.signature).count()) / (a | t.signature).count();
    if (iou > .45)
        return J();
    double outside = double((a & ~t.expanded).count() + (t.signature & ~dilated(a)).count()) /
                     (a.count() + t.signature.count());
    if (outside > .025)
        return J();
    return {{"template", name},
            {"outsideFraction", outside},
            {"iouError", iou},
            {"method", "cached-native-signature-with-one-cell-tolerance"}};
}
J waqfCandidates(const J &paths, const J &geometry, int width, J baseline, double scale) {
    if (baseline.is_null() || !std::isfinite(baseline.get<double>()) || !std::isfinite(scale) ||
        scale <= 0)
        return arr();
    J ink = arr(), evidence = obj(), candidates = arr();
    for (auto p : paths) {
        auto b = box(p);
        if (b[1] + b[3] >= baseline.get<double>())
            continue;
        p["signature"] =
            geometry.value(s(p, "id"), obj()).value("signature", p.value("signature", J("0")));
        ink.push_back(p);
        if (n(p, "contourCount") == 1) {
            J e = obj();
            for (const auto &[name, t] : templates()) {
                auto m = templateMatch(p, name, scale);
                if (!m.is_null())
                    e[name] = m;
            }
            evidence[s(p, "id")] = e;
        }
    }
    auto has = [&](const J &p, const std::string &name) {
        return evidence.contains(s(p, "id")) && evidence[s(p, "id")].contains(name);
    };
    auto add = [&](const std::string &type, const J &parts, const J &matches) {
        candidates.push_back({{"type", type},
                              {"pathIds", pathIds(parts)},
                              {"contours", totalContours(parts)},
                              {"evidence", matches}});
    };
    for (const auto &p : ink)
        for (auto [name, kind] : std::vector<std::pair<std::string, std::string>>{
                 {"meem.waqf", "waqf-meem"}, {"sad.waqf", "waqf-sad"}})
            if (has(p, name))
                add(kind, J::array({p}), J::array({evidence[s(p, "id")][name]}));
    for (const auto &body : ink)
        for (auto [name, dotName, kind] :
             std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"qaf.waqf.body", "twodots.waqf", "waqf-qaf"},
                 {"hah.waqf", "onedot.waqf", "waqf-jeem"}}) {
            if (!has(body, name))
                continue;
            auto b = box(body);
            for (const auto &dot : ink) {
                if (s(dot, "id") == s(body, "id") || !has(dot, dotName))
                    continue;
                auto p = box(dot);
                bool fits = b[0] <= p[0] && p[0] + p[2] <= b[0] + b[2] &&
                            (kind == "waqf-jeem" ? (b[1] <= p[1] && p[1] + p[3] <= b[1] + b[3])
                                                 : (b[1] - p[3] <= p[1] + p[3] &&
                                                    p[1] + p[3] <= b[1] + .25 * b[3]));
                if (fits)
                    add(kind, J::array({body, dot}),
                        J::array({evidence[s(body, "id")][name], evidence[s(dot, "id")][dotName]}));
            }
        }
    J dotPaths = arr();
    for (const auto &p : ink) {
        if (n(p, "contourCount") < 1 || n(p, "contourCount") > 3)
            continue;
        if (has(p, "onedot"))
            dotPaths.push_back({{"path", p},
                                {"parts", J::array({p})},
                                {"matches", J::array({evidence[s(p, "id")]["onedot"]})}});
        else if (n(p, "contourCount") > 1) {
            auto pieces = connectedInk(p["runs"], width);
            if (int(pieces.size()) != n(p, "contourCount"))
                continue;
            J parts = arr(), matches = arr();
            bool good = true;
            for (size_t i = 0; i < pieces.size(); ++i) {
                auto part = componentPath(p, width, pieces[i], std::to_string(i), true);
                part["rasterizedComponent"] = true;
                parts.push_back(part);
                auto m = templateMatch(part, "onedot", scale);
                if (m.is_null()) {
                    good = false;
                    break;
                }
                matches.push_back(m);
            }
            if (good)
                dotPaths.push_back({{"path", p}, {"parts", parts}, {"matches", matches}});
        }
    }
    for (int count : {1, 2, 3})
        for (auto combo : combinations(dotPaths.size(), count)) {
            J parts = arr(), whole = arr(), matches = arr();
            for (int i : combo) {
                append(parts, dotPaths[i]["parts"]);
                whole.push_back(dotPaths[i]["path"]);
                append(matches, dotPaths[i]["matches"]);
            }
            if (parts.size() != 3)
                continue;
            std::stable_sort(parts.begin(), parts.end(),
                             [](const J &a, const J &b) { return box(a)[1] < box(b)[1]; });
            auto upper = box(parts[0]), left = box(parts[1]), right = box(parts[2]);
            if (left[0] > right[0])
                std::swap(left, right);
            double hi = std::max({upper[3], left[3], right[3]}),
                   lo = std::min({upper[3], left[3], right[3]});
            if (hi > 1.25 * lo)
                continue;
            double ux = upper[0] + upper[2] / 2, uy = upper[1] + upper[3] / 2,
                   lx = left[0] + left[2] / 2, ly = left[1] + left[3] / 2,
                   rx = right[0] + right[2] / 2, ry = right[1] + right[3] / 2,
                   unit = (upper[3] + left[3] + right[3]) / 3;
            if (lx < ux && ux < rx && .8 * unit < rx - lx && rx - lx < 2.5 * unit &&
                .7 * unit < std::min(ly, ry) - uy && std::min(ly, ry) - uy < 2.2 * unit &&
                std::abs(ly - ry) < .35 * unit)
                add("waqf-three-dots", whole, matches);
        }
    return candidates;
}
static bool overlap(const J &first, const J &second) {
    auto a = box(first), b = box(second);
    return std::max(a[0], b[0]) < std::min(a[0] + a[2], b[0] + b[2]) &&
           std::max(a[1], b[1]) < std::min(a[1] + a[3], b[1] + b[3]);
}
J topWaqfCandidate(const J &paths, int contours) {
    auto reject = [](const char *reason) { return J{{"paths", J()}, {"reason", reason}}; };
    if (paths.empty() || contours < 1 || contours > 3)
        return reject("invalid-or-unsupported-geometry");
    for (const auto &p : paths) {
        auto b = box(p);
        if (!std::all_of(b.begin(), b.end(), [](double x) { return std::isfinite(x); }) ||
            std::min(b[2], b[3]) <= 0)
            return reject("invalid-or-unsupported-geometry");
    }
    J ordered = paths;
    std::stable_sort(ordered.begin(), ordered.end(), [](const J &a, const J &b) {
        double ay = box(a)[1], by = box(b)[1];
        return ay != by ? ay < by : s(a, "id") < s(b, "id");
    });
    auto seed = ordered[0];
    if (n(seed, "contourCount") > contours)
        return reject("highest-path-exceeds-waqf-quota");
    J solutions = arr();
    for (int count = 0; count <= contours - n(seed, "contourCount"); ++count)
        for (auto combo : combinations(ordered.size() - 1, count)) {
            J group = J::array({seed});
            for (int i : combo)
                group.push_back(ordered[i + 1]);
            if (totalContours(group) != contours)
                continue;
            std::set<std::string> connected{s(seed, "id")};
            for (size_t step = 0; step < group.size(); ++step)
                for (const auto &p : group)
                    if (!connected.count(s(p, "id")))
                        for (const auto &q : group)
                            if (connected.count(s(q, "id")) && overlap(p, q)) {
                                connected.insert(s(p, "id"));
                                break;
                            }
            if (connected.size() == group.size()) {
                solutions.push_back(group);
                if (solutions.size() > 1)
                    return reject("ambiguous-spatial-group");
            }
        }
    if (solutions.empty())
        return reject("missing-spatial-group");
    return {{"paths", solutions[0]}, {"reason", J()}};
}
J reserveWaqf(const J &paths, const J &buckets, J original, J baseline, J bodyBox, double scale,
              const J &geometry, int width) {
    std::vector<int> stops;
    for (size_t i = 0; i < buckets.size(); ++i)
        if (s(buckets[i], "role") == "word-sign")
            for (const auto &slot : buckets[i]["expectedSlots"])
                if (s(slot, "type").rfind("waqf-", 0) == 0) {
                    stops.push_back(i);
                    break;
                }
    if (stops.size() != 1 || buckets.size() < 2)
        return original;
    int stopIndex = stops[0];
    for (size_t i = 0; i < buckets.size(); ++i)
        if (s(buckets[i], "role") != "letter" && int(i) != stopIndex)
            return original;
    for (const auto &p : paths)
        if (p.value("legacyType", 0) == 1)
            return original;
    const auto &stop = buckets[stopIndex];
    J detail = {{"method", "unflagged-waqf-geometry-and-contour-quota"},
                {"status", "fallback"},
                {"ownerId", stop["id"]},
                {"expectedContours", stop["expectedContours"]},
                {"changedOwnerIds", arr()}};
    auto keep = [&](const char *why) {
        detail["reason"] = why;
        original["waqfReservation"] = detail;
        return original;
    };
    int quota = 0;
    for (const auto &b : buckets)
        quota += n(b, "expectedContours");
    if (totalContours(paths) != quota)
        return keep("original-contour-totals-disagree");
    J recognized = geometry.empty() || !width
                       ? arr()
                       : waqfCandidates(paths, geometry, width, baseline, scale),
      matching = arr();
    std::string type;
    for (const auto &slot : stop["expectedSlots"])
        if (s(slot, "type").rfind("waqf-", 0) == 0) {
            type = s(slot, "type");
            break;
        }
    for (const auto &c : recognized)
        if (s(c, "type") == type && n(c, "contours") == n(stop, "expectedContours"))
            matching.push_back(c);
    detail["recognitionCandidates"] = recognized;
    if (matching.size() > 1)
        return keep("multiple-waqf-template-candidates");
    J chosen = arr();
    if (!matching.empty()) {
        auto candidate = ids(matching[0]["pathIds"]);
        for (const auto &p : paths)
            if (candidate.count(s(p, "id")))
                chosen.push_back(p);
        detail["selectionMethod"] = "geometric-templates";
        J quotaIssues = arr();
        for (const auto &o : original["owners"])
            if (!o["issues"].empty())
                quotaIssues.push_back({{"ownerId", o["id"]}, {"issues", o["issues"]}});
        detail["originalQuotaIssues"] = quotaIssues;
        detail["originalUnusedPathIds"] = original["unusedPathIds"];
    } else {
        if (issues(original))
            return keep("original-contour-quota-difference");
        auto candidate = topWaqfCandidate(paths, n(stop, "expectedContours"));
        detail["selectionMethod"] = "highest-spatial-group";
        if (!candidate["reason"].is_null())
            return keep(s(candidate, "reason").c_str());
        chosen = candidate["paths"];
    }
    auto selectedIds = ids(pathIds(chosen));
    detail["candidatePathIds"] = pathIds(chosen);
    auto previous = original["owners"][stopIndex];
    if (selectedIds == ids(previous["pathIds"])) {
        detail["status"] = "unchanged";
        return keep(matching.empty() ? "highest-spatial-group-already-owned-by-waqf"
                                     : "recognized-group-already-owned-by-waqf");
    }
    J remaining = arr(), letters = arr();
    for (const auto &p : paths)
        if (!selectedIds.count(s(p, "id")))
            remaining.push_back(p);
    if (remaining.empty() || !std::isfinite(scale) || scale <= 0)
        return keep("remaining-marks-or-scale-unavailable");
    double top = INFINITY, next = INFINITY;
    for (const auto &p : chosen)
        top = std::min(top, box(p)[1]);
    for (const auto &p : remaining)
        next = std::min(next, box(p)[1]);
    detail.update({{"waqfTop", top},
                   {"highestRemainingMarkTop", next},
                   {"clearance", next - top},
                   {"requiredClearance", .5 * scale}});
    if (matching.empty() && next - top <= .5 * scale)
        return keep("top-edge-margin-ambiguous");
    for (size_t i = 0; i < buckets.size(); ++i)
        if (int(i) != stopIndex)
            letters.push_back(buckets[i]);
    auto residual = consumeVertical(remaining, letters, baseline, bodyBox, scale);
    if (issues(residual))
        return keep("remaining-letter-quotas-do-not-fit");
    J byId = index(residual["owners"]);
    byId[s(stop, "id")] = consume(chosen, J::array({stop}))["owners"][0];
    J owners = arr(), consumed = arr(), changes = arr(), changed = arr();
    for (size_t i = 0; i < buckets.size(); ++i) {
        auto owner = byId[s(buckets[i], "id")];
        auto before = original["owners"][i];
        if (ids(before["pathIds"]) != ids(owner["pathIds"])) {
            changes.push_back({{"ownerId", owner["id"]},
                               {"before", before["pathIds"]},
                               {"after", owner["pathIds"]}});
            changed.push_back(owner["id"]);
        }
        owner["startIndex"] = consumed.size();
        append(consumed, owner["pathIds"]);
        owner["endIndex"] = consumed.size();
        owners.push_back(owner);
    }
    detail.update({{"status", "applied"},
                   {"reason", matching.empty() ? "unique-highest-spatial-group-with-complete-quotas"
                                               : "unique-template-group-with-complete-quotas"},
                   {"changes", changes},
                   {"changedOwnerIds", changed},
                   {"previousWaqfPathIds", previous["pathIds"]}});
    return {{"owners", owners},          {"initialOrder", original["initialOrder"]},
            {"consumedOrder", consumed}, {"swaps", residual["swaps"]},
            {"unusedPathIds", arr()},    {"verticalAssignment", residual["verticalAssignment"]},
            {"waqfReservation", detail}};
}
} // namespace NativeMarks
