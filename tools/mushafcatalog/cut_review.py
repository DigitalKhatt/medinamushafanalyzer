"""Recompute multi-letter cuts and compare them with a frozen catalog."""
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


def review_targets(db, pages, templates, form):
    selectors = None
    if form:
        config = json.loads(templates.read_bytes())
        selectors = {position: set(rule['current']) for position in ('initial', 'medial', 'final')
                     for rule in config[position] if rule['id'] == form}
        require(selectors, f'Unknown template {form}')
    targets = {}
    for pid in pages:
        targets[pid] = set()
        if selectors is None:
            targets[pid].update(row[0] for row in db.execute(
                "SELECT id FROM catalog_subwords WHERE page_id=? AND kind='text' AND letter_count>1", (pid,)))
            continue
        letters = defaultdict(list)
        for sid, char in db.execute("""
            SELECT s.id,o.character FROM catalog_subwords s JOIN catalog_owners o ON o.subword_id=s.id
            WHERE s.page_id=? AND s.kind='text' AND s.letter_count>1 AND o.role='letter'
            ORDER BY s.id,o.word_offset""", (pid,)):
            letters[sid].append(char)
        for sid, chars in letters.items():
            by_position = {'initial': chars[:1], 'medial': chars[1:-1], 'final': chars[-1:]}
            if any(allowed.intersection(by_position[position]) for position, allowed in selectors.items()):
                targets[pid].add(sid)
    return targets


def compare_page(db, pid, folder, targets):
    data = json.loads((folder/(pid+'.json')).read_bytes())
    after = json.loads(zlib.decompress((folder/(pid+'.vectors')).read_bytes()))
    before = json.loads(zlib.decompress(db.execute(
        'SELECT vectors FROM catalog_page_art WHERE page_id=?', (pid,)).fetchone()[0]))
    expected = {r['id']: r for r in db.execute('''SELECT id,text,letter_count,body_key
        FROM catalog_subwords WHERE page_id=? AND kind='text' AND letter_count>1''', (pid,)) if r['id'] in targets}
    data['results'] = {sid:r for sid,r in data['results'].items() if r['status']!='single-letter'}
    require(data['results'].keys() == expected.keys(), f'{pid}: cutter output disagrees with the selected subwords')
    owners = defaultdict(list)
    for row in db.execute('''SELECT o.id,o.subword_id,o.character,o.form FROM catalog_owners o
            JOIN catalog_subwords s ON s.id=o.subword_id WHERE s.page_id=? AND o.role='letter'
            ORDER BY o.word_offset''', (pid,)):
        owners[row['subword_id']].append(dict(row))
    prior_labels = dict(db.execute("""SELECT p.owner_id,p.variant FROM catalog_parts p
        JOIN catalog_subwords s ON s.id=p.subword_id WHERE s.page_id=? AND p.role='body'""", (pid,)))
    changes, label_changes, issues = [], [], []
    for sid, result in data['results'].items():
        spec = expected[sid]
        if data['keys'][sid] != spec['body_key']:
            issues.append(dict(id=sid, issue='source-key-changed'))
        complete = result['status'] == 'template'
        if not complete:
            issues.append(dict(id=sid, issue='unresolved', status=result['status']))
        elif sum(p['role']=='body' for p in result.get('parts', [])) != spec['letter_count']:
            issues.append(dict(id=sid, issue='letter-count'))
        keys = [o['id']+'/'+role for o in owners[sid] for role in ('body','kashida')]
        a = {k:before['parts'][k] for k in keys if k in before['parts']}
        b = {k:after['parts'][k] for k in keys if k in after['parts']}
        altered = {k:[f for f in FIELDS if a.get(k,{}).get(f)!=b.get(k,{}).get(f)]
                   for k in a.keys()|b.keys()}
        altered = {k:v for k,v in altered.items() if v or (k in a)!=(k in b)}
        if complete:
            labels = [{'owner': p['owner'], 'before': prior_labels[p['owner']], 'after': p.get('variant')}
                      for p in result['parts'] if p['role']=='body'
                      and p['owner'] in prior_labels and prior_labels[p['owner']] != p.get('variant')]
            if labels:
                label_changes.append(dict(id=sid, geometryChanged=bool(altered), labels=labels))
        if not altered:
            continue
        if complete:
            try:
                partition_rows(db, dict(version=VERSION, subword=sid, sourceKey=data['keys'][sid],
                                        producer='cut-review', parts=result.get('parts', [])))
            except ValueError as error:
                issues.append(dict(id=sid, issue='invalid-partition', error=str(error)))
        changes.append(dict(id=sid, text=spec['text'], status=result['status'], changedParts=altered,
                            owners=owners[sid], before=a, after=b))
    return changes, label_changes, issues


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
            if side == 'after' and change['status'] != 'template':
                panels.append('<section><h3>After</h3><p>Unresolved</p></section>')
                continue
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
    search = report.get('candidateSearch', {})
    forms = search.get('forms', {})
    candidates = ''
    if forms:
        rows = ''.join('<tr><td>'+escape(form)+'</td>'+''.join(
            f'<td>{counts.get(key,0):,}</td>' for key in ('requests','builds','reused','cutPairs','partitionChecks'))+'</tr>'
            for form,counts in sorted(forms.items(),key=lambda item:item[1].get('requests',0),reverse=True))
        candidates = ('<details><summary>Candidate search by template</summary><p>Requests include recovery retries. '
            'Built lists contain the same ordered candidates as a fresh search; reused lists skip repeated generation. '
            'Partition checks are local geometry calls, not distinct complete subword segmentations.</p>'
            '<table><thead><tr><th>Template</th><th>Requests</th><th>Built</th><th>Reused</th>'
            '<th>Generated cut pairs</th><th>Partition checks</th></tr></thead><tbody>'+rows+'</tbody></table></details>')
    html = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cutter changes</title><style>body{font:15px system-ui;margin:24px;background:#f3f5f7;color:#20323e}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(440px,1fr));gap:12px}article{background:white;padding:14px;border:1px solid #d2dae0;border-radius:8px}
h2{font-size:16px}h3{font-size:13px;margin:4px}.pair{display:grid;grid-template-columns:1fr 1fr;gap:16px}svg{width:100%;height:190px}
pre{white-space:pre-wrap}input{padding:10px;width:300px}table{border-collapse:collapse}th,td{text-align:left;padding:6px 12px;border-bottom:1px solid #d2dae0}@media(max-width:500px){.grid{grid-template-columns:1fr}}</style>
<h1>Cutter changes</h1><p>__SUMMARY__</p><p>Teal: changed letter bodies · gray: unchanged letters · gold: kashidas · pink: cut endpoints. Click an image to enlarge.</p>
<input placeholder="Filter by subword ID" oninput="document.querySelectorAll('article').forEach(e=>e.hidden=!e.dataset.id.includes(this.value))">
__CANDIDATES__<details><summary>Verification and timing</summary><pre>__JSON__</pre></details><main class="grid">__CARDS__</main></html>'''
    unresolved = sum(issue['issue']=='unresolved' for issue in report['issues'])
    changed = sum(change['status']=='template' for change in report['changes'])
    failures = len(report['issues']) - unresolved
    (folder/'index.html').write_text(html.replace('__SUMMARY__',escape(
        f"{report['pages']} pages · {report['subwords']:,} multi-letter subwords · {changed} changed complete cuts · "
        f"{unresolved} unresolved · {failures} validation issues"))
        .replace('__JSON__',escape(json.dumps(summary,ensure_ascii=False,indent=2)))
        .replace('__CANDIDATES__',candidates)
        .replace('__CARDS__',''.join(cards)))


def review(database, destination, pages, workers, templates, *, compare_anchors=False, form=None):
    started = time.perf_counter()
    folder = destination.resolve()
    require(not folder.exists() or not any(folder.iterdir()), 'Choose an empty review directory')
    folder.mkdir(parents=True, exist_ok=True)
    (folder/'pages').mkdir()
    shutil.copy2(templates, folder/'letter-forms.json')
    # Freeze the source of comparison while the viewer remains usable.
    with closing(read(database)) as source, closing(sqlite3.connect(folder/'reference.sqlite')) as target:
        source.backup(target)
    binary = folder/'cutter'
    shutil.copy2(native_binary(), binary)
    with closing(read(folder/'reference.sqlite')) as db:
        targets = review_targets(db, selected_pages(db, pages), folder/'letter-forms.json', form)
    chosen = [pid for pid, ids in targets.items() if ids]
    require(chosen, 'No multi-letter subwords match the review selection')
    counts, stages, python, anchors = Counter(), Counter(), Counter(), Counter()
    candidate_totals, candidate_forms = Counter(), defaultdict(Counter)
    changes, label_changes, issues, slowest = [], [], [], []
    revisions, cache_hits = set(), 0
    cut_started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=workers) as pool, (folder/'cutter.log').open('w') as log:
        jobs = {pool.submit(prepare_page,folder/'reference.sqlite',pid,binary,folder/'pages',
                            targets[pid],folder/'letter-forms.json',profile=True,compare_anchors=compare_anchors):pid for pid in chosen}
        for n,job in enumerate(as_completed(jobs),1):
            pid = jobs[job]
            revisions.add(job.result())
            data = json.loads((folder/'pages'/(pid+'.json')).read_bytes())
            counts.update(log_page(log,pid,data['results']))
            timing = json.loads((folder/'pages'/(pid+'.profile.json')).read_bytes())
            stages.update(timing['timingMs']); python.update(timing['pythonSeconds'])
            cache_hits += timing.get('inputCacheHit',False)
            slowest.extend(timing['subwords'])
            for subword in timing['subwords']:
                search = subword.get('search', {})
                candidate_totals.update({k:v for k,v in search.items() if k not in ('forms','anchorSearch')})
                if status := search.get('anchorSearch'):
                    anchors[status] += 1
                    if status == 'different-cuts':
                        issues.append(dict(id=subword['id'], issue='anchor-search-difference'))
                for form, statistics in search.get('forms', {}).items():
                    candidate_forms[form].update(statistics)
            if n%25==0 or n==len(chosen):
                print(f'{n}/{len(chosen)} pages; {time.perf_counter()-cut_started:.1f}s',flush=True)
                log.flush()
    cut_seconds = time.perf_counter()-cut_started
    require(len(revisions)==1, 'Mixed cutter revisions')
    with closing(read(folder/'reference.sqlite')) as db:
        for pid in chosen:
            altered, labels, errors = compare_page(db,pid,folder/'pages',targets[pid])
            changes.extend(altered); label_changes.extend(labels); issues.extend(errors)
    (folder/'label-changes.json').write_text(encoded(label_changes))
    differences = dict(bodyOutlines=sum(any(k.endswith('/body') and 'd' in fields
                           for k, fields in c['changedParts'].items()) for c in changes),
                       rasterMasks=sum(any('ink' in fields for fields in c['changedParts'].values()) for c in changes),
                       connectorCaps=sum(any('caps' in fields for fields in c['changedParts'].values()) for c in changes),
                       connectorCounts=sum(sum(k.endswith('/kashida') for k in c['before']) !=
                                           sum(k.endswith('/kashida') for k in c['after']) for c in changes),
                       templateLabels=len(label_changes),
                       labelsOnly=sum(not c['geometryChanged'] for c in label_changes))
    report = dict(pages=len(chosen), subwords=sum(counts.values()), statusCounts=counts,
                  anchorSearch=dict(sorted(anchors.items())),
                  changes=changes, issues=issues, inputCacheHits=cache_hits, form=form, differences=differences,
                  cutSeconds=cut_seconds, elapsedSeconds=time.perf_counter()-started,
                  stageSeconds={k:v/1000 for k,v in stages.items()}, pythonSeconds=python,
                  candidateSearch=dict(candidate_totals,forms=dict(candidate_forms)),
                  timingMeaning='cutSeconds and elapsedSeconds are wall time; stageSeconds and pythonSeconds accumulate across workers.',
                  slowestSubwords=sorted(slowest,key=lambda s:s['milliseconds'],reverse=True)[:20],
                  binarySha256=sha(binary.read_bytes()), templatesSha256=sha((folder/'letter-forms.json').read_bytes()),
                  revision=next(iter(revisions)),
                  comparison='Selected multi-letter subwords recomputed and compared with catalog outlines, fill rules, connector caps, raster hashes and template labels.')
    (folder/'comparison.json').write_text(encoded(report))
    render_report(folder,report)
    return dict(report=str(folder/'index.html'),pages=len(chosen),subwords=report['subwords'],
                changed=len(changes),differences=differences,issues=issues,anchorSearch=report['anchorSearch'],
                cutSeconds=cut_seconds,elapsedSeconds=report['elapsedSeconds'])
