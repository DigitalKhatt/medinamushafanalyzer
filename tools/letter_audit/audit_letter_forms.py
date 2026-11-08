#!/usr/bin/env python3
"""Report body-form coverage from the segmentation cache used by the native UI."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import unicodedata


def audit(cache):
    config = cache.get("templateConfig", {})
    if config.get("version") != 2 or any(key not in config for key in ("initial", "medial", "final")):
        raise ValueError("Regenerate this page: its cache lacks a version-2 template snapshot.")
    groups = defaultdict(list)
    status_counts = Counter()
    counts = Counter()
    unresolved = []
    for subword in cache["subwords"]:
        # Match PageAnalysisResult::bodyLetters: tatweel has no body slot.
        text = "".join(c for c in subword["text"]
                       if c != "\u0640" and not unicodedata.category(c).startswith("M"))
        connected = len(text) > 1
        counts["subwords"] += 1
        counts["connectedSubwords" if connected else "singleLetterSubwords"] += 1
        counts["completeSubwords"] += bool(subword["complete"])
        counts["completeConnectedSubwords"] += connected and bool(subword["complete"])
        counts["provisionalSubwords"] += bool(subword.get("provisional"))
        counts["expectedRegionCount"] += len(subword["bodies"]) == len(text)
        letters = subword.get("letters", [])
        if len(letters) != len(text):
            raise ValueError("Incomplete per-letter diagnostics; regenerate the page cache.")
        location = {key: subword[key] for key in ("line", "word", "subword", "text")}
        location["id"] = f"page{cache['page']:03d}:l{location['line']}-w{location['word']}-s{location['subword']}"
        missing = []
        for letter in letters:
            status = letter["status"]
            if status not in {"matched", "not-required", "no-template", "no-match", "blocked", "remainder"}:
                raise ValueError(f"Unknown letter status: {status}")
            status_counts[status] += 1
            if status in {"matched", "not-required"}:
                continue
            example = {**location, **letter}
            missing.append(letter)
            key = (status, letter["position"], letter["character"], tuple(letter["candidates"]))
            groups[key].append(example)
        if subword["complete"] and missing:
            raise ValueError(f"Complete subword has unresolved bodies: {location['id']}")
        if not subword["complete"]:
            unresolved.append({**location, "letters": missing, "trace": subword["trace"],
                               "reason": "provisional-remainder" if subword.get("provisional") else
                                         "unresolved-forms" if missing else "cut-conflict"})
    grouped = [{"status": key[0], "position": key[1], "character": key[2],
                "candidates": list(key[3]), "count": len(examples), "examples": examples}
               for key, examples in sorted(groups.items(), key=lambda item: (item[0][0], -len(item[1]), item[0][1:]))]
    return {"page": cache["page"], "templateVersion": 2,
            "templateHash": cache["letterFormsSha256"], "sourceHash": cache["pageDataSha256"],
            "trackingEnabled": config["kashidaDefaults"].get("trackingEnabled", False),
            "summary": dict(counts), "letterStatuses": dict(status_counts),
            "groups": grouped, "unresolvedSubwords": unresolved}


def format_report(report):
    summary = report["summary"]
    lines = [f"Page {report['page']} — body-template coverage",
             f"Tracker enabled: {report['trackingEnabled']}",
             f"Templates: {report['templateHash']}",
             f"Connected subwords complete: {summary['completeConnectedSubwords']}/{summary['connectedSubwords']}",
             f"Connected subwords partial: {summary['connectedSubwords'] - summary['completeConnectedSubwords'] - summary['provisionalSubwords']}",
             f"Provisional remainder partitions: {summary['provisionalSubwords']}",
             f"Single-letter subwords (no cut needed): {summary['singleLetterSubwords']}",
             "Counts below are letter occurrences, not subword counts.",
             "Coverage reports accepted body templates; it is not a visual accuracy measurement."]
    descriptions = {"no-template": "No template selects this character in this position",
                    "no-match": "A template was tried but its body boundary was not accepted",
                    "blocked": "A template exists but an earlier unresolved boundary blocked its search",
                    "remainder": "Assigned from a final-yeh cut; initial form still missing, review required"}
    for status, description in descriptions.items():
        lines.extend(["", f"{status}: {description} ({report['letterStatuses'].get(status, 0)} occurrences)"])
        for group in report["groups"]:
            if group["status"] != status:
                continue
            candidates = ", ".join(group["candidates"]) or "none"
            lines.append(f"  {group['position']} {group['character']}: {group['count']} — templates: {candidates}")
            for example in group["examples"]:
                lines.append(f"    {example['id']} · letter {example['letterIndex']} · {example['text']}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = audit(json.loads(args.cache.read_text()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    args.output.with_suffix(".txt").write_text(format_report(report))
    summary = report["summary"]
    print(f"Page {report['page']}: {summary['completeConnectedSubwords']}/{summary['connectedSubwords']} connected subwords have complete body templates.")
    print(f"{summary['singleLetterSubwords']} single-letter subwords need no cuts. Tracker enabled: {report['trackingEnabled']}")
    for status in ("no-template", "no-match", "blocked", "remainder"):
        print(f"\n{status}:")
        for group in report["groups"]:
            if group["status"] == status:
                print(f"  {group['position']} {group['character']}: {group['count']} — {group['examples'][0]['id']}")


if __name__ == "__main__":
    main()
