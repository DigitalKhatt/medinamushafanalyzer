"""Materialize the native corpus and selectively attach complete letter-body partitions."""
from contextlib import closing
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import sqlite3
import sys
from tempfile import TemporaryFile
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VECTOR = ROOT / 'tools/mark-classifier'
REPORT = ROOT / 'output/mark-assignment-character-links-vector'
DB = ROOT / 'output/catalog.sqlite'
sys.path.insert(0, str(VECTOR))
from automatic.common import bounding_box, pack_runs, plain, unpack_runs
from automatic.transcript_marks import backfill_waqf_links, ensure_mark_letter_column, import_page, require
from automatic.word_audit import union_intervals
from letter_classes import shape_class

VERSION = 1
META = 'mushafCatalog'
VECTOR_STORAGE = 'mushafCatalogVectorStorage'
INK_REF = b'V1:'
ALL = [f'page{i:03d}' for i in range(1, 605)]


def encoded(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ink_hash(blob):
    if blob.startswith(INK_REF) and len(blob)==len(INK_REF)+32:
        return blob[len(INK_REF):].hex()
    return sha(blob)


def ink_reference(blob):
    return INK_REF + bytes.fromhex(ink_hash(blob))


@lru_cache(maxsize=4)
def load_report_ink(report, pid, fingerprint):
    data = (Path(report)/f'{pid}.json').read_bytes()
    require(sha(data)==fingerprint, f'{pid}: native validation report changed')
    page = json.loads(data)
    paths = {p['id']:p['runs'] for p in page['paths']}
    bodies = {s['id']:(s.get('compoundBody') or {}).get('runs',paths.get(s.get('bodyPathId'),[]))
              for s in page['subwords']}
    marks = {g['id']:g['runs'] for g in page['groups']}
    groups = {s['id']:s['pathIds'] for s in page['ignoredSignGroups']}
    return bodies,marks,groups


def report_ink(db, pid):
    report = meta(db,META)['reportDirectory']
    row = db.execute('SELECT report_sha FROM catalog_pages WHERE page_id=?',(pid,)).fetchone()
    require(row, f'{pid}: native validation report is not indexed')
    return load_report_ink(report,pid,row[0])


def read(dbfile):
    db = sqlite3.connect(Path(dbfile).resolve().as_uri() + '?mode=ro', uri=True, timeout=60)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA query_only=ON')
    return db


def meta(db, name):
    row = db.execute('SELECT value FROM meta WHERE key=?', (name,)).fetchone()
    return json.loads(row[0]) if row else None


def put_meta(db, key, data):
    db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)', (key, encoded(data)))


def tables(db):
    return {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}


def upgrade_native_page_art(db):
    columns = {row[1] for row in db.execute('PRAGMA table_info(catalog_page_art)')}
    if 'svg_gzip' not in columns:
        return
    require(not db.execute("SELECT 1 FROM pages WHERE source_kind IS NOT 'native-pdf' LIMIT 1").fetchone() and
            not db.execute("SELECT 1 FROM catalog_page_art WHERE length(svg_gzip)>0 LIMIT 1").fetchone(),
            'Build a new catalog database from the AI/PDF report.')
    db.execute('ALTER TABLE catalog_page_art RENAME COLUMN svg_sha TO source_sha')
    db.execute('ALTER TABLE catalog_page_art DROP COLUMN svg_gzip')


def merged(parts):
    return [v for start, end in union_intervals(parts) for v in (start, end-start)]


def extent(runs, width):
    left, top, right, bottom = width, runs[0] // width, 0, 0
    for start, length in zip(runs[::2], runs[1::2]):
        y, x = divmod(start, width)
        ey, ex = divmod(start + length - 1, width)
        left = min(left, 0 if y != ey else x)
        right = max(right, width if y != ey else ex+1)
        bottom = max(bottom, ey+1)
    return left, top, right-left, bottom-top


def manifest(report):
    buf = (report / 'manifest.json').read_bytes()
    doc = json.loads(buf)
    ids = [p['pageId'] for p in doc['pages']]
    require(doc['schemaVersion'] == 5 and not doc['failures'] and ids and
            ids == sorted(set(ids)) == doc['requestedPageIds'] and set(ids) <= set(ALL),
            'Expected a complete native character-link manifest.')
    return doc, sha(buf)


def page_rows(db, page, transcript, seed):
    pid, raster = page['pageId'], page['raster']
    source = page['provenance']
    native = page['nativeAssignment']
    require(native['producer'] == 'medinamushafanalyzer-native-marks-v1' and
            native['transcriptSha256'] == sha(encoded(transcript).encode()) and
            page['assignmentUnchangedAfterValidation'] and page['conservation']['everyPathAccounted'] and
            page['policy']['classifierUsedForAssignment'] is False, f'{pid}: invalid native provenance')
    sources = (source['sourceSha256'], source['pdfSha256'], raster['width'], raster['height'])
    require(source.get('sourceFormat')=='ai-pdf' and sources[0]==sources[1],
            f'{pid}: expected an AI/PDF source report')
    if seed:
        db.execute('INSERT INTO pages(page_id,split,source_sha,pdf_sha,width,height,source_kind) VALUES (?,?,?,?,?,?,?)',
                   (pid, 'native', *sources, 'native-pdf'))
    row = db.execute('SELECT source_sha,pdf_sha,width,height FROM pages WHERE page_id=?', (pid,)).fetchone()
    require(row and tuple(row) == sources, f'{pid}: page sources or raster differ from the vector corpus')
    paths = {p['id']: p for p in page['paths']}
    words = {w['id']: w for w in transcript['words']}
    native_words = {w['id']: w for w in page['words']}
    require(words.keys() == native_words.keys() and all(w['text'] == native_words[i]['text'] for i,w in words.items()),
            f'{pid}: word set or transcript differs')
    if seed:
        db.executemany('INSERT INTO words(id,page_id,line,ordinal,text,bases,x,y,w,h) VALUES (?,?,?,?,?,?,?,?,?,?)',
            [(wid, pid, w['line'], w['ordinal'], w['text'], plain(w['text']),
              *bounding_box([paths[i] for i in native_words[wid]['pathIds']])) for wid,w in words.items()])
    else:
        stored = {r[0]: tuple(r[1:]) for r in db.execute('SELECT id,text,line,ordinal FROM words WHERE page_id=?',(pid,))}
        require(stored == {i: (w['text'],w['line'],w['ordinal']) for i,w in words.items()}, f'{pid}: vector words differ')
    return sources, paths, words


def insert_structure(db, page, paths, words):
    pid = page['pageId']
    compact = meta(db,VECTOR_STORAGE)
    subs, owners = [], []
    letters, word_boxes = {}, {}
    for s in page['subwords']:
        for o in s['owners']:
            if o['role'] == 'letter':
                letters.setdefault(s['wordId'], []).append((o['offset'], o['character']))
    neighbors = {}
    for wid, sequence in letters.items():
        ordered = sorted(sequence)
        chars = [None] + [ch for _,ch in ordered] + [None]
        for i,(offset,_) in enumerate(ordered):
            neighbors[wid,offset] = (chars[i], chars[i+2])
    def location(subword):
        wid, number = subword['id'].rsplit('-s',1)
        return words[wid]['line'], words[wid]['ordinal'], int(number)

    for s in sorted([*page['subwords'], *page['ignoredSignGroups']],key=location):
        sid = s['id']
        wid, number = sid.rsplit('-s', 1)
        word = words[wid]
        native_owners = s.get('owners', [])
        expected = [o['id'] for o in native_owners if o['role'] == 'letter']
        kind = 'text' if expected else 'sign' if native_owners else 'decoration'
        body_path = s.get('bodyPathId')
        members = s['pathIds']
        runs = (s.get('compoundBody') or paths.get(body_path, {})).get('runs', [])
        if kind == 'decoration':
            runs = merged(paths[i]['runs'] for i in members)
        # Bind a future partition to the word, the exact native pixels and its ordered logical owners.
        key = None
        if kind == 'text' and runs:
            key = sha(encoded([pid, page['provenance']['pdfSha256'], page['provenance']['sourceSha256'],
                               page['raster']['width'], page['raster']['height'], sid, s['text'], expected, runs]).encode())
        groups = [g for o in native_owners for g in o['groups']]
        # Measure painted raster ink, including corrected compound bodies, in both a new and the shared vector DB.
        boxes = list(groups)
        if runs:
            boxes.append({'bbox':extent(runs,page['raster']['width'])})
        for path_id in s.get('unusedPathIds',[]):
            discarded = paths[path_id]['runs']
            if discarded:
                boxes.append({'bbox':extent(discarded,page['raster']['width'])})
        box = bounding_box(boxes)
        if boxes:
            word_boxes.setdefault(wid,[]).append({'bbox':box})
        subs.append((sid, pid, wid, int(number), s['text'], plain(s['text']), kind,
                     encoded(s.get('nativeSubwordIds', [sid.split(':')[1]])), body_path, s.get('baseline'),
                     *box, (b'' if compact else pack_runs(runs)) if runs else None, key, len(expected), len(groups)))
        for o in native_owners:
            offset, char = o['offset'], o['character']
            require(word['text'][offset] == char and word['characters'][offset]['text'] == char,
                    f"{o['id']}: native owner does not match QuranTextByWord")
            form = word['characters'][offset]['position']
            owners.append((o['id'],sid,wid,o['role'],offset,char,o['text'],form,
                           shape_class(char, form) if o['role']=='letter' else None, o['ordinal'],
                           *neighbors.get((wid,offset),(None,None))))
    db.executemany('''INSERT INTO catalog_subwords(id,page_id,word_id,ordinal,text,bases,kind,members,body_path,
        baseline,x,y,w,h,runs,body_key,letter_count,mask_count) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)''', subs)
    db.executemany('''INSERT INTO catalog_owners(id,subword_id,word_id,role,word_offset,character,text,form,shape_class,
        ordinal,prev_letter,next_letter) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)''', owners)
    db.executemany('INSERT INTO catalog_word_bounds VALUES (?,?,?,?,?,?)',
                   [(wid,pid,*bounding_box(word_boxes.get(wid,[]))) for wid in words])


def export_bodies(db, stream):
    """JSON Lines, ordered for a deterministic replay into an empty catalog."""
    require(not meta(db,VECTOR_STORAGE), 'This catalog stores vector bodies. Use export-page for SVG or check-rebuild to replay the vector catalog.')
    for sid, key, producer in db.execute('SELECT subword_id,source_key,producer FROM catalog_splits ORDER BY subword_id'):
        parts = []
        for owner, role, neighbor, variant, runs in db.execute(
                'SELECT owner_id,role,next_owner_id,variant,runs FROM catalog_parts WHERE subword_id=? ORDER BY owner_id,role', (sid,)):
            part = dict(owner=owner, role=role, nextOwner=neighbor, variant=variant, runs=unpack_runs(runs))
            if role=='body':
                measures = db.execute('SELECT advance,spine,kashida_left,kashida_right,confidence,quality '
                                      'FROM catalog_measurements WHERE owner_id=?',(owner,)).fetchone()
                if measures:
                    part['metrics'] = {k:v for k,v in zip(MEASURES,measures) if v is not None}
            parts.append(part)
        stream.write(encoded(dict(version=VERSION, subword=sid, sourceKey=key, producer=producer, parts=parts))+'\n')


MEASURES = ('advance','spine','kashidaLeft','kashidaRight','confidence','quality')


def partition_rows(db, record):
    sid = record['subword']
    sub = db.execute('''SELECT s.body_key,s.runs,p.width,p.height,s.page_id FROM catalog_subwords s JOIN pages p USING(page_id)
        WHERE s.id=? AND s.kind='text' ''', (sid,)).fetchone()
    require(record['version'] == VERSION and sub and sub[0] and sub[0] == record['sourceKey'],
            f'{sid}: subword, owners or native body pixels differ from the supplied source key')
    producer = record['producer']
    require(isinstance(producer,str) and producer.strip(), f'{sid}: specify the source of the segmentation')
    ordered = [r[0] for r in db.execute("SELECT id FROM catalog_owners WHERE subword_id=? AND role='letter' ORDER BY word_offset", (sid,))]
    neighbors = dict(zip(ordered[:-1],ordered[1:]))
    body_ids, masks, rows, measurements = [], [], [], []
    for p in record['parts']:
        owner, role, runs = p['owner'], p['role'], p['runs']
        adjacent = p.get('nextOwner')
        require(owner in ordered and role in ('body','connection','kashida'), f'{sid}: invalid part owner or role')
        if role == 'body':
            require(adjacent is None, f'{sid}: a body has one owner')
            body_ids.append(owner)
        else:
            require(adjacent and neighbors.get(owner) == adjacent, f'{sid}: connection needs two adjacent letter owners')
        require(isinstance(runs,list) and runs and len(runs)%2==0 and all(type(v)==int for v in runs) and
                all(st>=0 and n>0 and st+n<=sub[2]*sub[3] for st,n in zip(runs[::2],runs[1::2])) and
                all(st+n<=following for st,n,following in zip(runs[::2],runs[1::2],runs[2::2])),
                f'{sid}: invalid page-raster runs')
        masks.append(runs)
        rows.append((owner,role,sid,adjacent,p.get('variant'),*extent(runs,sub[2]),sum(runs[1::2]),pack_runs(runs)))
        metrics = p.get('metrics',{})
        if metrics:
            require(role=='body' and isinstance(metrics,dict) and metrics.keys() <= set(MEASURES),
                    f'{sid}: only body parts may report letter metrics')
            for key,value in metrics.items():
                if key=='quality':
                    require(isinstance(value,str) and value.strip(),f'{sid}: invalid cut quality')
                else:
                    require(type(value) in (int,float) and math.isfinite(value) and value>=0
                            and (key!='confidence' or value<=1),f'{sid}: invalid {key}')
            measurements.append((owner, *(metrics.get(k) for k in MEASURES)))
    require(sorted(body_ids)==sorted(ordered), f'{sid}: each transcript letter must have one body')
    pixels = merged(masks)
    source = report_ink(db,sub[4])[0][sid] if sub[1]==b'' else unpack_runs(sub[1])
    require(pixels == merged([source]) and sum(pixels[1::2]) == sum(sum(r[1::2]) for r in masks),
            f'{sid}: parts overlap, omit native body ink, or contain other ink')
    return rows, measurements


def attach_record(db, record, *, vector_parts=None):
    rows, measurements = partition_rows(db,record)
    sid = record['subword']
    if meta(db,VECTOR_STORAGE):
        require(vector_parts is not None, 'This catalog requires verified native outlines. Run segment-letters to update its body cuts.')
        for row in rows:
            outline = vector_parts.get(row[0]+'/'+row[1])
            require(outline and outline['ink']==sha(row[-1]),f'{sid}: missing verified vector for {row[0]}/{row[1]}')
        rows = [(*row[:-1],ink_reference(row[-1])) for row in rows]
    db.execute('DELETE FROM catalog_splits WHERE subword_id=?',(sid,))
    db.execute('INSERT INTO catalog_splits VALUES (?,?,?)',(sid,record['sourceKey'],record['producer']))
    db.executemany('INSERT INTO catalog_parts VALUES (?,?,?,?,?,?,?,?,?,?,?)',rows)
    db.executemany('INSERT INTO catalog_measurements(owner_id,advance,spine,kashida_left,kashida_right,confidence,quality) '
                   'VALUES (?,?,?,?,?,?,?)',measurements)


def import_bodies(db, stream):
    seen = set()
    for line in stream:
        record = json.loads(line)
        require(record['subword'] not in seen, 'Duplicate subword in the segmentation bundle')
        attach_record(db,record)
        seen.add(record['subword'])
    return len(seen)


def inventory(db):
    names = dict(pages='catalog_pages', words='words', subwords="catalog_subwords WHERE kind='text'",
                 signs="catalog_subwords WHERE kind='sign'", decorations="catalog_subwords WHERE kind='decoration'",
                 letters="catalog_owners WHERE role='letter'", segmented='catalog_splits', parts='catalog_parts',
                 masks='native_marks WHERE runs IS NOT NULL')
    result = {k: db.execute('SELECT count(*) FROM '+v).fetchone()[0] for k,v in names.items()}
    result['pending'] = result['subwords'] - result['segmented']
    result['verdicts'] = dict(db.execute('SELECT verdict,count(*) FROM native_marks GROUP BY verdict'))
    return result


def verify(db, deep=False):
    info = meta(db,META)
    require(info and info['version'] == VERSION, 'Build the mushaf catalog first.')
    counts = inventory(db)
    for key in ('pages','words','subwords','signs','decorations','letters','masks','verdicts'):
        require(counts[key]==info['inventory'][key], f'{key} count differs from the native import')
    errors = db.execute('''SELECT m.id FROM native_marks m LEFT JOIN catalog_owners o ON o.id=m.owner_id
        LEFT JOIN words w ON w.id=m.word_id WHERE o.id IS NULL OR m.subword_id!=o.subword_id OR m.word_id!=o.word_id
        OR (m.character_id IS NOT NULL AND (substr(w.text,m.word_offset+1,1)!=m.source_character
        OR m.character_id!=w.id||':c'||m.word_offset)) OR (o.role='letter' AND (m.base_offset IS NOT o.word_offset
        OR m.base_character IS NOT o.character)) LIMIT 1''').fetchone()
    require(not errors, f'Invalid native mark or character owner: {errors}')
    errors = db.execute('''SELECT m.id FROM native_marks m JOIN catalog_owners o ON o.id=m.owner_id
        WHERE (m.attached_letter_id IS NOT NULL OR (o.role='word-sign' AND m.expected_type LIKE 'waqf-%'))
          AND m.attached_letter_id IS NOT CASE WHEN o.role='word-sign' AND m.expected_type LIKE 'waqf-%' THEN (
              SELECT l.id FROM catalog_owners l WHERE l.subword_id=o.subword_id AND l.role='letter'
              AND l.word_offset<o.word_offset ORDER BY l.word_offset DESC LIMIT 1
          ) END LIMIT 1''').fetchone()
    require(not errors, f'Invalid waqf letter link: {errors}')
    require(db.execute('SELECT count(*) FROM catalog_word_bounds').fetchone()[0]==counts['words'] and
            db.execute('SELECT count(*) FROM catalog_coordinates').fetchone()[0]==counts['pages'],
            'Incomplete word bounds or raster coordinates')
    if deep:
        if 'catalog_page_art' in tables(db):
            for pid, saved_source, saved_report, expected_source, expected_report, vectors in db.execute(
                    '''SELECT a.page_id,a.source_sha,a.report_sha,p.source_sha,c.report_sha,a.vectors
                       FROM catalog_page_art a JOIN pages p USING(page_id) JOIN catalog_pages c USING(page_id)'''):
                require(saved_source == expected_source and saved_report == expected_report,
                        f'{pid}: stored page artwork differs from the catalog sources')
                artwork = json.loads(zlib.decompress(vectors)).get('pageArt')
                require(artwork and artwork['paint'] and artwork['opening']['bounds'],
                        f'{pid}: missing native page artwork')
        require(not db.execute("""SELECT s.id FROM catalog_subwords s WHERE s.letter_count!=
            (SELECT count(*) FROM catalog_owners o WHERE o.subword_id=s.id AND o.role='letter') OR s.mask_count!=
            (SELECT count(*) FROM native_marks m WHERE m.subword_id=s.id AND m.runs IS NOT NULL) LIMIT 1""").fetchone(),
            'Native subword inventory differs from its owners or ink masks')
    require(not db.execute('''SELECT c.page_id FROM catalog_pages c LEFT JOIN native_mark_pages n USING(page_id)
        WHERE c.report_sha IS NOT n.report_sha LIMIT 1''').fetchone(), 'Native mark and structural imports differ')
    require(not db.execute('''SELECT s.id FROM catalog_splits b JOIN catalog_subwords s ON s.id=b.subword_id
        WHERE b.source_key IS NOT s.body_key OR s.letter_count!=(SELECT count(*) FROM catalog_parts p
        WHERE p.subword_id=s.id AND p.role='body') LIMIT 1''').fetchone(), 'Incomplete letter-body partition')
    require(not db.execute('PRAGMA foreign_key_check').fetchone(), 'Invalid foreign key in corpus')
    if deep and meta(db,VECTOR_STORAGE):
        from compact import verify_vectors
        verify_vectors(db)
    elif deep and counts['segmented']:
        with TemporaryFile(mode='w+t',encoding='utf-8') as saved:
            export_bodies(db,saved)
            saved.seek(0)
            for line in saved:
                partition_rows(db,json.loads(line))
    return counts


PARTITION_TABLES = ('catalog_splits','catalog_parts','catalog_measurements')


def save_vector_partitions(db, source):
    for table in PARTITION_TABLES:
        if source is db:
            db.execute(f'CREATE TEMP TABLE saved_{table} AS SELECT * FROM {table}')
        else:
            db.execute(f'CREATE TEMP TABLE saved_{table} AS SELECT * FROM {table} WHERE 0')
            rows = source.execute(f'SELECT * FROM {table}')
            placeholders = ','.join('?' for _ in rows.description)
            db.executemany(f'INSERT INTO saved_{table} VALUES ({placeholders})',rows)
    if source is not db:
        db.execute('CREATE TEMP TABLE saved_page_art AS SELECT * FROM catalog_page_art WHERE 0')
        db.executemany('INSERT INTO saved_page_art VALUES (?,?,?,?)',source.execute('SELECT * FROM catalog_page_art'))


def build(database, report, expected_manifest=None, overlay_db=None):
    database, report = Path(database).resolve(), Path(report).resolve()
    manifest_doc, fingerprint = manifest(report)
    require(expected_manifest is None or fingerprint==expected_manifest, 'Mark report manifest changed')
    data = (report/'inputs/transcript.json').read_bytes()
    transcript_sha = sha(data)
    snapshot = json.loads(data)
    del data
    require(snapshot['producer']=='medinamushafanalyzer-QuranTextByWord-v1', 'Expected native QuranTextByWord')
    transcript = {p['pageId']:p for p in snapshot['pages']}
    entries = manifest_doc['pages']
    require(set(transcript) >= set(manifest_doc['requestedPageIds']), 'Missing transcript pages')
    if database == DB.resolve():
        require(manifest_doc['requestedPageIds']==ALL, 'The shared corpus requires all 604 pages')
    database.parent.mkdir(parents=True,exist_ok=True)
    with closing(sqlite3.connect(database, timeout=120)) as db, TemporaryFile(mode='w+t',encoding='utf-8') as saved, db:
        known = tables(db)
        require(not known or {'pages','words','meta'}<=known, 'Unrecognized SQLite database')
        seed = not known
        sql = ''
        if seed:
            sql = (VECTOR/'automatic/schema.sql').read_text().replace('PRAGMA journal_mode=WAL;', '')
            db.execute('PRAGMA journal_mode=WAL')
        db.execute('PRAGMA foreign_keys=ON')
        db.executescript('BEGIN IMMEDIATE;\n'+sql+(VECTOR/'automatic/transcript_marks.sql').read_text()+
                         (HERE/'schema.sql').read_text()+(HERE/'page_art.sql').read_text())
        upgrade_native_page_art(db)
        added_mark_letter = ensure_mark_letter_column(db)
        if not seed:
            require({r[0] for r in db.execute('SELECT page_id FROM pages')} == set(manifest_doc['requestedPageIds']),
                    'Report must cover exactly the pages of this corpus')
        compact = bool(meta(db,VECTOR_STORAGE))
        if overlay_db:
            with closing(read(overlay_db)) as old:
                old.execute('BEGIN')
                old_compact = bool(meta(old,VECTOR_STORAGE))
                require(not compact or old_compact,'Use a vector catalog to restore a vector catalog')
                if old_compact:
                    save_vector_partitions(db,old)
                    put_meta(db,VECTOR_STORAGE,True)
                    compact = True
                else:
                    export_bodies(old,saved)
        elif compact:
            save_vector_partitions(db,db)
        else:
            export_bodies(db,saved)
        indexed = dict(db.execute('SELECT page_id,report_sha FROM native_mark_pages'))
        rebuild_marks = indexed != {entry['pageId']:entry['reportSha256'] for entry in entries} or (
            compact and db.execute('SELECT 1 FROM native_marks WHERE length(runs)>0 LIMIT 1').fetchone())
        if rebuild_marks:
            # Native sequence numbers also drive the existing mark browser's page order.
            db.execute('DELETE FROM native_mark_pages')
        db.execute('DELETE FROM catalog_subwords')
        db.execute('DELETE FROM catalog_pages')
        for n,entry in enumerate(entries,1):
            pid = entry['pageId']
            buf = (report/f'{pid}.json').read_bytes()
            require(sha(buf)==entry['reportSha256'], f'{pid}: report hash mismatch')
            page = json.loads(buf)
            require(page['pageId']==pid and page['provenance']['transcriptSnapshotSha256']==transcript_sha and
                    page['assignmentSha256']==entry['assignmentSha256'], f'{pid}: report or transcript changed')
            sources,paths,words = page_rows(db,page,transcript[pid],seed)
            if rebuild_marks:
                import_page(db,page,entry,sources)
            db.execute('INSERT INTO catalog_pages VALUES (?,?)',(pid,entry['reportSha256']))
            db.execute('INSERT INTO catalog_coordinates VALUES (?,?,?)',
                       (pid,encoded(page['raster']),encoded(page['provenance']['pdfToRaster'])))
            insert_structure(db,page,paths,words)
            if n%100==0:
                print(f'Indexed {n}/{len(entries)} native pages', file=sys.stderr, flush=True)
        if added_mark_letter and not rebuild_marks:
            backfill_waqf_links(db)
        if compact:
            for table in PARTITION_TABLES:
                db.execute(f'INSERT INTO {table} SELECT * FROM saved_{table}')
            if overlay_db:
                db.execute('INSERT OR REPLACE INTO catalog_page_art SELECT * FROM saved_page_art')
        else:
            saved.seek(0)
            import_bodies(db,saved)
        db.execute('''DELETE FROM catalog_page_art WHERE NOT EXISTS (
            SELECT 1 FROM catalog_pages c JOIN pages p USING(page_id) WHERE c.page_id=catalog_page_art.page_id
            AND c.report_sha=catalog_page_art.report_sha AND p.source_sha=catalog_page_art.source_sha)''')
        counts = inventory(db)
        summary = manifest_doc['summary']
        require((counts['masks'],counts['letters'],counts['subwords']+counts['signs'],counts['signs'])==
                (summary['classifiedGroups'],summary['letters'],summary['subwords'],summary['signOnlyGroups']),
                'Catalog and native report counts differ')
        put_meta(db,'nativeMarkIndex',dict(pages=len(entries),masks=counts['masks'],counts=counts['verdicts'],
                                          reportDirectory=str(report),manifestSha256=fingerprint))
        put_meta(db,META,dict(version=VERSION,reportDirectory=str(report),manifestSha256=fingerprint,
                              transcriptSha256=transcript_sha,inventory=counts))
        verify(db)
    return counts
