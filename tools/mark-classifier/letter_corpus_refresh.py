"""Stage new native references against frozen corpus predictions, without inference.

The original comparison remains read-only. Publishing the staged database is a
separate operation after the ownership and regression report has been checked.
"""
import argparse
from collections import Counter
from contextlib import closing
import io
import json
from pathlib import Path
import sqlite3
import time

import numpy as np

from letter_corpus_audit import (
    NATIVE, corpus_summary, digest, document_for, prepare_example, save_example,
    source_records, write_json,
)
from letter_bases import base_character_count


def geometry(row):
    def path(value):
        return [value.get('fillRule'), [[v if isinstance(v, str) else round(v, 6)
                                       for v in command] for command in value.get('commands', [])]]
    return dict(bodies=[[body['letter'], path(body)] for body in row['bodies']],
                kashidas=[path(join.get('kashida') or {}) for join in row['joins']])


def stage_reference(db, item, document, revision):
    """Replace only a reference; reject any change to the saved inference input."""
    sid = item['vector']['id']
    old = db.execute('SELECT detail,masks FROM examples WHERE id=?', (sid,)).fetchone()
    if old is None:
        raise ValueError(f'No frozen prediction for {sid}')
    detail = json.loads(old[0])
    if detail.get('referenceHumanReviewed'):
        raise ValueError(f'Cannot overwrite a human-reviewed reference: {sid}')
    with np.load(io.BytesIO(old[1])) as saved:
        arrays = {name: saved[name].copy() for name in ('ink', 'prediction', 'predicted_roles', 'raw')}
    if not np.array_equal(item['ink'], arrays['ink']) or item['bbox'] != detail['bbox']:
        raise ValueError(f'Source ink or crop changed for {sid}; inference would need to be reconsidered')
    db.execute('DELETE FROM examples WHERE id=?', (sid,))
    fresh = save_example(db, item, arrays['prediction'], arrays['predicted_roles'], arrays['raw'],
                         document, detail['split'], {sid} if detail['usedForTraining'] else set())
    if fresh['expectedBodies'] != detail['expectedBodies'] or fresh['text'] != detail['text']:
        raise ValueError(f'Transcript or expected body count changed for {sid}')
    fresh['originalCompleted'] = detail.get('originalCompleted', detail['completed'])
    fresh['referenceRevision'] = revision
    fresh['referenceHumanReviewed'] = False
    db.execute('UPDATE examples SET detail=? WHERE id=?', (json.dumps(fresh, ensure_ascii=False), sid))
    blob = db.execute('SELECT masks FROM examples WHERE id=?', (sid,)).fetchone()[0]
    with np.load(io.BytesIO(blob)) as updated:
        for name, array in arrays.items():
            if not np.array_equal(updated[name], array):
                raise ValueError(f'Frozen {name} changed for {sid}')
    return detail, fresh


def stage_metadata(db, sid, row, revision):
    """Keep the packed masks intact when only template diagnostics changed."""
    record = db.execute('SELECT detail FROM examples WHERE id=?', (sid,)).fetchone()
    if record is None:
        raise ValueError(f'No frozen prediction for {sid}')
    old = json.loads(record[0])
    if old.get('referenceHumanReviewed'):
        raise ValueError(f'Cannot overwrite a human-reviewed reference: {sid}')
    fresh = dict(old, templates=row['templates'], letters=row['letters'], referenceRevision=revision,
                 referenceHumanReviewed=False,
                 originalCompleted=old.get('originalCompleted', old['completed']))
    db.execute('UPDATE examples SET detail=?,templates=? WHERE id=?',
               (json.dumps(fresh, ensure_ascii=False), json.dumps(row['templates']), sid))
    return old, fresh


def refresh(folder, baseline, snapshot, output, batch_file=None):
    started = time.perf_counter()
    generation_file = snapshot / 'generation.json'
    generation_hash = digest(generation_file)
    generation = json.loads(generation_file.read_bytes())
    if generation['pagesCompleted'] != 604 or generation['failures'] or generation.get('trackerEnabled'):
        raise ValueError('A complete, tracker-disabled native snapshot is required')
    if output.exists():
        raise ValueError('Use a new staging directory; an earlier result already exists')
    output.mkdir(parents=True)
    live = folder / 'corpus-audit'
    summary = json.loads((live / 'summary.json').read_bytes())
    connected_only = summary.get('connectedOnly', False)

    def indexed_rows(cache):
        return {(r['line'], r['word'], r['subword']): r for r in source_records(cache)
                if not connected_only or base_character_count(r['text']) >= 2}

    database = output / 'comparison.sqlite'
    with closing(sqlite3.connect((live / 'comparison.sqlite').as_uri() + '?mode=ro', uri=True)) as source:
        with closing(sqlite3.connect(database)) as staged:
            source.backup(staged)
    changes, counts = [], Counter()
    revision = generation_hash[:16]
    with closing(sqlite3.connect(database.as_uri(), uri=True)) as db:
        db.execute('CREATE TEMP TABLE changed_ids (id TEXT PRIMARY KEY)')
        for page in range(1, 605):
            name = f'page{page:03}.json'
            old_cache = json.loads((baseline / 'pages' / name).read_bytes())
            new_cache = json.loads((snapshot / 'pages' / name).read_bytes())
            if old_cache.get('humanReviewed') or new_cache.get('humanReviewed'):
                raise ValueError('This refresh is reserved for automatic cutter references')
            for key in ('sourceFileSha256', 'fontSha256', 'sourceWords', 'sourceLines'):
                if old_cache[key] != new_cache[key]:
                    raise ValueError(f'Native source scope changed on page {page}: {key}')
            old_rows = indexed_rows(old_cache)
            new_rows = indexed_rows(new_cache)
            if old_rows.keys() != new_rows.keys():
                raise ValueError(f'Subword identities changed on page {page}')
            document = None
            with db:
                for key, row in new_rows.items():
                    previous = old_rows[key]
                    counts['examined'] += 1
                    if row['sourceInk'] != previous['sourceInk'] or row['text'] != previous['text']:
                        raise ValueError(f'Artwork or transcript changed on page {page}, {key}')
                    old_geometry, new_geometry = geometry(previous), geometry(row)
                    mask_changed = old_geometry != new_geometry
                    metadata_changed = any(previous.get(k) != row.get(k) for k in
                                           ('complete', 'letters', 'templates'))
                    if not mask_changed and not metadata_changed:
                        continue
                    status_changed = previous['complete'] != row['complete']
                    if mask_changed or status_changed:
                        if document is None:
                            raw = json.loads((NATIVE / 'output/vector-review' / f'page{page:03}.raw.json').read_bytes())
                            document = document_for(new_cache, raw)
                        item = prepare_example(row, document)
                        old, new = stage_reference(db, item, document, revision)
                    else:
                        sid = f'page{page:03}:l{key[0]}-w{key[1]}-s{key[2]}'
                        old, new = stage_metadata(db, sid, row, revision)
                    sid = new['id']
                    db.execute('INSERT INTO changed_ids VALUES (?)', (sid,))
                    change = dict(id=sid, page=page, text=row['text'], geometryChanged=mask_changed,
                                  beforeReference=old['reference'], afterReference=new['reference'],
                                  before=old['completed']['differenceFraction'],
                                  after=new['completed']['differenceFraction'],
                                  beforeTemplates=old['templates'], afterTemplates=new['templates'],
                                  rasterError=new['referenceStatus'].get('rasterError'))
                    changes.append(change)
                    counts['geometryChanged' if mask_changed else 'metadataOnly'] += 1
                    if old['reference'] != 'partial' and new['reference'] == 'partial':
                        counts['becamePartial'] += 1
                    if old['reference'] == 'partial' and new['reference'] != 'partial':
                        counts['resolved'] += 1
            if page % 40 == 0:
                print(f'{page}/604 pages checked; {len(changes)} references staged', flush=True)
        db.execute('ATTACH DATABASE ? AS original',
                   ((live / 'comparison.sqlite').as_uri() + '?mode=ro',))
        untouched = db.execute('''SELECT COUNT(*) FROM examples AS fresh
            JOIN original.examples AS old USING(id)
            WHERE NOT EXISTS (SELECT 1 FROM changed_ids WHERE id=fresh.id)
            AND (fresh.detail != old.detail OR fresh.masks != old.masks)''').fetchone()[0]
        if untouched:
            raise ValueError('An unrelated reference changed in the staging database')
        summary.update(corpus_summary(db))
        if summary['examples'] != counts['examined']:
            raise ValueError('The refreshed corpus does not match the frozen prediction population')
    if digest(generation_file) != generation_hash:
        raise ValueError('Native snapshot changed during the comparison')
    report = dict(schemaVersion=1, snapshot=str(snapshot), generationSha256=generation_hash,
                  revision=revision, originalComparison=str(live / 'comparison.sqlite'),
                  stagedDatabase=str(database), elapsedSeconds=time.perf_counter()-started,
                  comparisonCodeSha256=digest(Path(__file__).with_name('letter_page_compare.py')),
                  comparisonRasterization='One-pixel edge repairs up to 3%; larger gaps stay unknown. Training export unchanged.',
                  counts=dict(counts), unchangedReferencesByteEqual=True,
                  frozenInferenceArraysUnchanged=True, sourceInkAndTranscriptsUnchanged=True,
                  trainingTargetsUpdated=False, weightsUpdated=False,
                  humanReviewed=False, changes=changes)
    summary['referenceUpdate'] = {k: v for k, v in report.items() if k != 'changes'}
    batches = {}
    if batch_file:
        ids = batch_file.read_text().splitlines()
        batches['first66-20261106'] = dict(title='Original top 66 through page130:l1-w9-s1', ids=ids)
    write_json(output / 'review-batches.json', batches)
    write_json(output / 'summary.json', summary)
    write_json(output / 'reference-update.json', report)
    (output / 'changed-ids.txt').write_text(''.join(change['id']+'\n' for change in changes))
    print(json.dumps({k: v for k, v in report.items() if k != 'changes'}, indent=2), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--batch-ids', type=Path)
    args = parser.parse_args()
    refresh(*(getattr(args, key).resolve() for key in ('folder', 'baseline', 'snapshot', 'output')),
            args.batch_ids)


if __name__ == '__main__':
    main()
