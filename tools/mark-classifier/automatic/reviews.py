"""Record an explicitly supplied human review of a fixed list of corpus masks.

This module is deliberately outside automatic proposal generation and training.
The manifest is an attestation, not a confidence-based auto-approval rule.
"""
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
import zlib

from .common import merge_runs,unpack_runs


def mask_digest(runs):
    normalized=merge_runs([{'runs':runs}])
    return hashlib.sha256(json.dumps(normalized,separators=(',',':')).encode()).hexdigest()


def mask_agreement(a,b):
    # Used only for explicitly documented render differences on a reviewed region.
    a=set(p for s,n in zip(a[::2],a[1::2]) for p in range(s,s+n))
    b=set(p for s,n in zip(b[::2],b[1::2]) for p in range(s,s+n))
    return len(a&b)/max(1,len(a|b))


def make_manifest(root,item_ids,statement,batch_id):
    """Freeze exact items for a review already performed by the human user."""
    if not statement.strip() or not item_ids or len(set(item_ids))!=len(item_ids):
        raise ValueError('An explicit human review statement and unique item IDs are required')
    root=Path(root);items=[]
    with closing(sqlite3.connect(root/'corpus.sqlite')) as db:
        db.row_factory=sqlite3.Row
        for item_id in item_ids:
            row=db.execute('''SELECT i.*,p.split,p.source_sha,p.proposal_sha FROM items i JOIN pages p USING(page_id)
                WHERE i.id=? AND i.in_scope=1''',(item_id,)).fetchone()
            if row is None:raise ValueError(f'Missing in-scope item: {item_id}')
            payload=json.loads(zlib.decompress((root/'pages'/f'{row["page_id"]}.json.zlib').read_bytes()))
            cached=next(i for i in payload['items'] if i['id']==item_id)
            if mask_digest(cached['runs'])!=mask_digest(unpack_runs(row['runs'])):raise ValueError('Cached source mask changed')
            items.append({'itemId':item_id,'pageId':row['page_id'],'candidateId':cached['candidate_id'],
                'role':row['teacher_kind'],'markType':row['teacher_type'],'split':row['split'],
                'sourceSha256':row['source_sha'],'proposalSha256':row['proposal_sha'],
                'maskSha256':mask_digest(cached['runs'])})
    return {'schemaVersion':1,'batchId':batch_id,'attribution':{
        'reviewedBy':'user','recordedBy':'assistant','basis':'explicit-user-attestation',
        'statement':statement,'recordedAt':datetime.now(timezone.utc).isoformat(),'batchId':batch_id},'items':items}


def source_document(store,root,page_id,proposal,db):
    """Initialize absent pages from the corpus's exact source ink, never a rerender."""
    doc=store.annotation(page_id)
    if doc:return doc
    import numpy as np
    from .build import components
    page=dict(db.execute('SELECT * FROM pages WHERE page_id=?',(page_id,)).fetchone())
    raster=proposal['raster'];mask=np.zeros((raster['height'],raster['width']),dtype='u1')
    for row in db.execute('SELECT runs FROM items WHERE page_id=?',(page_id,)):
        runs=unpack_runs(row['runs'])
        for start,length in zip(runs[::2],runs[1::2]):
            if mask.ravel()[start:start+length].any():raise ValueError('Overlapping source masks')
            mask.ravel()[start:start+length]=1
    if int(mask.sum())!=page['ink_pixels']:raise ValueError('Source ink coverage changed')
    return {'schemaVersion':1,'characterMetadataVersion':1,'pageId':page_id,'sourceSha256':page['source_sha'],
        'raster':raster,'inkPixels':int(mask.sum()),'parts':components(mask),'groups':{},'nextGroup':1,'revision':0,'lines':[],
        'sourceInkProvenance':{'sourceKind':page['source_kind'],'pdfSha256':page['pdf_sha'],
            'note':'Exact existing corpus pixels imported for review; no rasterization or inferred labels.'}}


def preflight(store,root,manifest,db):
    from review_bridge import reconcile,proposal_digest
    from .scope import load_scope,scoped_review
    if manifest.get('schemaVersion')!=1 or not manifest.get('attribution',{}).get('statement'):
        raise ValueError('A versioned explicit human review manifest is required')
    items=manifest.get('items',[])
    if not items or len({i['itemId'] for i in items})!=len(items):raise ValueError('Review IDs must be nonempty and unique')
    plans={}
    for target in items:
        page_id=target['pageId'];store.source(page_id)
        row=db.execute('''SELECT i.*,p.split,p.source_sha,p.proposal_sha FROM items i JOIN pages p USING(page_id)
            WHERE i.id=?''',(target['itemId'],)).fetchone()
        if row is None or not row['in_scope'] or row['page_id']!=page_id:raise ValueError('Reviewed item is missing or outside scope')
        if (row['source_sha'],row['proposal_sha'],row['split'])!=(target['sourceSha256'],target['proposalSha256'],target['split']):
            raise ValueError('Source, proposal snapshot or page split changed after the review')
        if (row['teacher_kind'],row['teacher_type'])!=(target['role'],target['markType']):raise ValueError('Original proposal label changed')
        if mask_digest(unpack_runs(row['runs']))!=target['maskSha256']:raise ValueError('Reviewed mask changed')
        if page_id not in plans:
            proposal=json.loads((root/'proposals'/f'{page_id}.json').read_text())
            if proposal_digest(proposal)!=target['proposalSha256']:raise ValueError('Proposal snapshot changed')
            doc=source_document(store,root,page_id,proposal,db)
            key='automatic:'+target['proposalSha256']
            view={**doc,'proposalReview':doc.get('proposalReviewSets',{}).get(key,{})}
            aligned=scoped_review(reconcile(view,proposal),doc,load_scope(root,page_id,target['sourceSha256']))
            plans[page_id]={'doc':doc,'aligned':aligned,'targets':[]}
        plan=plans[page_id]
        candidate=next((c for c in plan['aligned']['candidates'] if c['id']==target['candidateId']),None)
        if not candidate or not candidate['canApply'] or candidate.get('inScope') is False:raise ValueError('Review geometry is unresolved')
        selected=[p for p in plan['doc']['parts'] if p['id'] in candidate['regionIds']]
        workbench_runs=merge_runs(selected)
        if mask_digest(workbench_runs)!=target['maskSha256']:
            # Different existing rasterizations may represent the same SVG path.
            # Both hashes must be explicitly frozen; never move either mask.
            if mask_digest(workbench_runs)!=target.get('workbenchMaskSha256') or mask_agreement(workbench_runs,unpack_runs(row['runs']))<.90:
                raise ValueError('Workbench and reviewed corpus masks differ')
        already=(candidate.get('review') or {}).get('decision')=='approved'
        if already:
            if any(not p['reviewed'] or p['role']!=target['role'] or p.get('markType')!=target['markType'] for p in selected):
                raise ValueError('A different human label already exists')
        elif candidate['protected']:raise ValueError('Existing human annotation is protected')
        plan['targets'].append((target,already))
    return plans


def record(store,root,manifest,progress=lambda *args:None):
    """Preflight every mask, save normal annotation history, then sync exact labels."""
    import segment
    root=Path(root)
    with closing(sqlite3.connect(root/'corpus.sqlite',timeout=60)) as db:
        db.row_factory=sqlite3.Row
        plans=preflight(store,root,manifest,db)
        # Persist the authorization before mutations. Interrupted runs can resume;
        # existing matching approvals are retained, never silently overwritten.
        segment.atomic_json(root/'human-reviews'/f'{manifest["batchId"]}.json',manifest)
        updates=[]
        for page_id,plan in plans.items():
            doc=plan['doc']
            if not store.annotation(page_id):doc=store.save(page_id,doc)
            for target,already in plan['targets']:
                if not already:
                    store.review(page_id,{'revision':doc['revision'],'action':'approve','id':target['candidateId'],
                        'proposalSource':'automatic','proposalSha256':target['proposalSha256'],
                        'role':target['role'],'markType':target['markType'],'reviewAttribution':manifest['attribution']})
                doc=store.annotation(page_id)
                approved=[p for p in doc['parts'] if p.get('proposalSource',{}).get('candidateId')==target['candidateId']
                    and p.get('proposalSource',{}).get('proposalSha256')==target['proposalSha256']]
                if len(approved)!=1 or mask_digest(approved[0]['runs'])!=target.get('workbenchMaskSha256',target['maskSha256']):raise ValueError('Saved review mask mismatch')
                updates.append((target,approved[0],doc['revision']))
            progress(page_id,len(updates),len(manifest['items']))
        with db:
            columns={r['name'] for r in db.execute('PRAGMA table_info(items)')}
            for name in ('proposal_origin','review_attribution','final_source'):
                if name not in columns:db.execute(f'ALTER TABLE items ADD COLUMN {name} TEXT')
            for target,part,revision in updates:
                db.execute('''UPDATE items SET proposal_origin=COALESCE(proposal_origin,origin),origin='human-reviewed',human_reviewed=1,
                    kind=?,mark_type=?,review_attribution=?,final_kind=?,final_type=?,final_source='human-reviewed' WHERE id=?''',
                    (part['role'],part['markType'],json.dumps(part.get('reviewAttribution')),part['role'],part['markType'],target['itemId']))
                db.execute('UPDATE pages SET annotation_revision=? WHERE page_id=?',(revision,target['pageId']))
        # Keep replaceable build caches aligned, while retaining original teacher
        # fields, evidence, IDs, feature rows and every source pixel.
        for page_id,plan in plans.items():
            path=root/'pages'/f'{page_id}.json.zlib'
            payload=json.loads(zlib.decompress(path.read_bytes()))
            for item in payload['items']:
                for target,part,revision in updates:
                    if item['id']==target['itemId']:
                        item.update(kind=part['role'],mark_type=part['markType'],human_reviewed=1,
                            proposal_origin=item.get('proposal_origin') or item['origin'],origin='human-reviewed',
                            review_attribution=json.dumps(part.get('reviewAttribution')))
                        payload['page']['annotation_revision']=revision
            temporary=path.with_suffix('.tmp')
            temporary.write_bytes(zlib.compress(json.dumps(payload,ensure_ascii=False,separators=(',',':')).encode(),5));temporary.replace(path)
    return {'batchId':manifest['batchId'],'reviewed':len(updates),'pages':len(plans)}
