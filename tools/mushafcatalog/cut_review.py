"""Recompute all multi-letter cuts and compare them with a frozen catalog."""
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import closing
from html import escape
import json
from pathlib import Path
import shutil
import sqlite3
import time
import zlib

from page_join import outline_commands
from segment_letters import prepare_page, native_binary, selected_pages, log_page
from store import VERSION, encoded, partition_rows, read, require, sha


FIELDS = ('d', 'caps', 'fillRule', 'ink')


def compare_page(db, pid, folder):
    data = json.loads((folder/(pid+'.json')).read_bytes())
    after = json.loads(zlib.decompress((folder/(pid+'.vectors')).read_bytes()))
    before = json.loads(zlib.decompress(db.execute(
        'SELECT vectors FROM catalog_page_art WHERE page_id=?', (pid,)).fetchone()[0]))
    expected = {r['id']: r for r in db.execute('''SELECT id,text,letter_count,body_key
        FROM catalog_subwords WHERE page_id=? AND kind='text' AND letter_count>1''', (pid,))}
    data['results'] = {sid:r for sid,r in data['results'].items() if r['status']!='single-letter'}
    require(data['results'].keys() == expected.keys(), f'{pid}: not all multi-letter subwords were checked')
    owners = defaultdict(list)
    for row in db.execute('''SELECT o.id,o.subword_id,o.character,o.form FROM catalog_owners o
            JOIN catalog_subwords s ON s.id=o.subword_id WHERE s.page_id=? AND o.role='letter'
            ORDER BY o.word_offset''', (pid,)):
        owners[row['subword_id']].append(dict(row))
    changes, issues = [], []
    for sid, result in data['results'].items():
        spec = expected[sid]
        if data['keys'][sid] != spec['body_key']:
            issues.append(dict(id=sid, issue='source-key-changed'))
        if result['status'] not in ('template', 'provisional'):
            issues.append(dict(id=sid, issue='unresolved', status=result['status']))
        if sum(p['role']=='body' for p in result.get('parts', [])) != spec['letter_count']:
            issues.append(dict(id=sid, issue='letter-count'))
        keys = [o['id']+'/'+role for o in owners[sid] for role in ('body','kashida')]
        a = {k:before['parts'][k] for k in keys if k in before['parts']}
        b = {k:after['parts'][k] for k in keys if k in after['parts']}
        altered = {k:[f for f in FIELDS if a.get(k,{}).get(f)!=b.get(k,{}).get(f)]
                   for k in a.keys()|b.keys()}
        altered = {k:v for k,v in altered.items() if v or (k in a)!=(k in b)}
        if not altered:
            continue
        try:
            partition_rows(db, dict(version=VERSION, subword=sid, sourceKey=data['keys'][sid],
                                    producer='cut-review', parts=result.get('parts', [])))
        except ValueError as error:
            issues.append(dict(id=sid, issue='invalid-partition', error=str(error)))
        changes.append(dict(id=sid, text=spec['text'], status=result['status'], changedParts=altered,
                            owners=owners[sid], before=a, after=b))
    return changes, issues


def render_report(folder, report):
    cards = []
    for change in report['changes']:
        parts = list(change['before'].values()) + list(change['after'].values())
        points = [(x,y) for p in parts for _,xy in outline_commands(p['d'])
                  for x,y in zip(xy[::2],xy[1::2])]
        if not points:
            continue
        x0,y0 = min(x for x,y in points)-8, min(y for x,y in points)-8
        width,height = max(x for x,y in points)+8-x0, max(y for x,y in points)+8-y0
        panels = []
        changed = change['changedParts']
        for side in ('before','after'):
            paths, cuts = [], []
            for key,p in change[side].items():
                color = '#ce9c31' if key.endswith('/kashida') else '#057f91' if key in changed else '#adb8be'
                paths.append(f'<path fill="{color}" fill-rule="{p.get("fillRule","evenodd")}" d="{escape(p["d"])}"/>')
                for a,b in p.get('caps',[]):
                    cuts.append(f'<path fill="none" stroke="#d51a68" stroke-width=".8" d="M{a[0]} {a[1]}L{b[0]} {b[1]}"/>')
            svg = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x0} {y0} {width} {height}">'+''.join(paths+cuts)+'</svg>'
            name = change['id'].replace(':','-')+'-'+side+'.svg'
            (folder/name).write_text(svg)
            panels.append(f'<section><h3>{side.title()}</h3><a href="{name}">{svg}</a></section>')
        sid = escape(change['id'])
        cards.append(f'<article data-id="{sid}"><h2>{sid} · <bdi dir="rtl">{escape(change["text"])}</bdi></h2>'
                     '<div class="pair">'+''.join(panels)+'</div></article>')
    summary = {k:v for k,v in report.items() if k!='changes'}
    html = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cutter changes</title><style>body{font:15px system-ui;margin:24px;background:#f3f5f7;color:#20323e}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(440px,1fr));gap:12px}article{background:white;padding:14px;border:1px solid #d2dae0;border-radius:8px}
h2{font-size:16px}h3{font-size:13px;margin:4px}.pair{display:grid;grid-template-columns:1fr 1fr;gap:16px}svg{width:100%;height:190px}
pre{white-space:pre-wrap}input{padding:10px;width:300px}@media(max-width:500px){.grid{grid-template-columns:1fr}}</style>
<h1>Cutter changes</h1><p>__SUMMARY__</p><p>Teal: changed letter bodies · gray: unchanged letters · gold: kashidas · pink: cut endpoints. Click an image to enlarge.</p>
<input placeholder="Filter by subword ID" oninput="document.querySelectorAll('article').forEach(e=>e.hidden=!e.dataset.id.includes(this.value))">
<details><summary>Verification and timing</summary><pre>__JSON__</pre></details><main class="grid">__CARDS__</main></html>'''
    (folder/'index.html').write_text(html.replace('__SUMMARY__',escape(
        f"{report['pages']} pages · {report['subwords']:,} multi-letter subwords · {len(cards)} changed · {len(report['issues'])} structural issues"))
        .replace('__JSON__',escape(json.dumps(summary,ensure_ascii=False,indent=2)))
        .replace('__CARDS__',''.join(cards)))


def review(database, destination, pages, workers, templates):
    started = time.perf_counter()
    folder = destination.resolve()
    require(not folder.exists() or not any(folder.iterdir()), 'Choose an empty review directory')
    folder.mkdir(parents=True, exist_ok=True)
    (folder/'pages').mkdir()
    # Freeze the source of comparison while the viewer remains usable.
    with closing(read(database)) as source, closing(sqlite3.connect(folder/'reference.sqlite')) as target:
        source.backup(target)
    binary = folder/'cutter'
    shutil.copy2(native_binary(), binary)
    shutil.copy2(templates, folder/'letter-forms.json')
    targets = defaultdict(set)
    with closing(read(folder/'reference.sqlite')) as db:
        chosen = selected_pages(db, pages)
        for sid,pid in db.execute("SELECT id,page_id FROM catalog_subwords WHERE kind='text' AND letter_count>1"):
            if pid in chosen:
                targets[pid].add(sid)
    counts, stages, python = Counter(), Counter(), Counter()
    changes, issues, slowest = [], [], []
    revisions, cache_hits = set(), 0
    cut_started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=workers) as pool, (folder/'cutter.log').open('w') as log:
        jobs = {pool.submit(prepare_page,folder/'reference.sqlite',pid,binary,folder/'pages',
                            targets[pid],folder/'letter-forms.json',profile=True):pid for pid in chosen}
        for n,job in enumerate(as_completed(jobs),1):
            pid = jobs[job]
            revisions.add(job.result())
            data = json.loads((folder/'pages'/(pid+'.json')).read_bytes())
            counts.update(log_page(log,pid,data['results']))
            timing = json.loads((folder/'pages'/(pid+'.profile.json')).read_bytes())
            stages.update(timing['timingMs']); python.update(timing['pythonSeconds'])
            cache_hits += timing.get('inputCacheHit',False)
            slowest.extend(timing['subwords'])
            if n%25==0 or n==len(chosen):
                print(f'{n}/{len(chosen)} pages; {time.perf_counter()-cut_started:.1f}s',flush=True)
                log.flush()
    cut_seconds = time.perf_counter()-cut_started
    require(len(revisions)==1, 'Mixed cutter revisions')
    with closing(read(folder/'reference.sqlite')) as db:
        for pid in chosen:
            altered, errors = compare_page(db,pid,folder/'pages')
            changes.extend(altered); issues.extend(errors)
    report = dict(pages=len(chosen), subwords=sum(counts.values()), statusCounts=counts,
                  changes=changes, issues=issues, inputCacheHits=cache_hits,
                  cutSeconds=cut_seconds, elapsedSeconds=time.perf_counter()-started,
                  stageSeconds={k:v/1000 for k,v in stages.items()}, pythonSeconds=python,
                  timingMeaning='cutSeconds and elapsedSeconds are wall time; stageSeconds and pythonSeconds accumulate across workers.',
                  slowestSubwords=sorted(slowest,key=lambda s:s['milliseconds'],reverse=True)[:20],
                  binarySha256=sha(binary.read_bytes()), templatesSha256=sha((folder/'letter-forms.json').read_bytes()),
                  revision=next(iter(revisions)),
                  comparison='Every multi-letter subword recomputed; exact outlines, fill rules, cut endpoints and raster-mask hashes compared. Single letters and source artwork remain unchanged.')
    (folder/'comparison.json').write_text(encoded(report))
    render_report(folder,report)
    return dict(report=str(folder/'index.html'),pages=len(chosen),subwords=report['subwords'],
                changed=len(changes),issues=issues,cutSeconds=cut_seconds,elapsedSeconds=report['elapsedSeconds'])
