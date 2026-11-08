"""Import native mark masks and their independently validated character links into the vector corpus."""
from workbench_paths import NATIVE
import argparse
from collections import Counter
from contextlib import closing
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import subprocess
import sys

from .common import bounding_box, pack_runs
from .word_audit import union_intervals

ROOT = Path(__file__).resolve().parents[1]
META_KEY = 'nativeMarkIndex'
CATALOG = NATIVE/'tools/mushafcatalog/catalog.py'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def ensure_mark_letter_column(db):
    added = 'attached_letter_id' not in {r[1] for r in db.execute('PRAGMA table_info(native_marks)')}
    if added:
        db.execute('ALTER TABLE native_marks ADD COLUMN attached_letter_id TEXT')
    db.execute('CREATE INDEX IF NOT EXISTS native_marks_attached_letter ON native_marks(attached_letter_id,seq) '
               'WHERE attached_letter_id IS NOT NULL')
    return added


def backfill_waqf_links(db):
    """Upgrade published catalogs using the same subword order as the native assigner."""
    return db.execute('''UPDATE native_marks AS m SET attached_letter_id=(
        SELECT l.id FROM catalog_owners l WHERE l.subword_id=m.subword_id AND l.role='letter'
          AND l.word_offset<m.word_offset ORDER BY l.word_offset DESC LIMIT 1
    ) WHERE m.expected_type LIKE 'waqf-%' AND m.attached_letter_id IS NULL
      AND EXISTS(SELECT 1 FROM catalog_owners o WHERE o.id=m.owner_id AND o.role='word-sign')''').rowcount


def import_page(db, doc, entry, source):
    pid = entry['pageId']
    compact = db.execute("SELECT 1 FROM meta WHERE key='mushafCatalogVectorStorage'").fetchone()
    raster, provenance = doc['raster'], doc['provenance']
    require(doc['pageId'] == pid and doc['schemaVersion'] == 5 and doc['assignmentUnchangedAfterValidation']
            and doc['assignmentSha256'] == entry['assignmentSha256'] and doc['conservation']['everyPathAccounted']
            and doc['policy']['classifierUsedForAssignment'] is False, f'{pid}: invalid native assignment')
    require(source == (provenance['sourceSha256'], provenance['pdfSha256'], raster['width'], raster['height']),
            f'{pid}: SVG, PDF or raster differs from the vector corpus')
    audit = dict(native=doc['nativeAssignment'], models=doc['classifier']['modelFiles'],
                 inkSha256=doc['conservation']['inkSha256'])
    db.execute('INSERT INTO native_mark_pages VALUES (?,?,?,?)',
               (pid, entry['reportSha256'], doc['assignmentSha256'], json.dumps(audit, sort_keys=True)))
    words = {w['id']: w for w in doc['words']}
    corpus_words = dict(db.execute('SELECT id,text FROM words WHERE page_id=?', (pid,)))
    require(all(corpus_words.get(i) == w['text'] for i,w in words.items()), f'{pid}: word or transcript changed')
    paths = {p['id']: p for p in doc['paths']}
    painted = {k: p['runs'] for k, p in paths.items()}
    for sub in doc['subwords']:
        body = sub.get('compoundBody')
        if body:
            painted[body['primaryPathId']] = body['runs']
            painted[body['secondaryPathId']] = []
    for wid in sorted({s['wordId'] for s in doc['subwords']}):
        ids = words[wid]['pathIds']
        spans = union_intervals(painted[k] for k in ids)
        runs = [v for a, b in spans for v in (a, b-a)]
        db.execute('INSERT INTO native_mark_words VALUES (?,?,?,?,?,?,?)',
                   (wid, pid, *bounding_box([paths[k] for k in ids]), b'' if compact else pack_runs(runs)))
    cols = ('id','page_id','word_id','subword_id','owner_id','base_character','base_offset','character_id',
            'word_offset','source_character','codepoint','expected_type','equivalence','method','candidates',
            'verdict','classifier_type','raw_type','classifier_kind','score','margin','path_ids','x','y','w','h','runs',
            'attached_letter_id')
    rows, counts = [], Counter()
    for sub, compared in zip(doc['subwords'], doc['validation'], strict=True):
        wid, sid = sub['wordId'], sub['id']
        require(sid == compared['id'], f'{sid}: comparison differs from assignment')
        preceding_letter = None
        for owner, result in zip(sub['owners'], compared['owners'], strict=True):
            require(owner['id'] == result['id'], f'{sid}: unexpected owner')
            base, base_offset = (owner['character'], owner['offset']) if owner['role'] == 'letter' else (None, None)
            common = dict(page_id=pid, word_id=wid, subword_id=sid, owner_id=owner['id'],
                          base_character=base, base_offset=base_offset)
            slots = {s['characterOffset']: s for s in owner['expectedSlots']}
            if owner['role']=='letter':
                preceding_letter = owner['id']
            def character(offset):
                slot = slots[offset]
                char = words[wid]['text'][offset]
                require(char == slot['sourceCharacter'] and slot['codePoint'] == f'U+{ord(char):04X}',
                        f'{wid}: incorrect character at offset {offset}')
                attached = preceding_letter if owner['role']=='word-sign' and slot['type'].startswith('waqf-') else None
                return dict(character_id=f'{wid}:c{offset}', word_offset=offset, source_character=char,
                            codepoint=slot['codePoint'], expected_type=slot['type'], equivalence=slot.get('equivalence'),
                            attached_letter_id=attached)

            for group, prediction in zip(owner['groups'], result['predictions'], strict=True):
                link = group['transcriptLink']
                gid, verdict = group['id'], prediction['characterMatch']
                require(gid == prediction['id'] and group['runs'] and link['baseOffset'] == base_offset
                        and link['baseCharacter'] == base, f'{gid}: ink, classifier or owner differs')
                row = dict(common, id=gid, verdict=verdict, classifier_type=prediction['inventoryType'],
                           raw_type=prediction['type'], classifier_kind=prediction['kind'], score=prediction['score'],
                           margin=prediction['margin'], path_ids=json.dumps(group['pathIds']),
                           runs=b'' if compact else pack_runs(group['runs']))
                row.update(zip(('x','y','w','h'), group['bbox'], strict=True))
                if link['status'] == 'linked':
                    row.update(character(link['characterOffset']), method=link['method'])
                    require(row['character_id'] == link['characterId'] == prediction['transcriptCharacterId'] and
                            row['expected_type'] == link['expectedType'] and verdict in ('match','different','accepted-form'),
                            f'{gid}: inconsistent transcript target')
                else:
                    require(verdict == link['status'] and verdict in ('ambiguous','unmatched'), f'{gid}: invalid status')
                    row['candidates'] = json.dumps(link['candidateOffsets'])
                attached = row.get('attached_letter_id')
                require(link.get('attachedLetterId',attached)==attached, f'{gid}: incorrect attached letter')
                rows.append(row)
                counts[verdict] += 1
            for offset in result['charactersWithoutInk']:
                slot = character(offset)
                rows.append(dict(common, **slot, id=slot['character_id']+':missing', verdict='missing'))
                counts['missing'] += 1
    db.executemany(f"INSERT INTO native_marks ({','.join(cols)}) VALUES ({','.join('?' for _ in cols)})",
                   (tuple(row.get(k) for k in cols) for row in rows))
    summary = entry['summary']
    require(sum(counts.values())-counts['missing'] == len(doc['groups']) == summary['classifiedGroups'] and
            (counts['different'], counts['missing'], counts['ambiguous'], counts['unmatched']) ==
            tuple(summary[k] for k in ('characterLinkMismatches','charactersWithoutInk','ambiguousInkGroups','unmatchedInkGroups')),
            f'{pid}: index counts differ from the report')
    return counts


def import_report(database, report_dir, expected_manifest=None):
    report_dir = Path(report_dir).resolve()
    # The consultation layer and mark rows must always describe the same native assignment.
    with closing(sqlite3.connect(Path(database).resolve().as_uri()+'?mode=ro', uri=True)) as old:
        has_catalog = old.execute("SELECT 1 FROM meta WHERE key='mushafCatalog'").fetchone()
    if has_catalog:
        rebuild_catalog(database, report_dir, expected_manifest)
        with closing(sqlite3.connect(Path(database).resolve().as_uri()+'?mode=ro', uri=True)) as db:
            return json.loads(db.execute('SELECT value FROM meta WHERE key=?',(META_KEY,)).fetchone()[0])
    raw = (report_dir/'manifest.json').read_bytes()
    manifest_sha = hashlib.sha256(raw).hexdigest()
    require(expected_manifest is None or expected_manifest == manifest_sha, 'The saved mark report has changed')
    manifest = json.loads(raw)
    entries = manifest['pages']
    pages = [p['pageId'] for p in entries]
    require(manifest['schemaVersion'] == 5 and pages and pages == sorted(set(pages)) == manifest['requestedPageIds']
            and all(re.fullmatch(r'page[0-9]{3}', p) for p in pages) and not manifest['failures'],
            'A complete character-link report is required')
    counts = Counter()
    with closing(sqlite3.connect(Path(database).resolve().as_uri()+'?mode=rw', uri=True, timeout=60)) as db, db:
        db.execute('PRAGMA foreign_keys=ON')
        db.executescript('BEGIN IMMEDIATE;\n'+Path(__file__).with_suffix('.sql').read_text())
        ensure_mark_letter_column(db)
        sources = {r[0]: r[1:] for r in db.execute('SELECT page_id,source_sha,pdf_sha,width,height FROM pages')}
        require(set(pages) == sources.keys(), 'Report must cover every page in the selected vector corpus')
        db.execute('DELETE FROM native_mark_pages')
        for entry in entries:
            pid = entry['pageId']
            data = (report_dir/f'{pid}.json').read_bytes()
            require(hashlib.sha256(data).hexdigest() == entry['reportSha256'], f'{pid}: page report hash mismatch')
            counts.update(import_page(db, json.loads(data), entry, sources[pid]))
        result = dict(pages=len(pages), masks=sum(counts.values())-counts['missing'], counts=dict(counts),
                      reportDirectory=str(report_dir), manifestSha256=manifest_sha)
        db.execute('INSERT OR REPLACE INTO meta(key,value) VALUES (?,?)', (META_KEY, json.dumps(result)))
    return result


def rebuild_catalog(database, report, expected_manifest, previous=None):
    command = [sys.executable, str(CATALOG), '--db', str(database), 'build', '--report', str(report)]
    if expected_manifest:
        command.extend(['--expected-manifest', expected_manifest])
    if previous:
        command.extend(['--restore-bodies-from', str(previous)])
    subprocess.run(command, check=True)


def replay_existing_index(previous, replacement):
    if not previous.is_file():
        return
    with closing(sqlite3.connect(previous.resolve().as_uri()+'?mode=ro', uri=True)) as old:
        row = old.execute('SELECT value FROM meta WHERE key=?', (META_KEY,)).fetchone()
        catalog = old.execute("SELECT value FROM meta WHERE key='mushafCatalog'").fetchone()
    if catalog:
        info = json.loads(catalog[0])
        rebuild_catalog(replacement, info['reportDirectory'], info['manifestSha256'], previous)
        return
    if row:
        info = json.loads(row[0])
        import_report(replacement, info['reportDirectory'], expected_manifest=info['manifestSha256'])


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--database', type=Path, default=ROOT/'data/automatic/corpus.sqlite')
    cli.add_argument('--report', type=Path, required=True)
    args = cli.parse_args()
    print(json.dumps(import_report(args.database, args.report), ensure_ascii=False, indent=2))
