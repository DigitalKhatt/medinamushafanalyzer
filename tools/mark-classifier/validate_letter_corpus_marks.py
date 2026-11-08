#!/usr/bin/env python3
"""Compare transcript-derived mark associations to a classifier, after assignment."""
import argparse
from collections import Counter
from contextlib import closing
import json
from pathlib import Path
import sqlite3

from automatic.common import unpack_runs

ROOT=Path(__file__).resolve().parent


def validate(root, against):
    with closing(sqlite3.connect(root/'corpus.sqlite')) as db, \
            closing(sqlite3.connect(against.resolve().as_uri()+'?mode=ro',uri=True)) as classifier:
        db.row_factory=classifier.row_factory=sqlite3.Row
        inference=classifier.execute("SELECT value FROM meta WHERE key='inference'").fetchone()
        provenance=json.dumps(dict(source=str(against.resolve()),inference=json.loads(inference[0]) if inference else None,
            policy='Validation only. Classifier values cannot change body masks, transcript slots, or ownership.'))
        records=[]
        for row in db.execute('''SELECT m.id,m.kind,s.runs,t.expected_type,t.status transcript_status
                FROM marks m JOIN source_items s ON s.id=m.id
                LEFT JOIN mark_transcripts t ON t.mark_id=m.id'''):
            prediction=classifier.execute('SELECT runs,prediction_kind,prediction_type,model_confidence FROM items WHERE id=?',(row['id'],)).fetchone()
            verdict='unresolved-transcript'
            if not prediction:verdict='classifier-unavailable'
            elif unpack_runs(prediction['runs'])!=unpack_runs(row['runs']):verdict='source-mask-changed'
            elif row['expected_type']:
                verdict='agreement' if (prediction['prediction_kind'],prediction['prediction_type'])==(row['kind'],row['expected_type']) else 'difference'
            records.append((row['id'],prediction['prediction_kind'] if prediction else None,
                prediction['prediction_type'] if prediction else None,prediction['model_confidence'] if prediction else None,
                row['expected_type'],verdict,provenance))
        with db:
            db.execute('DELETE FROM mark_validation')
            db.executemany('INSERT INTO mark_validation VALUES (?,?,?,?,?,?,?)',records)
        report=dict(validationOnly=True,assignmentUsesClassifier=False,counts=dict(Counter(r[5] for r in records)),
            differences=[dict(id=r[0],expected=r[4],classifier=r[2],confidence=r[3]) for r in records if r[5]=='difference'])
    (root/'classification-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(report['counts']))
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus',type=Path,default=ROOT/'data/automatic/letter-corpus')
    parser.add_argument('--against',type=Path,default=ROOT/'data/automatic/corpus.sqlite')
    args=parser.parse_args();validate(args.corpus,args.against)
