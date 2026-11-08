"""Freeze training targets independently of fitting and corpus inference.

Both machine proposals and reviewed regions use the same target record. Their
provenance stays distinct; drafts and out-of-scope ink have no training target.
"""
from collections import Counter
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3


def digest_file(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()


def geometry_digest(db):
    """Bind targets to source pixels, feature order, scope and page split."""
    h=hashlib.sha256()
    for row in db.execute('SELECT page_id,split,source_sha,width,height,proposal_sha,annotation_revision FROM pages ORDER BY page_id'):
        h.update(json.dumps(list(row),separators=(',',':')).encode());h.update(b'\n')
    for row in db.execute('SELECT id,page_id,feature_row,x,y,w,h,contours,in_scope,runs FROM items ORDER BY feature_row'):
        values=list(row);runs=values.pop()
        h.update(json.dumps(values,separators=(',',':')).encode());h.update(b'\0');h.update(runs);h.update(b'\n')
    scope=db.execute("SELECT value FROM meta WHERE key='scope'").fetchone()
    if scope:h.update(scope[0].encode())
    return h.hexdigest()


def prepare(root,destination=None):
    root=Path(root);destination=Path(destination or root/'training-labels');destination.mkdir(parents=True,exist_ok=True)
    source=sqlite3.connect(f'file:{root / "corpus.sqlite"}?mode=ro',uri=True);source.row_factory=sqlite3.Row
    temporary=destination/'labels.building.sqlite'
    if temporary.exists():temporary.unlink()
    target=sqlite3.connect(temporary)
    target.executescript('''CREATE TABLE targets(item_id TEXT PRIMARY KEY,feature_row INTEGER UNIQUE,page_id TEXT,split TEXT,
        role TEXT,type TEXT,quality REAL,provenance TEXT,human_reviewed INTEGER,bases TEXT);
        CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);''')
    from .weak_corrections import corrections,VERSION
    corrected=corrections(source,root)
    target.execute('CREATE TABLE corrections(item_id TEXT PRIMARY KEY,evidence TEXT NOT NULL)')
    target.executemany('INSERT INTO corrections VALUES (?,?)',[(key,json.dumps(record)) for key,record in corrected.items()])
    counts=Counter();type_roles={};total=0
    try:
        for row in source.execute('''SELECT i.id,i.feature_row,i.page_id,p.split,i.kind,i.mark_type,i.teacher_kind,i.teacher_type,i.quality,
            i.origin,i.human_reviewed,i.in_scope,i.bases FROM items i JOIN pages p USING(page_id) ORDER BY i.feature_row'''):
            human=bool(row['human_reviewed']) and row['origin']=='human-reviewed'
            provenance='human-reviewed' if human else 'automatic' if row['origin']=='machine-weak' else 'unlabelled'
            source_role=row['kind'] if human else row['teacher_kind']
            if not human and row['id'] in corrected:
                provenance='excluded' if corrected[row['id']]['action']=='exclude' else 'automatic'
                source_role=corrected[row['id']]['role']
            eligible=row['in_scope'] and provenance!='unlabelled' and source_role not in (None,'unknown')
            role=source_role if eligible else None
            kind_type=(row['mark_type'] if human else row['teacher_type']) if role else None
            if role and not human and row['id'] in corrected:
                role,kind_type=corrected[row['id']]['role'],corrected[row['id']]['type']
            target.execute('INSERT INTO targets VALUES (?,?,?,?,?,?,?,?,?,?)',(row['id'],row['feature_row'],row['page_id'],row['split'],
                role,kind_type,(1. if human else corrected.get(row['id'],{}).get('quality',row['quality'])) if role else 0.,provenance,int(human),row['bases']))
            counts[f'{provenance}:{row["split"]}:{role or "ignore"}']+=1;total+=1
            if role in ('dot','mark','symbol') and kind_type not in (None,'unknown') and row['split']=='train':
                type_roles.setdefault(kind_type,Counter())[role]+=1
        manifest={'schemaVersion':1,'createdAt':datetime.now(timezone.utc).isoformat(),'items':total,
            'geometrySha256':geometry_digest(source),'featuresSha256':digest_file(root/'features.u8'),
            'typeRoles':{t:c.most_common(1)[0][0] for t,c in sorted(type_roles.items())},'counts':dict(counts),
            'automaticCorrections':{'version':VERSION,'items':len(corrected),
                'actions':dict(Counter(r['action'] for r in corrected.values())),
                'splits':dict(Counter(r['split'] for r in corrected.values())),
                'policy':'Reciprocal swaps, native dammatan form evidence and detached-rule targets; automatic quality weights are not verified accuracy. No model predictions or human review flags.'},
            'geometryWarning':'Region grouping and text links currently come from the vector proposal preparation program; this is not a learned character mask decoder.',
            'policy':'Automatic labels are weak training targets only. Human review flags are never inferred. Null targets are ignored.'}
        target.execute('INSERT INTO meta VALUES (?,?)',('manifest',json.dumps(manifest,ensure_ascii=False)))
        target.commit();target.close();temporary.replace(destination/'labels.sqlite')
        manifest['labelsSha256']=digest_file(destination/'labels.sqlite')
        (destination/'corrections.json').write_text(json.dumps(list(corrected.values()),ensure_ascii=False,indent=2))
        temp=destination/'manifest.building.json';temp.write_text(json.dumps(manifest,ensure_ascii=False,indent=2));temp.replace(destination/'manifest.json')
        return manifest
    finally:source.close();target.close()


def load(root,destination=None):
    root=Path(root);destination=Path(destination or root/'training-labels')
    if not (destination/'manifest.json').exists():raise ValueError('Prepare a training-label snapshot first with prepare_automatic_labels.py --reuse-corpus')
    manifest=json.loads((destination/'manifest.json').read_text())
    if digest_file(destination/'labels.sqlite')!=manifest['labelsSha256']:raise ValueError('Training-label snapshot changed; prepare a new snapshot')
    with closing(sqlite3.connect(f'file:{root / "corpus.sqlite"}?mode=ro',uri=True)) as db:
        if geometry_digest(db)!=manifest['geometrySha256']:raise ValueError('Training labels belong to different regions, scope or page splits; prepare a new snapshot')
    if digest_file(root/'features.u8')!=manifest['featuresSha256']:raise ValueError('Training features differ from the frozen label snapshot')
    with closing(sqlite3.connect(f'file:{destination / "labels.sqlite"}?mode=ro',uri=True)) as db:
        db.row_factory=sqlite3.Row
        rows=[dict(r) for r in db.execute('SELECT * FROM targets ORDER BY feature_row')]
    if len(rows)!=manifest['items'] or [r['feature_row'] for r in rows]!=list(range(len(rows))):raise ValueError('Invalid training feature order')
    return manifest,rows
