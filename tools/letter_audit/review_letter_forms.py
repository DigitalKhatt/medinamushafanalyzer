#!/usr/bin/env python3
"""Create a standalone visual comparison of two native segmentation caches."""
import argparse
from collections import Counter
from html import escape
import json
from pathlib import Path
import shutil

COLORS = ("#326baf", "#178a5b", "#a64796", "#be5b28", "#7c6d26", "#307b84")


def location(item):
    return item["line"], item["word"], item["subword"]


def identifier(item):
    return "l%d-w%d-s%d" % location(item)


def geometry_changed(before, after):
    def bodies(item):
        return [(body["commands"], body["fillRule"]) for body in item["bodies"]]
    return bodies(before) != bodies(after) or before["joins"] != after["joins"]


def path(data, color):
    commands = " ".join(str(command[0]) + " " + " ".join(f"{n:.8g}" for n in command[1:])
                        for command in data["commands"])
    return f'<path d="{commands}" fill="{color}" fill-rule="{data["fillRule"]}"/>'


def geometry(item, cuts=True):
    parts = [path(body, COLORS[i % len(COLORS)] if len(item["bodies"]) > 1 else "#384452")
             for i, body in enumerate(item["bodies"])]
    for join in item["joins"]:
        if join.get("kashida", {}).get("commands"):
            parts.append(path(join["kashida"], "#f4c863"))
        if cuts:
            for cut in join.get("endpoints", [join["anchor"]]):
                a, b = cut
                parts.append(f'<path d="M {a[0]} {a[1]} L {b[0]} {b[1]}" '
                             'fill="none" stroke="#cf314b" stroke-width=".075"/>')
    return "".join(parts)


def svg(item):
    x, y, w, h = item["bounds"]
    return (f'<svg xmlns="http://www.w3.org/2000/svg" role="img" '
            f'aria-label="{escape(item["text"])}" viewBox="{x-1} {y-1} {w+2} {h+2}">'
            + geometry(item) + "</svg>")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    before = json.loads(args.before.read_text())
    after = json.loads(args.after.read_text())
    if before["page"] != after["page"] or before["pageDataSha256"] != after["pageDataSha256"]:
        raise ValueError("Comparison requires the same page and source data")
    old = {location(item): item for item in before["subwords"]}
    if set(old) != {location(item) for item in after["subwords"]}:
        raise ValueError("Comparison requires the same subword inventory")
    changed = [item for item in after["subwords"] if not old[location(item)]["complete"] or
               geometry_changed(old[location(item)], item) or
               old[location(item)]["complete"] != item["complete"] or
               bool(old[location(item)].get("provisional")) != bool(item.get("provisional"))]
    summary = {
        "page": after["page"], "sourceHash": after["pageDataSha256"],
        "beforeCacheVersion": before["version"], "afterCacheVersion": after["version"],
        "beforeTemplateHash": before["letterFormsSha256"], "afterTemplateHash": after["letterFormsSha256"],
        "trackingEnabled": after["templateConfig"]["kashidaDefaults"]["trackingEnabled"],
        "total": len(after["subwords"]),
        "beforeComplete": sum(item["complete"] for item in before["subwords"]),
        "afterComplete": sum(item["complete"] for item in after["subwords"]),
        "afterProvisional": sum(bool(item.get("provisional")) for item in after["subwords"]),
        "comparedSubwords": len(changed),
        "geometryChanged": sum(geometry_changed(old[location(item)], item) for item in after["subwords"]),
        "resolved": [identifier(item) for item in changed if not old[location(item)]["complete"] and item["complete"]],
        "previousCompleteChanged": [identifier(item) for item in changed if old[location(item)]["complete"] and geometry_changed(old[location(item)], item)],
        "templateUsage": dict(Counter(form for item in after["subwords"] for form in item["templates"])),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "comparison.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    shutil.copyfile(args.before, args.output / "before.json")
    shutil.copyfile(args.after, args.output / "after.json")
    cards = []
    for item in changed:
        previous = old[location(item)]
        label = f'page{after["page"]:03d}:{identifier(item)}'
        panels = []
        for title, value in (("Before", previous), ("After", item)):
            kashidas = sum(bool(join.get("kashida", {}).get("commands")) for join in value["joins"])
            state = "Complete" if value["complete"] else "Provisional — needs review" if value.get("provisional") else "Partial"
            panels.append(f'<div><h3>{title} · {state}</h3>{svg(value)}'
                          f'<p>{len(value["bodies"])} bodies · {kashidas} kashida spans</p></div>')
        trace = escape("\n".join(item["trace"]))
        cards.append(f'<article id="{identifier(item)}"><h2>{label} · <bdi dir="rtl">{escape(item["text"])}</bdi></h2>'
                     f'<div class="pair">{"".join(panels)}</div>'
                     f'<details><summary>Matched templates and trace</summary><pre>{trace}</pre></details></article>')
    bounds = [item["bounds"] for item in after["subwords"]]
    x, y = min(b[0] for b in bounds) - 2, min(b[1] for b in bounds) - 2
    w, h = max(b[0]+b[2] for b in bounds)-x+2, max(b[1]+b[3] for b in bounds)-y+2
    overview = (f'<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="{1200*h/w:.0f}" '
                f'viewBox="{x} {y} {w} {h}"><rect x="{x}" y="{y}" width="{w}" height="{h}" fill="white"/>'
                + "".join(f'<g><title>{escape(identifier(item)+" "+item["text"])}</title>{geometry(item)}</g>'
                          for item in after["subwords"]) + "</svg>")
    (args.output / "page.svg").write_text(overview)
    headline = (f'{len(summary["resolved"])} partial subwords resolved.' if summary["resolved"] else
                f'{summary["geometryChanged"]} subwords with revised cuts; {len(changed)} cases shown for review.')
    html = f'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Page {after["page"]} — segmentation review</title><style>
body{{font:16px system-ui,sans-serif;margin:24px auto;padding:0 20px;max-width:1200px;color:#202b38;background:#f5f7fa}}
header,article{{background:white;padding:24px;border:1px solid #d9e1e8;border-radius:12px;margin-bottom:20px}}
h1{{margin-top:0}}h2{{font-size:18px}}h3,p{{font-size:15px}}p{{line-height:1.6}}a{{color:#25669a}}
.pair{{display:grid;grid-template-columns:1fr 1fr;gap:24px}}svg{{display:block;width:100%;height:240px}}
pre{{white-space:pre-wrap;font:13px/1.6 monospace}}summary{{cursor:pointer;color:#25669a}}input{{padding:12px;width:280px;max-width:90%;font:inherit}}
.legend{{display:inline-block;background:#f4c863;width:14px;height:14px}}article:target{{border:2px solid #25669a}}
@media(max-width:650px){{.pair{{grid-template-columns:1fr}}svg{{height:190px}}}}
</style><header><h1>Page {after["page"]} · segmentation review</h1>
<p><strong>{headline}</strong> Complete subwords: {summary["beforeComplete"]} → {summary["afterComplete"]} of {summary["total"]}.
Previously complete subwords with changed geometry: {len(summary["previousCompleteChanged"])}. Kashida tracker: {"enabled" if summary["trackingEnabled"] else "disabled"}.</p>
<p>Provisional remainder partitions: {summary["afterProvisional"]}. These remain outside template-complete training labels until reviewed or replaced by a body template.</p>
<p>Colors identify successive letter bodies. <span class="legend"></span> Gold identifies connecting ink; red lines show cuts.
These are automatic template matches, ready for visual review.</p>
<p><a href="page.svg">Whole page</a> · <a href="comparison.json">Comparison data</a> · <a href="after.json">Segmented corpus</a></p>
<label>Find a subword <input id="find" placeholder="l8-w8-s1 or مُصْلِحُو"></label></header>
{"".join(cards)}<script>document.querySelector('#find').addEventListener('input',e=>{{const q=e.target.value.trim();
document.querySelectorAll('article').forEach(card=>card.hidden=!card.textContent.includes(q));}});</script></html>'''
    (args.output / "index.html").write_text(html)
    print(json.dumps({key: value for key, value in summary.items() if key != "templateUsage"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
