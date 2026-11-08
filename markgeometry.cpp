#include "markassignment_p.h"
#include <QFile>
#include <QSaveFile>
#include <QCryptographicHash>
#include <queue>

namespace NativeMarks {
J readJson(const QString &file) {
    QFile input(file);
    require(input.open(QIODevice::ReadOnly), "Cannot read " + file.toStdString());
    const auto bytes = input.readAll();
    return J::parse(bytes.constData(), bytes.constData() + bytes.size());
}
void writeJson(const QString &file, const J &value) {
    QSaveFile output(file);
    const auto bytes = value.dump() + "\n";
    require(output.open(QIODevice::WriteOnly) &&
                output.write(bytes.data(), bytes.size()) == qint64(bytes.size()) && output.commit(),
            "Cannot write " + file.toStdString());
}
const J &shapeBank() {
    static const J bank = readJson(":/templates/mark-shapes.json")["prototypes"];
    return bank;
}
const J &waqfBank() {
    static const J bank = readJson(":/tools/waqf_templates.json");
    return bank;
}
Bits bits(const J &value) {
    require(value.is_string(), "Geometric signatures must use lossless hexadecimal encoding");
    const auto text = value.get<std::string>();
    require(text.size() <= 64, "Invalid geometric signature");
    Bits out;
    for (char c : text) {
        int v = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        require(v >= 0, "Invalid geometric signature");
        out = (out << 4) | Bits(v);
    }
    return out;
}
std::string hex(const Bits &value) {
    std::string out;
    for (int nibble = 63; nibble >= 0; --nibble) {
        int v = 0;
        for (int k = 3; k >= 0; --k)
            v = (v << 1) | value[nibble * 4 + k];
        out += "0123456789abcdef"[v];
    }
    return out;
}
J maskUnion(const J &paths) {
    std::vector<std::pair<int, int>> intervals;
    for (const auto &p : paths) {
        const auto &r = p["runs"];
        require(r.size() % 2 == 0, "Odd mask RLE");
        for (size_t i = 0; i < r.size(); i += 2) {
            int a = r[i], n = r[i + 1];
            require(a >= 0 && n > 0, "Invalid mask RLE");
            intervals.emplace_back(a, a + n);
        }
    }
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<int, int>> merged;
    for (auto p : intervals) {
        if (!merged.empty() && p.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, p.second);
        else
            merged.push_back(p);
    }
    J out = arr();
    for (auto [a, b] : merged) {
        out.push_back(a);
        out.push_back(b - a);
    }
    return out;
}
J inkBox(const J &runs, int width) {
    int left = width, right = 0, top = INT_MAX, bottom = 0;
    for (size_t i = 0; i < runs.size(); i += 2) {
        int start = runs[i], count = runs[i + 1];
        while (count) {
            int y = start / width, x = start % width, n = std::min(count, width - x);
            left = std::min(left, x);
            right = std::max(right, x + n);
            top = std::min(top, y);
            bottom = std::max(bottom, y + 1);
            count -= n;
            start += n;
        }
    }
    require(top != INT_MAX, "Empty native ink mask");
    return J::array({left, top, right - left, bottom - top});
}
J clusterBox(const J &list, const J &paths) {
    double x = INFINITY, y = INFINITY, right = -INFINITY, bottom = -INFINITY;
    for (const auto &id : list) {
        auto b = box(paths.at(str(id)));
        x = std::min(x, b[0]);
        y = std::min(y, b[1]);
        right = std::max(right, b[0] + b[2]);
        bottom = std::max(bottom, b[1] + b[3]);
    }
    return J::array({x, y, right - x, bottom - y});
}
std::vector<Piece> connectedInk(const J &runs, int width) {
    std::set<Point> ink;
    for (size_t i = 0; i < runs.size(); i += 2) {
        int start = runs[i], count = runs[i + 1];
        for (int p = start; p < start + count; ++p)
            ink.emplace(p / width, p % width);
    }
    std::vector<Piece> pieces;
    while (!ink.empty()) {
        std::vector<Point> pending{*ink.begin()};
        ink.erase(ink.begin());
        Piece piece;
        while (!pending.empty()) {
            auto [y, x] = pending.back();
            pending.pop_back();
            piece.emplace_back(y, x);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    auto it = ink.find({y + dy, x + dx});
                    if (it != ink.end()) {
                        pending.push_back(*it);
                        ink.erase(it);
                    }
                }
        }
        std::sort(piece.begin(), piece.end());
        pieces.push_back(piece);
    }
    std::sort(pieces.begin(), pieces.end(),
              [](const auto &a, const auto &b) { return a.front() < b.front(); });
    return pieces;
}
bool hasCounter(const Piece &piece) {
    if (piece.empty())
        return false;
    std::set<Point> ink(piece.begin(), piece.end()), empty;
    int top = INT_MAX, bottom = INT_MIN, left = INT_MAX, right = INT_MIN;
    for (auto [y, x] : piece) {
        top = std::min(top, y - 1);
        bottom = std::max(bottom, y + 1);
        left = std::min(left, x - 1);
        right = std::max(right, x + 1);
    }
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x)
            if (!ink.count({y, x}))
                empty.emplace(y, x);
    std::vector<Point> pending{{top, left}};
    empty.erase({top, left});
    while (!pending.empty()) {
        auto [y, x] = pending.back();
        pending.pop_back();
        for (auto p : Piece{{y - 1, x}, {y + 1, x}, {y, x - 1}, {y, x + 1}}) {
            auto it = empty.find(p);
            if (it != empty.end()) {
                pending.push_back(p);
                empty.erase(it);
            }
        }
    }
    return !empty.empty();
}
J componentPath(const J &path, int width, const Piece &piece, const std::string &suffix,
                bool withSignature) {
    J runs = arr();
    int left = INT_MAX, top = INT_MAX, right = 0, bottom = 0;
    for (auto [y, x] : piece) {
        int offset = y * width + x;
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x + 1);
        bottom = std::max(bottom, y + 1);
        if (!runs.empty() && runs[runs.size() - 2].get<int>() + runs.back().get<int>() == offset)
            runs.back() = runs.back().get<int>() + 1;
        else {
            runs.push_back(offset);
            runs.push_back(1);
        }
    }
    J out = merged(path, {{"id", s(path, "id") + "-" + suffix},
                          {"sourcePathId", path["id"]},
                          {"bbox", J::array({left, top, right - left, bottom - top})},
                          {"runs", runs},
                          {"contourCount", 1},
                          {"templateScores", obj()},
                          {"signature", "0"}});
    if (withSignature) {
        std::set<Point> ink(piece.begin(), piece.end());
        Bits b;
        for (int row = 0; row < 16; ++row)
            for (int col = 0; col < 16; ++col)
                b = (b << 1) | Bits(ink.count({top + (2 * row + 1) * (bottom - top) / 32,
                                               left + (2 * col + 1) * (right - left) / 32}));
        out["signature"] = hex(b);
    }
    return out;
}
J stroke(const Piece &piece) {
    if (piece.size() < 8)
        return J();
    double x = 0, y = 0;
    for (auto [yy, xx] : piece) {
        x += xx;
        y += yy;
    }
    x /= piece.size();
    y /= piece.size();
    double xx = 0, yy = 0, xy = 0;
    for (auto [b, a] : piece) {
        xx += (a - x) * (a - x);
        yy += (b - y) * (b - y);
        xy += (a - x) * (b - y);
    }
    double spread = std::hypot(xx - yy, 2 * xy), major = xx + yy + spread, minor = xx + yy - spread,
           angle = std::atan2(2 * xy, xx - yy) / 2;
    if (major < 9 * std::max(minor, 1.) || angle < -.85 || angle > .12)
        return J();
    return J::array({x, y, angle, std::sqrt(major / piece.size())});
}
J strokePaths(const J &path, int width, bool split) {
    auto pieces = connectedInk(path["runs"], width);
    J profiles = arr();
    bool valid = !pieces.empty();
    for (const auto &p : pieces) {
        profiles.push_back(stroke(p));
        valid &= !profiles.back().is_null();
    }
    if (pieces.size() == 1)
        return J::array({merged(path, {{"stroke", profiles[0]}})});
    if (!split || !valid)
        return J::array({path});
    J out = arr();
    for (size_t i = 0; i < pieces.size(); ++i)
        out.push_back(
            merged(componentPath(path, width, pieces[i], "stroke-" + std::to_string(i + 1), false),
                   {{"stroke", profiles[i]}}));
    return out;
}
bool strokePair(const J &paths, bool staggered) {
    if (paths.size() != 2 || paths[0].value("stroke", J()).is_null() ||
        paths[1].value("stroke", J()).is_null())
        return false;
    auto a = paths[0]["stroke"].get<std::array<double, 4>>(),
         b = paths[1]["stroke"].get<std::array<double, 4>>();
    double angle = (a[2] + b[2]) / 2, dx = b[0] - a[0], dy = b[1] - a[1];
    double along = std::abs(dx * std::cos(angle) + dy * std::sin(angle)),
           across = std::abs(dy * std::cos(angle) - dx * std::sin(angle)),
           scale = std::min(a[3], b[3]);
    if (std::abs(a[2] - b[2]) <= .23 && std::max(a[3], b[3]) <= 1.6 * scale &&
        along <= 2.25 * scale && across >= .25 * scale && across <= 1.5 * scale)
        return true;
    if (!staggered || !paths[0].contains("bbox") || !paths[1].contains("bbox"))
        return false;
    auto upper = box(paths[0]), lower = box(paths[1]);
    if (upper[1] > lower[1])
        std::swap(upper, lower);
    return lower[1] - (upper[1] + upper[3]) >= 0 &&
           std::min(upper[0] + upper[2], lower[0] + lower[2]) - std::max(upper[0], lower[0]) > 0 &&
           dx * dy < 0 && std::abs(a[2] - b[2]) <= .08 && std::max(a[3], b[3]) <= 1.15 * scale &&
           along <= 2.5 * scale && across >= .75 * scale && across <= 1.25 * scale;
}
double shapeDistance(const J &path, const J &prototype) {
    auto b = box(path);
    Bits a = bits(path.value("signature", J("0"))), p = bits(prototype[0]);
    return 1. - double((a & p).count()) / std::max<size_t>(1, (a | p).count()) +
           .12 * std::abs(std::log((b[2] / std::max(b[3], .01)) / prototype[1].get<double>())) +
           .12 * std::abs(std::log(b[3] / prototype[2].get<double>()));
}
std::string strongType(const J &path) {
    const auto scores = path.value("templateScores", obj());
    std::vector<std::pair<double, std::string>> values;
    for (auto [name, label] : std::vector<std::pair<std::string, std::string>>{
             {"onedot", "single-dot"}, {"twodots", "double-dot"}, {"damma", "damma"}})
        values.emplace_back(scores.value(name, 1.), label);
    auto best = *std::min_element(values.begin(), values.end());
    return best.first < .18                                                         ? best.second
           : path.contains("dotShapeEvidence") && !path["dotShapeEvidence"].empty() ? "double-dot"
                                                                                    : "";
}
J doubleDotEvidence(const J &path, const J &bank, int width) {
    auto b = box(path);
    if (n(path, "contourCount") != 1 || b[3] <= 0 || b[2] / b[3] < 1.2 || b[2] / b[3] > 2.8 ||
        !bank.contains("double-dot"))
        return J();
    double score = INFINITY, other = 1.;
    for (const auto &proto : bank["double-dot"])
        score = std::min(score, shapeDistance(path, proto));
    if (score > .12)
        return J();
    bool found = false;
    for (auto it = bank.begin(); it != bank.end(); ++it)
        if (it.key() != "double-dot")
            for (const auto &proto : it.value()) {
                double value = shapeDistance(path, proto);
                other = found ? std::min(other, value) : value;
                found = true;
            }
    if (other - score < .2)
        return J();
    auto pieces = connectedInk(path["runs"], width);
    if (pieces.size() != 1 || hasCounter(pieces[0]))
        return J();
    return {{"method", "train-page-signature"},
            {"type", "double-dot"},
            {"distance", score},
            {"otherTypeDistance", other}};
}
J splitDotPairs(const J &path, int width, const J &bank) {
    if (n(path, "contourCount") != 2)
        return J::array({path});
    auto pieces = connectedInk(path["runs"], width);
    if (pieces.size() != 2)
        return J::array({path});
    J out = arr();
    for (size_t i = 0; i < pieces.size(); ++i) {
        auto p = componentPath(path, width, pieces[i], "dot-pair-" + std::to_string(i + 1), true);
        auto evidence = doubleDotEvidence(p, bank, width);
        if (evidence.is_null())
            return J::array({path});
        p["dotShapeEvidence"] = evidence;
        out.push_back(p);
    }
    return out;
}
J decomposeDotPairs(const J &paths, int width, const J &bank) {
    J out = arr();
    for (const auto &p : paths)
        append(out, bank.empty() ? J::array({p}) : splitDotPairs(p, width, bank));
    return out;
}

static J splitDammas(const J &path, int width, const J &prototypes) {
    auto pieces = connectedInk(path["runs"], width);
    if (pieces.size() != 2)
        return J::array({path});
    auto cx = [](const Piece &p) {
        double sum = 0;
        for (auto [y, x] : p)
            sum += x;
        return sum / p.size();
    };
    std::stable_sort(pieces.begin(), pieces.end(),
                     [&](const auto &a, const auto &b) { return cx(a) > cx(b); });
    J out = arr();
    for (size_t i = 0; i < pieces.size(); ++i) {
        auto p = componentPath(path, width, pieces[i], "damma-" + std::to_string(i + 1), true);
        double score = INFINITY;
        for (const auto &proto : prototypes)
            score = std::min(score, shapeDistance(p, proto));
        if (score > .24)
            return J::array({path});
        p["templateScores"]["damma"] = score;
        out.push_back(p);
    }
    return out;
}
static bool tripleLayout(const J &single, const J &pair) {
    auto a = box(single), b = box(pair);
    if (std::min({a[2], a[3], b[2], b[3]}) <= 0)
        return false;
    if (a[2] / a[3] < .55 || a[2] / a[3] > 1.6 || b[2] / b[3] < 1.2 || b[2] / b[3] > 2.8 ||
        b[2] / a[2] < 1.3 || b[2] / a[2] > 2.8 || b[3] / a[3] < .5 || b[3] / a[3] > 1.8)
        return false;
    double dx = std::abs(a[0] + a[2] / 2 - b[0] - b[2] / 2), dy = b[1] + b[3] / 2 - a[1] - a[3] / 2;
    return dx <= b[2] * .5 && dy >= a[3] * .45 && dy <= a[3] * 2.2;
}
static int tripleParts(const J &paths) {
    if (paths.size() != 2)
        return -1;
    std::vector<int> singles;
    for (int i = 0; i < 2; ++i)
        if (strongType(paths[i]) == "single-dot")
            singles.push_back(i);
    if (singles.size() != 1)
        return -1;
    int single = singles[0], pair = 1 - single;
    auto scores = paths[pair].value("templateScores", obj());
    if (scores.value("twodots", 1.) > .35 && strongType(paths[pair]) != "double-dot")
        return -1;
    return tripleLayout(paths[single], paths[pair]) ? single : -1;
}
static J geometricAssignment(const J &paths, const J &expected, const J &bank, J baseline) {
    if (paths.size() > 14 || expected.size() > 14)
        return {{"assigned", arr()}, {"reason", "assignment-too-large"}};
    int count = paths.size();
    auto score = [&](const std::vector<int> &indices, const J &slot) -> double {
        std::string kind = s(slot, "type"), side = s(slot, "side");
        int cap = n(slot, "capacity");
        J parts = selected(paths, indices);
        if ((kind == "tanwin-fath" || kind == "tanwin-kasr") && !strokePair(parts))
            return INFINITY;
        int physical = 0;
        for (const auto &p : parts)
            physical += std::max(1, n(p, "contourCount"));
        if (physical != cap)
            return INFINITY;
        int triple = kind == "triple-dot" ? tripleParts(parts) : -1;
        if (kind == "triple-dot" && triple < 0)
            return INFINITY;
        if (!baseline.is_null() && !side.empty())
            for (const auto &p : parts) {
                auto b = box(p);
                if (ends(kind, "-dot") && side == "above" &&
                    b[1] + b[3] / 2 > baseline.get<double>() + 2)
                    return INFINITY;
            }
        double cohesion = 0;
        if (parts.size() > 1) {
            double left = INFINITY, right = -INFINITY, top = INFINITY, bottom = -INFINITY;
            for (const auto &p : parts) {
                auto b = box(p);
                left = std::min(left, b[0]);
                right = std::max(right, b[0] + b[2]);
                top = std::min(top, b[1]);
                bottom = std::max(bottom, b[1] + b[3]);
            }
            if (right - left > 90 || bottom - top > 90)
                return INFINITY;
            for (size_t i = 0; i < parts.size(); ++i)
                for (size_t j = i + 1; j < parts.size(); ++j) {
                    auto a = box(parts[i]), b = box(parts[j]);
                    double distance = std::hypot(a[0] + a[2] / 2 - b[0] - b[2] / 2,
                                                 a[1] + a[3] / 2 - b[1] - b[3] / 2),
                           size = std::max({a[2], a[3], b[2], b[3]});
                    if (distance > std::max(14., (kind == "triple-dot" ? 1.4 : 2.4) * size))
                        return INFINITY;
                    cohesion += distance / std::max(1., size);
                }
        }
        double value = 0;
        for (size_t at = 0; at < parts.size(); ++at) {
            const auto &p = parts[at];
            auto strong = strongType(p);
            auto scores = p.value("templateScores", obj());
            if (triple >= 0)
                strong = int(at) == triple ? "single-dot" : "double-dot";
            if (ends(kind, "-dot")) {
                if (strong != "single-dot" && strong != "double-dot")
                    return INFINITY;
                if (kind != "triple-dot" && strong != kind)
                    return INFINITY;
                value += .06;
                if (kind == "triple-dot" && strong == "double-dot")
                    value +=
                        .2 * std::min(scores.value("twodots", 1.),
                                      p.value("dotShapeEvidence", obj()).value("distance", 1.));
            } else if (strong == "single-dot" || strong == "double-dot")
                value += 1.4;
            else if (kind == "tanwin-damm" || kind == "damma")
                value += std::min(1.3, scores.value("damma", 1.));
            else if (bank.contains(kind) && parts.size() == 1) {
                double best = INFINITY;
                for (const auto &proto : bank[kind])
                    best = std::min(best, shapeDistance(p, proto));
                value += best;
            } else
                value += .65;
        }
        value /= std::max<size_t>(1, parts.size());
        if (!baseline.is_null() && !side.empty())
            for (const auto &p : parts) {
                auto b = box(p);
                double cy = b[1] + b[3] / 2,
                       wrong = std::max(0., side == "above" ? cy - baseline.get<double>()
                                                            : baseline.get<double>() - cy);
                value += std::min(1.5, wrong / 18);
            }
        return value + .12 * cohesion;
    };
    struct Option {
        int mask;
        double cost;
        std::vector<int> indices;
    };
    std::vector<std::vector<Option>> options;
    for (const auto &slot : expected) {
        std::vector<Option> choices;
        for (int k = 1; k <= std::min(n(slot, "capacity"), count); ++k)
            for (auto combo : combinations(count, k)) {
                double cost = score(combo, slot);
                if (!std::isfinite(cost))
                    continue;
                int mask = 0;
                for (int i : combo)
                    mask |= 1 << i;
                choices.push_back({mask, cost, combo});
            }
        options.push_back(choices);
    }
    struct Solution {
        double cost;
        std::vector<Option> choices;
    };
    std::map<std::pair<int, int>, Solution> memo;
    std::function<Solution(int, int)> solve = [&](int j, int used) -> Solution {
        if (j == int(expected.size()))
            return {.95 * (count - __builtin_popcount(unsigned(used))), {}};
        auto key = std::make_pair(j, used);
        if (memo.count(key))
            return memo.at(key);
        auto tail = solve(j + 1, used);
        Solution best = tail;
        best.cost += 1.1;
        best.choices.insert(best.choices.begin(), {0, 1.1, {}});
        for (const auto &option : options[j])
            if (!(used & option.mask)) {
                auto proposal = solve(j + 1, used | option.mask);
                proposal.cost += option.cost;
                if (proposal.cost < best.cost) {
                    proposal.choices.insert(proposal.choices.begin(), option);
                    best = proposal;
                }
            }
        return memo[key] = best;
    };
    auto solution = solve(0, 0);
    J assigned = arr();
    int missing = 0;
    for (size_t j = 0; j < solution.choices.size(); ++j) {
        auto o = solution.choices[j];
        if (o.indices.empty())
            ++missing;
        else
            assigned.push_back({{"indices", o.indices}, {"cost", o.cost}, {"slot", expected[j]}});
    }
    return {{"assigned", assigned},
            {"reason", missing ? J(std::to_string(missing) + " expected marks unmatched") : J()}};
}
// Reuse the geometry-only extended kasratan pass after fixed-owner grouping.
static J staggeredGroups(J ink, J candidates, int width, J baseline) {
    if (baseline.is_null())
        return {{"ink", ink}, {"candidates", candidates}, {"count", 0}};
    J strokes = arr(), splits = obj();
    double base = baseline;
    for (const auto &p : ink) {
        auto b = box(p);
        if (b[1] + b[3] / 2 <= base || b[2] < 1.1 * b[3])
            continue;
        auto parts = strokePaths(p, width, true);
        if (parts.size() > 1)
            splits[s(p, "id")] = parts;
        for (const auto &part : parts)
            if (!part.value("stroke", J()).is_null() && part["stroke"][1].get<double>() > base)
                strokes.push_back(part);
    }
    J pairs = arr();
    std::map<std::string, int> uses;
    for (auto combo : combinations(strokes.size(), 2)) {
        auto pair = selected(strokes, combo);
        if (!strokePair(pair) && strokePair(pair, true)) {
            pairs.push_back(pair);
            for (const auto &p : pair)
                ++uses[s(p, "id")];
        }
    }
    J byId = index(ink);
    int count = 0;
    for (const auto &pair : pairs) {
        if (uses[s(pair[0], "id")] != 1 || uses[s(pair[1], "id")] != 1)
            continue;
        ++count;
        std::set<std::string> parents;
        for (const auto &p : pair)
            if (splits.contains(s(p, "sourcePathId")))
                parents.insert(s(p, "sourcePathId"));
        for (const auto &parent : parents) {
            for (const auto &p : splits[parent])
                byId[s(p, "id")] = p;
            for (auto &c : candidates) {
                J list = arr();
                for (const auto &id : c["pathIds"])
                    if (str(id) == parent)
                        append(list, pathIds(splits[parent]));
                    else
                        list.push_back(id);
                c["pathIds"] = list;
            }
        }
        auto members = ids(pathIds(pair));
        for (const auto &p : pair)
            byId[s(p, "id")] = p;
        J affected = arr();
        for (const auto &c : candidates)
            for (const auto &id : c["pathIds"])
                if (members.count(str(id))) {
                    affected.push_back(c);
                    break;
                }
        if (affected.size() == 1 && ids(affected[0]["pathIds"]) == members)
            continue;
        J kept = arr();
        for (auto c : candidates) {
            J remain = arr();
            for (const auto &id : c["pathIds"])
                if (!members.count(str(id)))
                    remain.push_back(id);
            if (!remain.empty()) {
                c["pathIds"] = remain;
                kept.push_back(c);
            }
        }
        kept.push_back({{"pathIds", members}});
        candidates = kept;
    }
    J expanded = arr();
    for (const auto &p : byId)
        expanded.push_back(p);
    return {{"ink", expanded}, {"candidates", candidates}, {"count", count}};
}
J geometricPartition(const J &owner, const J &paths, int width, J baseline, const J &geometry,
                     const J &bank) {
    J ink = arr();
    for (const auto &id : owner["pathIds"]) {
        auto source = paths.at(str(id)), shape = geometry.value(str(id), source);
        source.update({{"templateScores", shape.value("templateScores", obj())},
                       {"signature", shape.value("signature", J("0"))},
                       {"line", 1}});
        ink.push_back(source);
    }
    auto required = counts(owner["expectedSlots"], "capacity");
    if (counts(ink, "contourCount") == required) {
        J groups = arr();
        for (const auto &p : ink)
            groups.push_back(J::array({p}));
        return {{"groups", groups},
                {"evidence", {{"method", "native-packed-groups"}, {"expandedPathIds", arr()}}}};
    }
    if (n(owner, "expectedMarkCount") == 1 &&
        totalContours(ink) == n(owner["expectedSlots"][0], "capacity"))
        return {
            {"groups", J::array({ink})},
            {"evidence", {{"method", "single-mark-contour-quota"}, {"expandedPathIds", arr()}}}};
    bool strokes = false, needsSplit = int(ink.size()) < n(owner, "expectedMarkCount");
    for (const auto &slot : owner["expectedSlots"])
        if (std::set<std::string>{"fatha", "kasra", "tanwin-fath", "tanwin-kasr"}.count(
                s(slot, "type")))
            strokes = true;
    J expanded = arr(), splitIds = arr();
    for (const auto &p : ink) {
        J parts = J::array({p});
        if (n(p, "contourCount") > 1 && strongType(p).empty()) {
            if (needsSplit && bank.contains("damma"))
                parts = splitDammas(p, width, bank["damma"]);
            if (parts.size() == 1 && strokes)
                parts = strokePaths(p, width, true);
        } else if (strokes)
            parts = strokePaths(p, width, false);
        if (parts.size() == 1 && !bank.empty() && n(p, "contourCount") == 2)
            parts = splitDotPairs(parts[0], width, bank);
        if (parts.size() > 1)
            splitIds.push_back(p["id"]);
        for (auto part : parts) {
            if (strongType(part).empty() && !bank.empty()) {
                auto evidence = doubleDotEvidence(part, bank, width);
                if (!evidence.is_null())
                    part["dotShapeEvidence"] = evidence;
            }
            expanded.push_back(part);
        }
    }
    ink = expanded;
    auto assignment = geometricAssignment(ink, owner["expectedSlots"], bank, baseline);
    std::set<int> used;
    J candidates = arr();
    for (const auto &a : assignment["assigned"]) {
        J list = arr();
        for (const auto &index : a["indices"]) {
            int i = index;
            used.insert(i);
            list.push_back(ink[i]["id"]);
        }
        candidates.push_back({{"pathIds", list}});
    }
    for (size_t i = 0; i < ink.size(); ++i)
        if (!used.count(i))
            candidates.push_back({{"pathIds", J::array({ink[i]["id"]})}});
    auto staggered = staggeredGroups(ink, candidates, width, baseline);
    auto byId = index(staggered["ink"]);
    J groups = arr();
    for (const auto &c : staggered["candidates"]) {
        J pieces = arr();
        for (const auto &id : c["pathIds"])
            pieces.push_back(byId.at(str(id)));
        groups.push_back(pieces);
    }
    return {{"groups", groups},
            {"evidence",
             {{"method", "original-vector-geometric-grouping"},
              {"expandedPathIds", splitIds},
              {"staggeredGroups", staggered["count"]},
              {"unmatchedGeometry", assignment["reason"]}}}};
}
J boundaryConflicts(const J &owners, const J &sourcePaths, int width, J baseline, const J &geometry,
                    const J &bank) {
    auto paths = index(sourcePaths);
    J active = arr();
    bool ambiguous = false;
    for (const auto &o : owners)
        if (!o["pathIds"].empty()) {
            active.push_back(o);
            J members = arr();
            for (const auto &id : o["pathIds"])
                members.push_back(paths.at(str(id)));
            if (n(o, "expectedMarkCount") > 1 &&
                counts(members, "contourCount") != counts(o["expectedSlots"], "capacity"))
                ambiguous = true;
        }
    if (active.size() < 2 || !ambiguous)
        return obj();
    J expected = arr();
    int count = 0;
    for (const auto &o : owners) {
        append(expected, o["expectedSlots"]);
        count += n(o, "expectedMarkCount");
    }
    J combined = {{"pathIds", pathIds(sourcePaths)},
                  {"expectedSlots", expected},
                  {"expectedMarkCount", count}};
    auto partition = geometricPartition(combined, paths, width, baseline, geometry, bank);
    std::map<std::string, std::string> ownerOf;
    for (const auto &o : owners)
        for (const auto &id : o["pathIds"])
            ownerOf[str(id)] = s(o, "id");
    J conflicts = obj();
    for (const auto &group : partition["groups"]) {
        std::set<std::string> parents, here;
        for (const auto &p : group) {
            auto id = paths.contains(s(p, "id"))
                          ? s(p, "id")
                          : p.value("sourcePathId", p["id"]).get<std::string>();
            parents.insert(id);
            here.insert(ownerOf.count(id) ? ownerOf[id] : "");
        }
        if (here.size() > 1)
            for (const auto &id : here)
                if (!id.empty()) {
                    if (!conflicts.contains(id))
                        conflicts[id] = arr();
                    conflicts[id].push_back(parents);
                }
    }
    return conflicts;
}
static J legacyGroups(const J &owner, const J &paths, int width, J baseline) {
    J groups = arr(), steps = arr();
    for (const auto &id : owner["pathIds"])
        groups.push_back(
            {{"pathIds", J::array({id})}, {"contours", paths.at(str(id))["contourCount"]}});
    auto required = counts(owner["expectedSlots"], "capacity");
    while (int(groups.size()) > n(owner, "expectedMarkCount")) {
        auto current = counts(groups, "contours");
        std::vector<std::tuple<double, int, int>> options;
        for (size_t i = 0; i < groups.size(); ++i)
            for (size_t j = i + 1; j < groups.size(); ++j) {
                int combined = n(groups[i], "contours") + n(groups[j], "contours");
                bool single = n(owner, "expectedMarkCount") == 1 &&
                              combined <= n(owner["expectedSlots"][0], "capacity");
                int have = current.count(combined) ? current.at(combined) : 0,
                    need = required.count(combined) ? required.at(combined) : 0;
                if (have >= need && !single)
                    continue;
                auto a = clusterBox(groups[i]["pathIds"], paths).get<std::array<double, 4>>(),
                     b = clusterBox(groups[j]["pathIds"], paths).get<std::array<double, 4>>();
                options.emplace_back(std::hypot(a[0] + a[2] / 2 - b[0] - b[2] / 2,
                                                a[1] + a[3] / 2 - b[1] - b[3] / 2),
                                     i, j);
            }
        if (options.empty())
            break;
        auto [distance, i, j] = *std::min_element(options.begin(), options.end());
        J joined = groups[i];
        append(joined["pathIds"], groups[j]["pathIds"]);
        joined["contours"] = n(joined, "contours") + n(groups[j], "contours");
        steps.push_back(merged(
            joined, {{"distance", distance},
                     {"reason",
                      "Nearest contour groups needed to reach the expected semantic mark count"}}));
        groups[i] = joined;
        groups.erase(groups.begin() + j);
    }
    J errors = arr();
    if (counts(groups, "contours") != required)
        errors.push_back("semantic-mark-count-or-contour-grouping-difference");
    std::stable_sort(groups.begin(), groups.end(), [&](const J &a, const J &b) {
        auto aa = clusterBox(a["pathIds"], paths), bb = clusterBox(b["pathIds"], paths);
        return aa[0].get<double>() + aa[2].get<double>() >
               bb[0].get<double>() + bb[2].get<double>();
    });
    J output = arr();
    int at = 0;
    for (const auto &g : groups) {
        J pieces = arr();
        for (const auto &id : g["pathIds"])
            pieces.push_back(paths.at(str(id)));
        auto runs = maskUnion(pieces);
        output.push_back(merged(g, {{"id", s(owner, "id") + ":g" + std::to_string(++at)},
                                    {"ownerId", owner["id"]},
                                    {"runs", runs},
                                    {"bbox", inkBox(runs, width)},
                                    {"baseline", baseline}}));
    }
    return {{"groups", output}, {"groupingSteps", steps}, {"groupingIssues", errors}};
}
J groupOwnedInk(const J &owner, const J &paths, int width, J baseline, const J &geometry,
                const J &bank, const J &conflicts) {
    if (!conflicts.is_null() && !conflicts.empty()) {
        auto retained = legacyGroups(owner, paths, width, baseline);
        retained["geometricGrouping"] = {
            {"method", "existing-owner-boundary-conflict"},
            {"expandedPathIds", arr()},
            {"crossOwnerGroups", conflicts},
            {"reason", "Full subword geometry crosses frozen owners; retain prior grouping until "
                       "ownership is corrected."}};
        return retained;
    }
    auto partition = geometricPartition(owner, paths, width, baseline, geometry, bank);
    auto evidence = partition["evidence"];
    J groups = arr();
    std::map<std::string, int> rank;
    int at = 0;
    for (const auto &id : owner["pathIds"])
        rank[str(id)] = at++;
    for (const auto &pieces : partition["groups"]) {
        std::set<std::string> unique;
        bool split = false;
        for (const auto &p : pieces) {
            unique.insert(rank.count(s(p, "id"))
                              ? s(p, "id")
                              : p.value("sourcePathId", p["id"]).get<std::string>());
            split |= p.contains("sourcePathId");
        }
        std::vector<std::string> parents(unique.begin(), unique.end());
        std::stable_sort(parents.begin(), parents.end(),
                         [&](const auto &a, const auto &b) { return rank.at(a) < rank.at(b); });
        auto runs = maskUnion(pieces);
        J group = {{"pathIds", parents},
                   {"contours", totalContours(pieces)},
                   {"runs", runs},
                   {"bbox", inkBox(runs, width)}};
        if (split) {
            group["sourceParts"] = arr();
            for (const auto &p : pieces)
                group["sourceParts"].push_back({{"id", p["id"]},
                                                {"sourcePathId", p.value("sourcePathId", p["id"])},
                                                {"runs", p["runs"]},
                                                {"bbox", p["bbox"]},
                                                {"contours", p["contourCount"]}});
        }
        groups.push_back(group);
    }
    J steps = arr(), errors = arr();
    for (const auto &g : groups)
        if (g["pathIds"].size() > 1 || g.contains("sourceParts"))
            steps.push_back({{"pathIds", g["pathIds"]},
                             {"contours", g["contours"]},
                             {"reason", evidence["method"]},
                             {"splitSourceInk", g.contains("sourceParts")}});
    if (counts(groups, "contours") != counts(owner["expectedSlots"], "capacity"))
        errors.push_back("semantic-mark-count-or-contour-grouping-difference");
    std::stable_sort(groups.begin(), groups.end(), [](const J &a, const J &b) {
        auto aa = box(a), bb = box(b);
        return aa[0] + aa[2] > bb[0] + bb[2];
    });
    J output = arr();
    at = 0;
    for (auto g : groups) {
        g.update({{"id", s(owner, "id") + ":g" + std::to_string(++at)},
                  {"ownerId", owner["id"]},
                  {"baseline", baseline}});
        output.push_back(g);
    }
    return {{"groups", output},
            {"groupingSteps", steps},
            {"groupingIssues", errors},
            {"geometricGrouping", evidence}};
}
} // namespace NativeMarks
