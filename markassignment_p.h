#pragma once
#include <QString>
#include <nlohmann/json.hpp>
#include <bitset>
#include <map>
#include <set>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace NativeMarks {
using J = nlohmann::json;
using Bits = std::bitset<256>;
using Point = std::pair<int, int>; // y, x
using Piece = std::vector<Point>;
inline J arr() { return J::array(); }
inline J obj() { return J::object(); }
inline std::string str(const J &j) { return j.is_string() ? j.get<std::string>() : std::string(); }
inline std::string s(const J &j, const std::string &k) { return str(j.value(k, J())); }
inline int n(const J &j, const std::string &k) { return j.at(k).get<int>(); }
inline double d(const J &j, const std::string &k) { return j.at(k).get<double>(); }
inline J merged(J a, const J &b) {
    a.update(b);
    return a;
}
inline void append(J &a, const J &b) {
    for (const auto &v : b)
        a.push_back(v);
}
inline void require(bool yes, const std::string &why) {
    if (!yes)
        throw std::runtime_error(why);
}
inline std::array<double, 4> box(const J &p) { return p.at("bbox").get<std::array<double, 4>>(); }
inline std::set<std::string> ids(const J &a) {
    std::set<std::string> out;
    for (const auto &v : a)
        out.insert(str(v));
    return out;
}
inline J subset(const J &j, std::initializer_list<const char *> keys) {
    J out = obj();
    for (auto k : keys)
        if (j.contains(k))
            out[k] = j[k];
    return out;
}
inline J index(const J &a, const char *key = "id") {
    J out = obj();
    for (const auto &v : a)
        out[s(v, key)] = v;
    return out;
}
inline J pathIds(const J &paths) {
    J out = arr();
    for (const auto &p : paths)
        out.push_back(p["id"]);
    return out;
}
inline int totalContours(const J &paths) {
    int out = 0;
    for (const auto &p : paths)
        out += n(p, "contourCount");
    return out;
}
inline std::map<int, int> counts(const J &a, const char *key) {
    std::map<int, int> out;
    for (const auto &p : a)
        ++out[n(p, key)];
    return out;
}
inline bool issues(const J &result) {
    if (!result["unusedPathIds"].empty())
        return true;
    for (const auto &o : result["owners"])
        if (!o["issues"].empty())
            return true;
    return false;
}
inline int pixels(const J &runs) {
    int count = 0;
    for (size_t i = 1; i < runs.size(); i += 2)
        count += runs[i].get<int>();
    return count;
}
inline bool ends(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
inline std::vector<std::vector<int>> combinations(int n, int k) {
    std::vector<std::vector<int>> out;
    std::vector<int> path;
    std::function<void(int)> visit = [&](int start) {
        if (int(path.size()) == k) {
            out.push_back(path);
            return;
        }
        for (int i = start; i < n; ++i) {
            path.push_back(i);
            visit(i + 1);
            path.pop_back();
        }
    };
    visit(0);
    return out;
}
inline J selected(const J &a, const std::vector<int> &indices) {
    J out = arr();
    for (int i : indices)
        out.push_back(a[i]);
    return out;
}
J readJson(const QString &file);
void writeJson(const QString &file, const J &value);
const J &shapeBank();
const J &waqfBank();
Bits bits(const J &value);
std::string hex(const Bits &value);
J maskUnion(const J &paths);
J inkBox(const J &runs, int width);
J clusterBox(const J &ids, const J &paths);
std::vector<Piece> connectedInk(const J &runs, int width);
bool hasCounter(const Piece &piece);
J componentPath(const J &path, int width, const Piece &piece, const std::string &suffix,
                bool signature = false);
J stroke(const Piece &piece);
J strokePaths(const J &path, int width, bool split);
bool strokePair(const J &paths, bool staggered = false);
double shapeDistance(const J &path, const J &prototype);
J doubleDotEvidence(const J &path, const J &bank, int width);
J splitDotPairs(const J &path, int width, const J &bank);
J decomposeDotPairs(const J &paths, int width, const J &bank);
std::string strongType(const J &path);
J geometricPartition(const J &owner, const J &paths, int width, J baseline, const J &geometry,
                     const J &bank);
J boundaryConflicts(const J &owners, const J &paths, int width, J baseline, const J &geometry,
                    const J &bank);
J groupOwnedInk(const J &owner, const J &paths, int width, J baseline, const J &geometry,
                const J &bank, const J &conflicts = J());
void linkTranscriptCharacters(J &groups, const J &owner, const std::string &wordId, const J &paths,
                              const J &geometry, const J &bank, int width);
J expectedLetters(const J &word);
J selectBody(const J &paths, const J &owners, const J &nativeBody);
J consume(const J &paths, const J &owners);
J consumeVertical(const J &paths, const J &owners, J baseline, J bodyBox = J(), double scale = 4);
J waqfCandidates(const J &paths, const J &geometry, int width, J baseline, double scale);
J topWaqfCandidate(const J &paths, int contours);
J reserveWaqf(const J &paths, const J &owners, J original, J baseline, J bodyBox, double scale,
              const J &geometry, int width);
Bits dilated(Bits signature);
J templateMatch(const J &path, const std::string &name, double scale);
J assignPage(const J &native, const J &transcript, const J &geometry, const J &bank);
} // namespace NativeMarks
