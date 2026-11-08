"""Run the native letter cutter and publish its partitions to a catalog."""
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
from tempfile import TemporaryDirectory
import zlib

from store import (HERE, META, ROOT, VECTOR_STORAGE, VERSION, attach_record, encoded, merged,
                   meta, pack_runs, read, report_ink, require, sha, unpack_runs)

NATIVE = 'medina-native-letters/'


def record_kashida_spans(parts, owner_ids):
    """Index the native cut-to-cut horizontal extension in catalog raster pixels."""
    bodies = {part['owner']: part for part in parts if part['role'] == 'body'}
    joins = {(part['owner'], part['nextOwner']): part for part in parts if part['role'] == 'kashida'}
    for previous, following in zip(owner_ids, owner_ids[1:]):
        join = joins.pop((previous, following), None)
        # A shared cut has zero extension. Subword edges have no adjoining
        # letter and remain NULL. Unresolved partitions never reach this step.
        span = 0.0
        if join is not None:
            caps = join.get('caps', [])
            require(len(caps) == 2 and all(len(cap) == 2 for cap in caps) and
                    all(len(point) == 2 and all(math.isfinite(v) for v in point) for cap in caps for point in cap),
                    f'{previous}: invalid native kashida end cuts')
            right, left = ((cap[0][0] + cap[1][0]) / 2 for cap in caps)
            span = abs(right - left)
        bodies[previous]['metrics']['kashidaLeft'] = span
        bodies[following]['metrics']['kashidaRight'] = span
    require(not joins, 'Native kashida joins nonadjacent letter owners')


def selected_pages(db, selection):
    indexed = {r[0] for r in db.execute('SELECT page_id FROM catalog_pages')}
    if selection == 'all':
        return sorted(indexed)
    result = set()
    for part in selection.split(','):
        first, dash, last = part.strip().partition('-')
        lo, hi = int(first), int(last if dash else first)
        require(1 <= lo <= hi <= 604, 'Choose pages from 1 to 604')
        result.update(f'page{n:03}' for n in range(lo, hi+1))
    require(result <= indexed, 'Pages missing from this catalog: ' + ', '.join(sorted(result-indexed)))
    return sorted(result)


def prepare_page(database, pid, binary, work, selected=None):
    with closing(read(database)) as db:
        db.execute('BEGIN')
        compact = meta(db,VECTOR_STORAGE)
        bodies, group_ink, _ = report_ink(db,pid) if compact else ({},{},{})
        page = db.execute('''SELECT report_sha, pdf_sha, source_sha, raster, pdf_to_raster, source_kind
            FROM catalog_pages JOIN catalog_coordinates USING(page_id) JOIN pages USING(page_id)
            WHERE page_id=?''', (pid,)).fetchone()
        require(page['source_kind']=='native-pdf', f'{pid}: build the catalog from an AI/PDF report first')
        owners = defaultdict(list)
        for row in db.execute('''SELECT o.subword_id,o.id,o.character FROM catalog_subwords s
            JOIN catalog_owners o ON o.subword_id=s.id WHERE s.page_id=? AND o.role='letter'
            ORDER BY s.seq,o.word_offset''', (pid,)):
            owners[row[0]].append((row[1],row[2]))
        requested, keys, results, singles = {}, {}, {}, {}
        for sid, members, path, key, blob in db.execute('''SELECT id,members,body_path,body_key,runs
                FROM catalog_subwords WHERE page_id=? AND kind='text' ORDER BY seq''',(pid,)):
            if selected is not None and sid not in selected:
                continue
            keys[sid] = key
            ids, letters = zip(*owners[sid])
            runs = bodies.get(sid,[]) if blob==b'' else unpack_runs(blob) if blob else []
            if not key or not runs:
                results[sid] = dict(status='missing-source')
            elif len(ids)==1:
                singles[path] = sid
                results[sid] = dict(status='single-letter', parts=[dict(owner=ids[0], role='body',
                    runs=runs, metrics=dict(quality='single-letter'))])
            else:
                native = json.loads(members)[0]
                require(native not in requested, f'{sid}: duplicate native subword {native}')
                requested[native] = dict(id=sid, bodyPath=path, owners=ids, letters=''.join(letters), runs=runs)
        if selected is not None:
            require(keys.keys() == selected and not singles, f'{pid}: choose existing multi-letter text subwords')
        marks = [] if selected is not None else db.execute(
            'SELECT id,path_ids,runs FROM native_marks WHERE page_id=? AND runs IS NOT NULL', (pid,)).fetchall()
    request = dict(page=int(pid[4:]), pdfSha256=page['pdf_sha'], raster=json.loads(page['raster']),
                   pdfToRaster=json.loads(page['pdf_to_raster']), subwords=requested,
                   marks=[dict(id=mark,paths=json.loads(ids or '[]'),runs=group_ink[mark] if blob==b'' else unpack_runs(blob))
                          for mark,ids,blob in marks])
    if selected is not None:
        request['partsOnly'] = True
    request_file, output = work/(pid+'.request'), work/(pid+'.cuts')
    request_file.write_text(encoded(request))
    command = [str(binary),'--segment-catalog-page',str(request_file),'--catalog-output',str(output)]
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    run = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
    require(run.returncode==0, f'{pid}: native cutter failed ({run.returncode}): '+(run.stderr or run.stdout)[-2000:])
    reply = json.loads(output.read_bytes())
    require(set(reply['results'])=={r['id'] for r in requested.values()}, f'{pid}: incomplete cutter output')
    results.update(reply['results'])
    paths = reply['paths']
    source_masks = {name: outline.pop('runs') for name,outline in paths.items()}
    for path, sid in singles.items():
        raw = paths.get(path, {})
        runs = source_masks.get(path, [])
        part = results[sid]['parts'][0]
        if runs and merged([runs]) == merged([part['runs']]):
            part['outline'] = dict(raw)
    outlines = {}
    for sid, result in results.items():
        if result['status'] in ('template','provisional'):
            record_kashida_spans(result['parts'], [owner for owner, _ in owners[sid]])
        for part in result.get('parts', []):
            if 'outline' in part:
                outline = part.pop('outline')
                outline['ink'] = sha(pack_runs(part['runs']))
                if 'caps' in part:
                    outline['caps'] = part.pop('caps')
                outlines[part['owner']+'/'+part['role']] = outline
    vectors = dict(paths=paths,parts=outlines,rasterMarks=reply['unresolvedMarks'])
    if selected is None:
        vectors['pageArt'] = reply['pageArt']
    (work/(pid+'.vectors')).write_bytes(zlib.compress(encoded(vectors).encode(),1))
    revision = f"{reply['version']}/{reply['templateSha256']}/{reply['fontSha256']}"
    (work/(pid+'.json')).write_text(encoded(dict(report=page['report_sha'], source=page['source_sha'], keys=keys, results=results)))
    request_file.unlink()
    output.unlink()
    return revision


def publish_page(db, pid, data, vectors, producer, counts, *, partial=False):
    current = db.execute('SELECT report_sha,source_sha FROM catalog_pages JOIN pages USING(page_id) WHERE page_id=?',
                         (pid,)).fetchone()
    require(current and tuple(current)==(data['report'],data['source']), f'{pid}: catalog changed during segmentation; retry')
    existing = dict(db.execute('''SELECT c.subword_id,c.producer FROM catalog_splits c JOIN catalog_subwords s
                    ON s.id=c.subword_id WHERE s.page_id=?''',(pid,)))
    generated = json.loads(zlib.decompress(vectors))
    if meta(db,VECTOR_STORAGE):
        require(not generated['rasterMarks'], f'{pid}: cutter could not verify all mark vectors')
        require(all(result['status'] in ('template','provisional','single-letter')
                    for result in data['results'].values()), f'{pid}: cutter left a body unresolved')
    if partial:
        saved = db.execute('SELECT vectors FROM catalog_page_art WHERE page_id=? AND source_sha=? AND report_sha=?',
                           (pid,data['source'],data['report'])).fetchone()
        require(saved, f'{pid}: publish the page artwork before updating individual cuts')
        page_vectors = json.loads(zlib.decompress(saved[0]))
        updates = generated['parts']
        for sid in data['results']:
            if sid in existing and not existing[sid].startswith(NATIVE):
                continue
            for (owner,) in db.execute("SELECT id FROM catalog_owners WHERE subword_id=? AND role='letter'", (sid,)):
                for role in ('body','kashida'):
                    key = owner+'/'+role
                    if key in updates:
                        page_vectors['parts'][key] = updates[key]
                    else:
                        page_vectors['parts'].pop(key,None)
        db.execute('UPDATE catalog_page_art SET vectors=? WHERE page_id=?',
                   (zlib.compress(encoded(page_vectors).encode(),1),pid))
    else:
        require(generated.get('pageArt'), f'{pid}: cutter did not export native page artwork')
        db.execute('INSERT OR REPLACE INTO catalog_page_art VALUES (?,?,?,?)',(pid,data['source'],data['report'],vectors))
    for sid, result in data['results'].items():
        old = existing.get(sid)
        if old is not None and not old.startswith(NATIVE):
            counts['preserved'] += 1
            continue
        status = result['status']
        if status in ('template','provisional','single-letter'):
            attach_record(db, dict(version=VERSION, subword=sid, sourceKey=data['keys'][sid],
                                   producer=producer, parts=result['parts']),vector_parts=generated['parts'])
            counts[status] += 1
        else:
            if old is not None:
                db.execute('DELETE FROM catalog_splits WHERE subword_id=?',(sid,))
            counts['pending/'+status] += 1


def segment(database, selection, workers, subwords=None):
    binary = Path(os.environ.get('MEDINA_ANALYZER_BINARY',ROOT/'build/ninja-nmc/Release/medinamushafanalyzer')).resolve()
    require(binary.is_file(), 'Compile the native segmenter first')
    binary_hash = sha(binary.read_bytes())
    with closing(read(database)) as db:
        require(meta(db,META), 'Build the native catalog first')
        pages = selected_pages(db, selection)
    targets = defaultdict(set)
    if subwords is not None:
        for sid in subwords:
            targets[sid.partition(':')[0]].add(sid)
        require(targets and targets.keys() <= set(pages), 'Choose subwords on the selected catalog pages')
        pages = sorted(targets)
    require(pages, 'No catalog pages selected')
    counts = Counter()
    with TemporaryDirectory(prefix='mushaf-catalog-letters-') as folder:
        work = Path(folder)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            revisions = set()
            for i, revision in enumerate(pool.map(lambda p: prepare_page(database,p,binary,work,targets.get(p)),pages),1):
                revisions.add(revision)
                print(f'Cut {i}/{len(pages)} pages ({pages[i-1]})',file=sys.stderr,flush=True)
        require(len(revisions)==1 and sha(binary.read_bytes())==binary_hash, 'Cutter or templates changed during segmentation; retry')
        producer = NATIVE + revisions.pop() + '/' + binary_hash + '/cut-span-x-v1'
        with closing(sqlite3.connect(database.resolve().as_uri()+'?mode=rw',uri=True,timeout=120)) as db, db:
            db.execute('PRAGMA foreign_keys=ON')
            db.execute('BEGIN IMMEDIATE')
            db.execute((HERE/'page_art.sql').read_text())
            for pid in pages:
                publish_page(db,pid,json.loads((work/(pid+'.json')).read_bytes()),
                             (work/(pid+'.vectors')).read_bytes(),producer,counts,partial=bool(targets))
    return dict(database=str(database.resolve()),pages=len(pages),subwords=dict(sorted(counts.items())))
