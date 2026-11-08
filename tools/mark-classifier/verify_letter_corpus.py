#!/usr/bin/env python3
"""Verify source conservation and replay the one-method letter corpus."""
import argparse
from collections import Counter
from contextlib import closing
import copy
import json
from pathlib import Path
import sqlite3
import tempfile

import numpy as np

from automatic.common import digest_file, unpack_runs
from letter_corpus.alignment import BODY_LETTERS
from letter_corpus.build import BODY, build_page
from letter_corpus.geometry import pixels
from letter_corpus.inference import Predictor

ROOT = Path(__file__).resolve().parent


def check_equal(db, records, pid):
    for table, rows in records.items():
        if table=='pages': continue  # Input fingerprint includes unused diagnostic fields.
        where = 'subword_id IN (SELECT id FROM subwords WHERE page_id=?)' if table=='expected_marks' else 'page_id=?'
        key='mark_id' if table=='mark_transcripts' else 'id'
        saved={r[key]:dict(r) for r in db.execute(f'SELECT * FROM {table} WHERE {where}',(pid,))}
        proposed={r[key]:r for r in rows}
        if saved!=proposed:
            changed=[k for k in saved.keys()|proposed.keys() if saved.get(k)!=proposed.get(k)]
            raise AssertionError(f'{pid}: replay differs in {table}: {changed[:5]}')


def verify(root, fresh_inference=False):
    root=Path(root).resolve()
    report=dict(method='mask2former',learnedMarkClassifierUsed=False,pages=[],freshInference=fresh_inference)
    original_hash=digest_file(root/'corpus.sqlite')
    review_path=root/'reviews.sqlite'
    review_hash=digest_file(review_path) if review_path.exists() else None
    with tempfile.TemporaryDirectory(prefix='letter-corpus-replay-') as temporary, \
            closing(sqlite3.connect((root/'corpus.sqlite').as_uri()+'?mode=ro',uri=True)) as db:
        db.row_factory=sqlite3.Row
        if db.execute('PRAGMA integrity_check').fetchone()[0]!='ok': raise AssertionError('Database integrity failed')
        if db.execute('PRAGMA foreign_key_check').fetchall(): raise AssertionError('Invalid foreign keys')
        device=json.loads((root/'build-report.json').read_text()).get('device','cpu')
        if fresh_inference:
            replay_root=Path(temporary)
            (replay_root/'model').symlink_to(root/'model',target_is_directory=True)
        else: replay_root=root
        predictor=Predictor(replay_root,device)
        for page in db.execute('SELECT * FROM pages ORDER BY id').fetchall():
            pid=page['id'];snapshot=json.loads((root/'inputs'/f'{pid}.json').read_text())
            sources={i['id']:i for i in snapshot['items']}
            stored=list(db.execute('SELECT * FROM source_items WHERE page_id=?',(pid,)))
            if set(sources)!={r['id'] for r in stored}: raise AssertionError('Source item identities changed')
            source_union=set();body_by_sub={};body_pixels=0
            for row in stored:
                expected=sources[row['id']];runs=unpack_runs(row['runs'])
                if runs!=expected['runs'] or row['word_id']!=expected['word_id'] or row['subword_id']!=expected['subword_id']:
                    raise AssertionError('A source mask or existing word/subword owner changed')
                pp=set(pixels(runs).tolist())
                if len(pp)!=row['area'] or source_union & pp: raise AssertionError('Source mask overlap or wrong area')
                source_union |= pp
                if row['kind'] in BODY:
                    body_by_sub.setdefault(row['subword_id'],set()).update(pp);body_pixels+=len(pp)
            region_union={};raw_pixels=0;unknown_pixels=0
            for row in db.execute('SELECT * FROM regions WHERE page_id=?',(pid,)):
                pp=set(pixels(unpack_runs(row['runs'])).tolist())
                raw=set(pixels(unpack_runs(row['raw_runs'])).tolist())
                union=region_union.setdefault(row['subword_id'],set())
                if union & pp or not raw<=pp or len(pp)!=row['area'] or len(raw)!=row['raw_area']:
                    raise AssertionError('Regions overlap, lose raw ink, or have wrong measurements')
                union.update(pp);raw_pixels+=len(raw)
                unknown_pixels+=len(pp) if row['role']=='unknown' else 0
            if region_union!=body_by_sub: raise AssertionError('Body ink was lost or invented')
            single_bodies=0
            for sub in db.execute('SELECT * FROM subwords WHERE page_id=? AND expected_letters=1',(pid,)):
                if not body_by_sub.get(sub['id']): continue  # Missing source ink remains unresolved.
                pieces=db.execute('SELECT * FROM regions WHERE subword_id=?',(sub['id'],)).fetchall()
                diagnostic=json.loads(sub['diagnostics'])
                if (len(pieces)!=1 or pieces[0]['role']!='body' or not pieces[0]['letter_id'] or
                        pieces[0]['runs']!=pieces[0]['raw_runs'] or
                        sub['alignment']!='single-base-source' or diagnostic['modelApplied'] or
                        diagnostic['predictionKey'] is not None):
                    raise AssertionError(f"Single-base body was segmented: {sub['id']}")
                single_bodies+=1
            native_count=sum(c['text'] in BODY_LETTERS for w in snapshot['words']
                             for c in snapshot['transcript'][w['id']]['characters'])
            actual_count=db.execute('SELECT COUNT(*) FROM letters WHERE page_id=?',(pid,)).fetchone()[0]
            if native_count!=actual_count: raise AssertionError('Logical letters were silently omitted')
            # These labels are deliberately wrong. They must not affect masks,
            # identities, quotas, assignment costs, or saved mark ownership.
            changed=copy.deepcopy(snapshot)
            for item in changed['items']:
                item.update(final_kind='wrong',final_type='wrong',prediction_kind='wrong',
                            prediction_type='wrong',model_confidence=-999,teacher_type='irrelevant')
            regenerated=build_page(changed,predictor)
            check_equal(db,regenerated,pid)
            summary=dict(page=pid,sourceItems=len(stored),sourcePixels=len(source_union),
                bodyPixels=body_pixels,rawBodyPixels=raw_pixels,unassignedBodyPixels=unknown_pixels,
                transcriptLetters=native_count,regions=len(regenerated['regions']),
                originalSingleLetterBodies=single_bodies,
                marks=len(regenerated['marks']),expectedMarks=len(regenerated['expected_marks']),
                markStatus=dict(Counter(m['status'] for m in regenerated['marks'])),
                learnedClassifiersCannotChangeOutput=True,identicalReplay=True,
                modelSha256=predictor.settings['modelSha256'])
            report['pages'].append(summary)
            print(json.dumps(summary),flush=True)
    if digest_file(root/'corpus.sqlite')!=original_hash: raise AssertionError('Verification changed the corpus')
    if review_hash and digest_file(review_path)!=review_hash: raise AssertionError('Verification changed reviews')
    report.update(databaseSha256=original_hash,reviewsUnchanged=True,exactSourceMasks=True,
                  exactWordAndSubwordOwners=True,disjointBodyPartition=True)
    (root/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus',type=Path,default=ROOT/'data/automatic/letter-corpus')
    parser.add_argument('--fresh-inference',action='store_true')
    args=parser.parse_args();verify(args.corpus,args.fresh_inference)
