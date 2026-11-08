"""Build one-method letter corpora from corrected subwords and original ink."""
from workbench_paths import NATIVE
import argparse
import importlib.metadata
from collections import Counter, defaultdict
from contextlib import closing
import json
from pathlib import Path
import re
import sqlite3

import numpy as np

from automatic.common import digest_file, pack_runs, plain, unpack_runs
from automatic.native_transcript import export_transcript, subword_characters
from .alignment import BODY_LETTERS, align_bodies, assign_marks, expected_slots, letter_records
from .geometry import bounds, crop, encode_pixels, fingerprint, mask_runs, pixels
from .inference import Predictor, prepare_bundle
from .mark_transcript import associate

ROOT = Path(__file__).resolve().parents[1]
BODY = {'body', 'kashida', 'subword-ink'}


def page_ids(spec):
    result = set()
    for token in spec.split(','):
        if token == 'all': return [f'page{p:03}' for p in range(1, 605)]
        m = re.fullmatch(r'(\d+)(?:-(\d+))?', token.strip())
        if not m: raise ValueError('Use page numbers, ranges, or all')
        a, b = int(m[1]), int(m[2] or m[1])
        if not 1 <= a <= b <= 604: raise ValueError('Page range must lie in 1–604')
        result.update(f'page{p:03}' for p in range(a, b+1))
    return sorted(result)


def page_snapshot(data, native, pid, transcript, provenance):
    with closing(sqlite3.connect((data/'automatic/corpus.sqlite').resolve().as_uri()+'?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        db.execute('BEGIN')
        page = dict(db.execute('SELECT * FROM pages WHERE page_id=?', (pid,)).fetchone())
        words = [dict(r) for r in db.execute('SELECT * FROM words WHERE page_id=? AND in_scope=1 ORDER BY line,ordinal', (pid,))]
        subs = [dict(r) for r in db.execute('SELECT * FROM subwords WHERE page_id=? AND in_scope=1', (pid,))]
        columns = ('id,word_id,subword_id,line,x,y,w,h,area,runs,origin,human_reviewed,'
                   'kind,mark_type,teacher_kind,teacher_type')
        items = [dict(r) for r in db.execute(f'SELECT {columns} FROM items WHERE page_id=? AND in_scope=1 ORDER BY id', (pid,))]
        for item in items: item['runs'] = unpack_runs(item['runs'])
    corpus = json.loads((data/'corpus.json').read_text())
    info = next(p for p in corpus['pages'] if p['id'] == pid)
    if digest_file(Path(corpus['sourceDir'])/info['filename']) != page['source_sha']:
        raise ValueError(f'{pid}: source artwork changed')
    raw = json.loads((native/'output/vector-review'/f'{pid}.raw.json').read_text())
    if raw['sourceSha256'] != page['source_sha'] or (raw['raster']['width'], raw['raster']['height']) != (page['width'], page['height']):
        raise ValueError(f'{pid}: coordinate/source provenance differs')
    return dict(page=page, words=words, subwords=subs, items=items, transcript=transcript[pid],
        provenance=dict(transcript=provenance, source=info, raster=raw['raster'],
            pdfToRaster=raw['provenance']['pdfToRaster'], sourceCorpus=str((data/'automatic/corpus.sqlite').resolve()),
            inputPolicy='Native region membership and saved human role overrides. '
                        'No learned mark classifications, scores or predictions are read.'))


def source_roles(snapshot):
    """Use existing native roles, repairing a displaced alif stem geometrically.

    Some native caches put hamza-below first and leave the tall alif stem
    unknown. For a single alif base, its uniquely tall vertical contour is the
    body; detached contours are marks. This uses no learned type or score.
    """
    roles = {i['id']:i['kind'] if i['human_reviewed'] else i['teacher_kind'] for i in snapshot['items']}
    groups = defaultdict(list)
    for item in snapshot['items']: groups[item['subword_id']].append(item)
    adjustments = []
    for sub in snapshot['subwords']:
        bases = ''.join(c for c in sub['text'] if c in BODY_LETTERS)
        if len(bases)!=1 or bases not in 'اأإآٱ': continue
        items = groups[sub['id']]
        if any(i['human_reviewed'] for i in items): continue
        stems = [i for i in items if i['h'] >= 3*max(1,i['w'])]
        if len(stems)!=1: continue
        stem = stems[0]
        if any(i['h'] > stem['h']/1.8 for i in items if i['id']!=stem['id']): continue
        for item in items:
            before = roles[item['id']]
            after = 'subword-ink' if item is stem else 'mark' if before in BODY|{'unknown'} else before
            if before!=after:
                roles[item['id']]=after
                adjustments.append(dict(id=item['id'],before=before,after=after,reason='unique-alif-stem-geometry'))
    return roles, adjustments


def insert(db, table, records):
    if not records: return
    keys = list(records[0])
    db.executemany(f"INSERT INTO {table} ({','.join(keys)}) VALUES ({','.join('?' for _ in keys)})",
                   [[row[k] for k in keys] for row in records])


def region_record(sub, mask, raw, role, number, box, width):
    runs, raw_runs = mask_runs(mask, box, width), mask_runs(raw, box, width)
    x, y, w, h = bounds(runs, width)
    return dict(id=f"{sub['id']}-region{number}", subword_id=sub['id'], page_id=sub['page_id'],
        role=role, letter_id=None, x=x, y=y, w=w, h=h, area=int(mask.sum()),
        runs=pack_runs(runs), raw_runs=pack_runs(raw_runs), raw_area=int(raw.sum()), mask_sha=fingerprint(runs))


def build_page(snapshot, predictor):
    page, pid = snapshot['page'], snapshot['page']['page_id']
    width, height = page['width'], page['height']
    source, marks_by_sub, body_by_sub = [], defaultdict(list), defaultdict(list)
    roles, role_adjustments = source_roles(snapshot)
    all_pixels = set()
    for item in snapshot['items']:
        pp = pixels(item['runs'])
        if len(pp) != item['area'] or np.any(pp < 0) or np.any(pp >= width*height):
            raise ValueError(f"Invalid source item: {item['id']}")
        if all_pixels.intersection(pp.tolist()): raise ValueError('Overlapping source items')
        all_pixels.update(pp.tolist())
        # A visual mark classifier is not allowed to choose the body's pixels.
        kind = roles[item['id']]
        mark_type = item['mark_type'] if item['human_reviewed'] and kind in ('dot','mark','symbol') else None
        src = dict(id=item['id'], page_id=pid, word_id=item['word_id'], subword_id=item['subword_id'],
            kind=kind, type=mark_type, x=item['x'], y=item['y'], w=item['w'], h=item['h'],
            area=item['area'], runs=pack_runs(item['runs']), metadata=json.dumps({k: item[k] for k in
                ('origin','human_reviewed','teacher_kind')}))
        source.append(src)
        if kind in BODY: body_by_sub[item['subword_id']].append(item)
        elif kind in ('dot', 'mark', 'symbol'):
            marks_by_sub[item['subword_id']].append(dict(id=item['id'], page_id=pid, word_id=item['word_id'],
                subword_id=item['subword_id'], kind=kind, type=mark_type, x=item['x'], y=item['y'],
                w=item['w'], h=item['h'], type_reviewed=int(bool(item['human_reviewed'])), type_confidence=None))
    children = defaultdict(list)
    for s in snapshot['subwords']: children[s['word_id']].append(s)
    letters, subwords, regions, marks, slots, associations = [], [], [], [], [], []
    processed = set()
    words = []
    for word in snapshot['words']:
        words.append({k: word[k] for k in ('id','page_id','line','ordinal','text','bases','x','y','w','h')})
        subs = sorted(children[word['id']], key=lambda s: int(s['id'].rsplit('-s', 1)[1]))
        native_word = snapshot['transcript'].get(word['id'])
        aligned = subword_characters(native_word, subs) if native_word else {}
        word_offset = word_ordinal = 0
        for sub in subs:
            processed.add(sub['id'])
            chars = aligned.get(sub['id'], [])
            ls, owners = letter_records(sub, chars, word_offset, word_ordinal)
            word_offset += len(sub['text']); word_ordinal += len(ls)
            ss = expected_slots(sub, chars, owners,word_offset-len(sub['text']))
            body = body_by_sub[sub['id']]
            source_runs = encode_pixels([p for b in body for p in pixels(b['runs'])])
            rs, bodies, diagnostic = [], [], {}
            if source_runs:
                ink, box = crop(source_runs, width, height)
                if len(ls) == 1:
                    # A transcript-confirmed single base already has its body.
                    # Preserve it whole, including disconnected contours and
                    # counters. Do not run inference or invent internal joins.
                    labels = ink.astype('uint16')
                    result = dict(instances=labels, raw=labels.copy(),
                                  roles=np.array([0, 1], dtype='uint8'), key=None)
                    diagnostic.update(modelApplied=False, bodyMethod='original-single-letter')
                else:
                    result = predictor.get(ink, box, width)
                    diagnostic.update(modelApplied=True, bodyMethod='mask2former')
                labels, raw, roles = result['instances'], result['raw'], result['roles']
                for number in np.unique(labels):
                    if not number: continue
                    role = 'body' if roles[number] == 1 else 'kashida'
                    mask = labels == number
                    r = region_record(sub, mask, raw == number, role, int(number), box, width)
                    rs.append(r)
                    if role == 'body': bodies.append(dict(r, mask=mask))
                unknown = ink & (labels == 0)
                if unknown.any(): rs.append(region_record(sub, unknown, np.zeros_like(unknown), 'unknown', 0, box, width))
                if sum(r['area'] for r in rs) != len(pixels(source_runs)):
                    raise ValueError('Segmentation did not conserve source body ink')
                diagnostic.update(sourcePixels=int(ink.sum()), rawUnassignedPixels=int((ink & (raw == 0)).sum()),
                    unassignedPixels=int(unknown.sum()), predictionKey=result['key'])
            mapping, method = align_bodies(ls, bodies)
            if mapping and diagnostic.get('bodyMethod') == 'original-single-letter':
                method = 'single-base-source'
            for r in rs: r['letter_id'] = mapping.get(r['id'])
            anchors = {}
            for letter in ls:
                r = next((r for r in rs if r['letter_id'] == letter['id']), None)
                if r:
                    letter.update(status='proposed', model_region_id=r['id'],
                        **{k: r[k] for k in ('x','y','w','h','area')}, raw_coverage=r['raw_area']/r['area'])
                    anchors[letter['id']] = r
            # A connector belongs to the letter on its right. Keep ambiguous or
            # unaligned connectors ownerless; they still remain in the corpus.
            ordered = sorted(anchors.values(), key=lambda r: -r['x']-r['w']/2)
            for r in rs:
                if r['role'] != 'kashida': continue
                center = r['x']+r['w']/2
                pairs = [(a, b) for a, b in zip(ordered, ordered[1:])
                         if b['x']+b['w']/2 < center < a['x']+a['w']/2]
                if len(pairs) == 1: r['letter_id'] = pairs[0][0]['letter_id']
            assignment = assign_marks(marks_by_sub[sub['id']], ss, anchors)
            signature = dict(source=page['source_sha'], subword=sub['id'], text=sub['text'],
                letters=[(l['id'],l['char'],l['form']) for l in ls],
                bodies=[(r['id'], r['mask_sha'], r['letter_id']) for r in rs])
            for m in marks_by_sub[sub['id']]:
                src = next(s for s in source if s['id'] == m['id'])
                mark = {k: v for k, v in m.items() if k not in ('x','y','w','h')}
                mark.update(assignment[m['id']])
                mark['candidates'] = json.dumps(mark['candidates'])
                mark['fingerprint'] = fingerprint(dict(signature, mark=m['id'], type=m['type'],
                    mask=unpack_runs(src['runs'])))
                marks.append(mark)
            associations.extend(associate([m for m in marks if m['subword_id']==sub['id']],ss,anchors,source,width))
            diagnostics = dict(diagnostic, expectedMarks=dict(Counter(s['type'] for s in ss)),
                sourceMarkRoles=dict(Counter(m['kind'] for m in marks_by_sub[sub['id']])),
                expectedMarkCount=len(ss), sourceMarkCount=len(marks_by_sub[sub['id']]),
                unassignedMarkCount=sum(a['status']=='unresolved' for a in assignment.values()),
                transcriptAligned=sub['id'] in aligned,
                unownedConnections=sum(r['role']=='kashida' and not r['letter_id'] for r in rs))
            status = ('unresolved' if len(ls) != len(mapping) or not ls or diagnostic.get('unassignedPixels')
                      else 'proposed')
            subwords.append(dict(id=sub['id'], word_id=sub['word_id'], page_id=pid,
                ordinal=int(sub['id'].rsplit('-s',1)[1]), text=sub['text'], bases=plain(sub['text']),
                **dict(zip(('x','y','w','h'), bounds(source_runs, width) if source_runs else [sub[k] for k in ('x','y','w','h')])),
                expected_letters=len(ls), model_bodies=len(bodies), status=status,
                alignment=method, diagnostics=json.dumps(diagnostics)))
            letters.extend(ls); regions.extend(rs); slots.extend(ss)
    for sid, mm in marks_by_sub.items():
        if sid in processed: continue
        for m in mm:
            mark = {k: v for k, v in m.items() if k not in ('x','y','w','h')}
            mark.update(predicted_letter_id=None, status='unresolved', method='no-subword', margin=None,
                candidates='[]', fingerprint=fingerprint(dict(m, source=page['source_sha'])))
            marks.append(mark)
    if set(body_by_sub) - processed:
        raise ValueError('Body ink without a transcript subword; repair ownership before building')
    page_provenance = dict(snapshot['provenance'], markAssignment='transcript-quota-and-geometry-v1',
        learnedMarkClassifierUsed=False, sourceRoleAdjustments=role_adjustments)
    page_provenance['inputPolicy']='Native region membership and human role overrides; no learned mark labels or scores.'
    return dict(pages=[dict(id=pid, split=page['split'], width=width, height=height, source_sha=page['source_sha'],
                    provenance=json.dumps(page_provenance), fingerprint=fingerprint(snapshot))],
                words=words, subwords=subwords, letters=letters, regions=regions,
                source_items=source, marks=marks, expected_marks=slots,mark_transcripts=associations)


def publish_page(path, records, model):
    with closing(sqlite3.connect(path)) as db:
        db.executescript(Path(__file__).with_name('schema.sql').read_text())
        columns={r[1] for r in db.execute('PRAGMA table_info(expected_marks)')}
        for name,kind in [('character','TEXT'),('source_character','TEXT'),('word_text_offset','INTEGER'),('dot_count','INTEGER')]:
            if name not in columns:db.execute(f'ALTER TABLE expected_marks ADD COLUMN {name} {kind}')
        old = db.execute("SELECT value FROM meta WHERE key='model'").fetchone()
        if old and json.loads(old[0]) != model:
            raise ValueError('A corpus must contain only one model; choose another output directory')
        with db:
            db.execute('DELETE FROM pages WHERE id=?', (records['pages'][0]['id'],))
            for name in ('pages','words','subwords','letters','regions','source_items','marks','expected_marks','mark_transcripts'):
                insert(db, name, records[name])
            db.execute("INSERT OR REPLACE INTO meta VALUES ('model',?)", (json.dumps(model),))
            db.execute("INSERT OR REPLACE INTO meta VALUES ('schemaVersion','1')")
            if db.execute('PRAGMA foreign_key_check').fetchall(): raise ValueError('Broken letter corpus relationships')


def run(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    inputs = root/'inputs'; inputs.mkdir(exist_ok=True)
    if not (root/'model/manifest.json').exists(): prepare_bundle(args.experiment, root)
    predictor = Predictor(root, args.device)
    if not args.from_snapshot:
        transcript, provenance = export_transcript(args.native.resolve(), inputs/'native-transcript.json')
    results = []
    for pid in page_ids(args.pages):
        path = inputs/f'{pid}.json'
        if args.from_snapshot:
            snapshot = json.loads(path.read_text())
        else:
            snapshot = page_snapshot(args.data.resolve(), args.native.resolve(), pid, transcript, provenance)
            path.write_text(json.dumps(snapshot, ensure_ascii=False, separators=(',', ':'))+'\n')
        records = build_page(snapshot, predictor)
        publish_page(root/'corpus.sqlite', records, predictor.settings)
        summary = dict(page=pid, **{k: len(v) for k,v in records.items() if k!='pages'},
            originalSingleLetterBodies=sum(s['alignment']=='single-base-source' for s in records['subwords']),
            segmentedSubwordBodies=sum(json.loads(s['diagnostics']).get('modelApplied',False) for s in records['subwords']),
            unresolvedSubwords=sum(s['status']=='unresolved' for s in records['subwords']),
            unalignedLetters=sum(l['status']=='unresolved' for l in records['letters']),
            proposedMarks=sum(m['status']=='proposed' for m in records['marks']),
            wordSigns=sum(m['status']=='word-sign' for m in records['marks']),
            unresolvedMarks=sum(m['status']=='unresolved' for m in records['marks']),
            transcriptTypedMarks=sum(m['expected_type'] is not None for m in records['mark_transcripts']),
            sourcePixels=sum(i['area'] for i in records['source_items']),
            bodyPixels=sum(r['area'] for r in records['regions']))
        results.append(summary)
        print(json.dumps(summary), flush=True)
    report = dict(method='mask2former', model=predictor.settings, pages=results, device=args.device,
        policy='Machine proposals only. Page 3 is a training page, not an independent accuracy evaluation. '
               'Single-base subwords use their complete source body without model inference or internal cuts. '
               'No template masks are stored. Unknown bodies, identities and attachments remain explicit. '
               'No learned mark classifier is used for body input or mark ownership. '
               'Human mark ownership reviews are stored in a separate database.')
    report['codeSha256'] = {str(p.relative_to(ROOT)): digest_file(p) for p in
                           sorted((ROOT/'letter_corpus').glob('*.py'))}
    report['inputSha256'] = {pid: digest_file(inputs/f'{pid}.json') for pid in page_ids(args.pages)}
    report['runtime'] = {}
    for name in ('numpy','scipy','Pillow','torch','transformers'):
        try: report['runtime'][name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError: report['runtime'][name] = 'not needed for cached inference'
    (root/'build-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pages', default='3')
    p.add_argument('--output', type=Path, default=ROOT/'data/automatic/letter-corpus')
    p.add_argument('--data', type=Path, default=ROOT/'data')
    p.add_argument('--native', type=Path, default=NATIVE)
    p.add_argument('--experiment', type=Path, default=ROOT/'data/automatic/experiments/letter-retest-20261104')
    p.add_argument('--device', choices=['cpu','mps','cuda'], default='cpu')
    p.add_argument('--from-snapshot', action='store_true', help='Reindex saved inputs; reuse identical prediction caches')
    run(p.parse_args())


if __name__ == '__main__': main()
