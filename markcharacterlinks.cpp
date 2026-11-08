#include "markassignment_p.h"

namespace NativeMarks {
namespace {
double prototypeDistance(const J &shape, const J &bank, const std::string &type) {
    const auto it = bank.find(type);
    double best = 1.;
    if (it != bank.end() && shape.contains("signature") && shape["signature"] != "0")
        for (const auto &prototype : *it)
            best = std::min(best, shapeDistance(shape, prototype));
    return best;
}

J groupOutline(const J &group, const J &paths, const J &geometry, int width) {
    J shape = {{"bbox", group["bbox"]}};
    if (group["pathIds"].size() == 1) {
        const auto key = str(group["pathIds"].front());
        const auto &source = paths.at(key);
        if (source["runs"] == group["runs"]) {
            const auto it = geometry.find(key);
            const auto &raw = it == geometry.end() ? source : *it;
            shape["bbox"] = source["bbox"];
            shape.update(subset(raw, {"signature", "templateScores", "dotShapeEvidence"}));
            if (!s(shape, "signature").empty() && s(shape, "signature") != "0") return shape;
        }
    }
    // A packed parent can contain two different vowels. Sample only this group's ink.
    Piece ink;
    const auto &runs = group["runs"];
    for (size_t i = 0; i < runs.size(); i += 2) {
        int start = runs[i], end = start + runs[i+1].get<int>();
        for (int p = start; p < end; ++p) ink.emplace_back(p / width, p % width);
    }
    if (!ink.empty()) shape["signature"] = componentPath(group, width, ink, "outline", true)["signature"];
    return shape;
}

std::string multipartShape(const J &group, const J &bank, int width) {
    const auto parts = connectedInk(group["runs"], width);
    if (parts.size() != 2) return {};
    if (!stroke(parts[0]).is_null() && !stroke(parts[1]).is_null()) return "two-strokes";
    for (const auto &part : parts) {
        const auto shape = componentPath(group, width, part, "outline", true);
        if (prototypeDistance(shape, bank, "double-dot") < .24) return "triple-dot";
    }
    return {};
}

double slotDistance(const J &group, const J &shape, const J &slot, const J &bank,
                    const std::string &multipart, bool oppositeVowels) {
    const auto type = s(slot, "type");
    if (!multipart.empty()) {
        const bool fits = multipart == type ||
            (multipart == "two-strokes" && (type == "tanwin-kasr" || type == "tanwin-fath"));
        return fits ? 0. : 1.;
    }
    const auto strong = strongType(shape);
    if (!strong.empty()) return strong == type ? 0. : 1.;
    double distance = prototypeDistance(shape, bank, type == "hamza-below" ? "hamza-above" : type);
    const auto &scores = shape.value("templateScores", obj());
    if (type == "single-dot") distance = std::min(distance, scores.value("onedot", 1.));
    if (type == "double-dot") distance = std::min(distance, scores.value("twodots", 1.));
    // Fatha and kasra share an outline. Only use the side to distinguish them
    // when this letter actually has both slots and the whole stroke is on one side.
    if (oppositeVowels && group["baseline"].is_number() && (type == "fatha" || type == "kasra")) {
        const double baseline = group["baseline"];
        const auto b = box(shape);
        if ((type == "fatha" && b[1] > baseline) || (type == "kasra" && b[1]+b[3] < baseline)) return 1.;
    }
    return distance;
}

void attach(J &group, const J &slot, const std::string &wordId, const char *method) {
    auto &link = group["transcriptLink"];
    link.update(subset(slot, {"characterOffset", "sourceCharacter", "codePoint"}));
    link.update({{"status", "linked"}, {"method", method}, {"expectedType", slot["type"]},
                 {"characterId", wordId + ":c" + std::to_string(n(slot, "characterOffset"))}});
    if (!slot.at("equivalence").is_null()) link["equivalence"] = slot["equivalence"];
}
} // namespace

void linkTranscriptCharacters(J &groups, const J &owner, const std::string &wordId, const J &paths,
                              const J &geometry, const J &bank, int width) {
    const auto &expected = owner.at("expectedSlots");
    const bool letter = owner["role"] == "letter";
    std::map<int, std::set<size_t>> masksByCount, slotsByCount;
    for (size_t g = 0; g < groups.size(); ++g) {
        groups[g]["transcriptLink"] = {{"baseCharacter", letter ? owner["character"] : J()},
                                       {"baseOffset", letter ? owner["offset"] : J()}};
        masksByCount[n(groups[g], "contours")].insert(g);
    }
    for (size_t t = 0; t < expected.size(); ++t) slotsByCount[n(expected[t], "capacity")].insert(t);
    const bool opposite = owner["expectedInventory"].contains("fatha") && owner["expectedInventory"].contains("kasra");
    for (auto &[contours, masks] : masksByCount) {
        auto &targets = slotsByCount[contours];
        const bool useShape = masks.size() > 1 || targets.size() > 1;
        std::map<std::pair<size_t,size_t>, double> costs;
        if (useShape && !targets.empty()) {
            for (auto g : masks) {
                const auto shape = groupOutline(groups[g], paths, geometry, width);
                const auto multipart = contours == 2 ? multipartShape(groups[g], bank, width) : std::string();
                for (auto t : targets)
                    costs[{g,t}] = slotDistance(groups[g], shape, expected[t], bank, multipart, opposite);
            }
        }
        while (!masks.empty() && !targets.empty()) {
            if (masks.size() == 1 && targets.size() == 1) {
                attach(groups[*masks.begin()], expected[*targets.begin()], wordId,
                       useShape ? "remaining-slot" : "contour-quota");
                masks.clear();
                break;
            }
            // Commit a shape only if both the mask and the transcript slot prefer
            // each other. Keep competing assignments visible when distances tie.
            std::vector<std::pair<size_t,size_t>> choices;
            for (auto g : masks) for (auto t : targets) {
                const double cost = costs.at({g,t});
                if (cost > .30) continue;
                const auto closeMask = [&](size_t h) { return h != g && costs.at({h,t}) < cost + .10; };
                const auto closeSlot = [&](size_t u) { return u != t && costs.at({g,u}) < cost + .10; };
                if (std::none_of(masks.begin(), masks.end(), closeMask) &&
                    std::none_of(targets.begin(), targets.end(), closeSlot)) choices.emplace_back(g,t);
            }
            if (choices.empty()) break;
            for (auto [g,t] : choices) {
                attach(groups[g], expected[t], wordId, "native-geometry");
                masks.erase(g);
                targets.erase(t);
            }
        }
        for (auto g : masks) {
            auto &link = groups[g]["transcriptLink"];
            link["status"] = targets.empty() ? "unmatched" : "ambiguous";
            link["candidateOffsets"] = arr();
            for (auto t : targets) link["candidateOffsets"].push_back(expected[t]["characterOffset"]);
        }
    }
}
} // namespace NativeMarks
