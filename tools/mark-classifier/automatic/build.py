"""Build a source-conserving, machine-labelled corpus without modifying reviews."""
from __future__ import annotations
from concurrent.futures import ProcessPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import zlib

from .common import FEATURE_DIM,bounding_box,digest_file,feature_vector,ink_components,merge_runs,pack_runs,plain


def components(mask):
    import cv2
    import numpy as np
    _,labels,stats,_=cv2.connectedComponentsWithStats(mask,connectivity=8)
    flat=labels.ravel(); starts=np.flatnonzero(np.r_[True,flat[1:]!=flat[:-1]])
    lengths=np.diff(np.r_[starts,flat.size]);runs=[[] for _ in range(len(stats))]
    for start,length,label in zip(starts,lengths,flat[starts]):
        if label:runs[int(label)].extend((int(start),int(length)))
    parts=[]
    for label in range(1,len(stats)):
        x,y,w,h,area=map(int,stats[label])
        parts.append({'id':f'part-{label:05d}','runs':runs[label],'bbox':[x,y,w,h],'area':area,
            'role':'unknown','reviewed':False,'owner':None,'line':None})
    return parts


def quality(candidate,word):
    kind=candidate['kind']
    if kind=='unknown':return 0.
    match=candidate['match']; q=min(match['sourceCoverage'],match['proposalCoverage'],1)
    if kind=='subword-ink':q*=.72
    elif kind=='dot':q*=.96
    elif candidate.get('shapeCost') is not None:q*=max(.18,.98-candidate['shapeCost'])
    elif candidate['markType']=='verse-number':q*=.94
    else:q*=.60
    if word and word['legacyFlags']!=1:q*=.48
    if any('unmatched' in s.lower() or 'not assigned' in s.lower() for s in candidate['issues']):q*=.4
    return round(q,5)


def _page(task):
    import cv2
    import numpy as np
    from PIL import Image
    import segment
    from review_bridge import reconcile
    cv2.setNumThreads(1)
    page_id,data_root,output_root,pdf_root,pdftoppm,proposal_root=task
    data_root,output_root,pdf_root=map(Path,(data_root,output_root,pdf_root))
    proposal_path=Path(proposal_root)/f'{page_id}.json'
    proposal=json.loads(proposal_path.read_text())
    prior_cache=output_root/'pages'/f'{page_id}.json.zlib'
    prior_reviews={}
    if prior_cache.exists():
        prior=json.loads(zlib.decompress(prior_cache.read_bytes()))
        if prior['page']['source_sha']==proposal['sourceSha256']:
            prior_reviews={tuple(i['runs']):i['id'] for i in prior['items'] if i['human_reviewed']}
    review_proposals=json.loads((data_root/'proposals'/f'{page_id}.json').read_text())
    review_map={tuple(sorted(c['pathIds'])):c['id'] for c in review_proposals['candidates'] if c['pathIds']}
    raster=proposal['raster'];width,height=raster['width'],raster['height']
    pdf=pdf_root/f'{int(page_id[4:]):03d}___Hafs39__DM.ai'
    pdf_sha=digest_file(pdf)
    if pdf_sha!=proposal['provenance']['pdfSha256']:raise ValueError(f'{page_id}: source PDF changed')
    render=output_root/'render'/f'{page_id}.png'
    render_meta=render.with_suffix('.json')
    expected_meta={'pdfSha256':pdf_sha,'width':width,'height':height,'renderer':'pdftoppm'}
    if not render.exists() or not render_meta.exists() or json.loads(render_meta.read_text())!=expected_meta:
        subprocess.run([pdftoppm,'-png','-singlefile','-cropbox','-scale-to-x',str(width),'-scale-to-y',str(height),str(pdf),str(render.with_suffix(''))],check=True,capture_output=True)
        segment.atomic_json(render_meta,expected_meta)
    rgb=np.asarray(Image.open(render).convert('RGB'))
    if rgb.shape[:2]!=(height,width):raise ValueError('PDF rendering dimensions changed')
    source=((rgb.mean(axis=2)<raster['threshold'])&((rgb.max(axis=2).astype('i2')-rgb.min(axis=2))<=raster['chroma'])).astype('u1')
    annotation_path=data_root/'annotations'/f'{page_id}.json'
    if annotation_path.exists():
        doc=json.loads(annotation_path.read_text())
        if doc['sourceSha256']!=proposal['sourceSha256'] or doc['raster']!=raster:raise ValueError(f'{page_id}: annotation source mismatch')
        segment.validate_annotation(doc,{'id':page_id,'sha256':proposal['sourceSha256'],'viewBox':raster['viewBox']})
        source[:]=0
        for part in doc['parts']:
            for s,n in zip(part['runs'][::2],part['runs'][1::2]):source.ravel()[s:s+n]=1
        source_kind='frozen-workbench-ink'
    else:
        parts=components(source)
        doc={'schemaVersion':1,'characterMetadataVersion':1,'pageId':page_id,'sourceSha256':proposal['sourceSha256'],
            'raster':raster,'inkPixels':int(source.sum()),'parts':parts,'groups':{},'nextGroup':1,'revision':0,'lines':[]}
        source_kind='paired-original-pdf'
    from .scope import ensure_assets
    svg_root=Path(json.loads((data_root/'corpus.json').read_text())['sourceDir'])
    scope=ensure_assets(output_root,(svg_root/f'{page_id}.svg').read_bytes(),page_id,width,height,proposal['sourceSha256'])
    allowed=np.zeros(source.size,dtype='u1')
    for start,length in zip(scope['runs'][::2],scope['runs'][1::2]):allowed[start:start+length]=1
    feature_source=source*allowed.reshape(source.shape)
    matched=reconcile(doc,proposal)
    parts={p['id']:p for p in doc['parts']};words={w['id']:w for w in proposal['words']};subs={s['id']:s for s in proposal['subwords']}
    for sid,record in doc.get('subwords',{}).items():
        if sid not in subs:subs[sid]={'id':sid,'text':record['text'],'wordId':record.get('wordId'),'line':record.get('line'),
            'characters':[],'issues':[],'bodyCandidateIds':[],'candidateIds':[]}
    path_index={p['id']:p for p in proposal['paths']}
    primary_boxes={}
    for sub in proposal['subwords']:
        for cid in sub['bodyCandidateIds'][:1]:
            c=next((c for c in matched['candidates'] if c['id']==cid),None)
            if c and c['regionIds']:primary_boxes.setdefault(sub['line'],[]).append(bounding_box([parts[i] for i in c['regionIds']]))
    baselines={line:float(np.median([b[1]+b[3] for b in boxes])) for line,boxes in primary_boxes.items()}
    baselines.update({w['line']:w['baseline'] for w in proposal['words'] if w.get('baseline',0)>0})
    claimed=set();items=[];features=[]
    def add(cid,selected,kind,mark_type,origin,q,candidate=None,human=False):
        if not selected:return
        runs=merge_runs(selected);bbox=bounding_box(selected);x,y,w,h=bbox
        candidate=candidate or {};sid=candidate.get('subwordId');wid=candidate.get('wordId')
        if (human or origin=='human-draft') and selected[0].get('subwordId') in subs:sid=selected[0]['subwordId'];wid=subs[sid]['wordId']
        sub=subs.get(sid,{});word=words.get(wid,{})
        line=candidate.get('line') or selected[0].get('line');base=baselines.get(line,y+h)
        if any(p['id'] in claimed for p in selected):raise ValueError('Automatic masks overlap')
        claimed.update(p['id'] for p in selected)
        contours=sum(path_index[p]['contourCount'] for p in candidate.get('pathIds',[]) if p in path_index) or len(selected)
        # Fresh detector parts are connected components. Saved annotation parts
        # may have been merged or split, so their count is not ink topology.
        component_count=ink_components(runs,bbox,width) if source_kind=='frozen-workbench-ink' else len(selected)
        item={'id':page_id+':'+cid,'candidate_id':candidate.get('id'), 'page_id':page_id,'word_id':page_id+':'+wid if wid else None,
            'subword_id':page_id+':'+sid if sid else None,'line':line,'kind':kind,'mark_type':mark_type,'text':sub.get('text',word.get('text','')),
            'bases':plain(sub.get('text',word.get('text',''))),'x':x,'y':y,'w':w,'h':h,'area':sum(runs[1::2]),
            'components':component_count,'contours':contours,'quality':q,'origin':origin,'human_reviewed':int(human),
            'review_candidate':review_map.get(tuple(sorted(candidate.get('pathIds',[])))) if candidate.get('pathIds') else None,
            'issues':json.dumps(candidate.get('issues',[]),ensure_ascii=False),'teacher_kind':kind,'teacher_type':mark_type,
            'runs':runs,'region_ids':[p['id'] for p in selected],'proposal_origin':None,'review_attribution':None}
        if human:
            item['id']=prior_reviews.get(tuple(runs),item['id'])
            # Keep the program's original evidence distinct from the human target.
            item.update(teacher_kind=candidate.get('kind','unknown'),teacher_type=candidate.get('markType','unknown'),
                quality=quality(candidate,word) if candidate else 0.,
                proposal_origin='machine-weak' if candidate else 'unresolved',
                review_attribution=json.dumps(selected[0].get('reviewAttribution'),ensure_ascii=False))
        items.append(item);features.append(feature_vector(runs,bbox,feature_source,base,component_count,raster['scale']))
    # Use each source component once. Geometry failures stay unresolved, never negatives.
    for c in matched['candidates']:
        if not c['canApply'] or c['id'].startswith('source:'):continue
        selected=[parts[i] for i in c['regionIds'] if i not in claimed]
        frozen=[p for p in selected if p['reviewed'] or p['role']!='unknown' or p.get('owner')]
        for part in frozen:
            reviewed=part['reviewed'];kind=part['role'] if reviewed else 'unknown'
            add('human:'+part['id'],[part],kind,part.get('markType','unknown'),'human-reviewed' if reviewed else 'human-draft',1. if reviewed else 0.,c,reviewed)
        selected=[p for p in selected if p['id'] not in claimed]
        if selected:
            q=quality(c,words.get(c.get('wordId')))
            if frozen:q*=.3
            add(c['id'],selected,c['kind'],c['markType'],'machine-weak',q,c)
    for part in doc['parts']:
        if part['id'] not in claimed:
            human=part['reviewed'];draft=part['role']!='unknown' or bool(part.get('owner'))
            add('source:'+part['id'],[part],part['role'] if human else 'unknown',part.get('markType','unknown'),
                'human-reviewed' if human else 'human-draft' if draft else 'unresolved',1. if human else 0.,human=human)
    if claimed!=set(parts):raise ValueError('Automatic corpus dropped source components')
    total=sum(item['area'] for item in items)
    if total!=doc['inkPixels']:raise ValueError(f'{page_id}: automatic ink coverage changed')
    # Verify exact union, not only the pixel count.
    seen=np.zeros(source.size,dtype='u1')
    for item in items:
        for s,n in zip(item['runs'][::2],item['runs'][1::2]):
            if np.any(seen[s:s+n]):raise ValueError('Automatic masks overlap in pixel space')
            seen[s:s+n]=1
    if not np.array_equal(seen,source.ravel()):raise ValueError('Automatic masks moved source ink')
    page={'page_id':page_id,'split':segment.split_for(page_id),'source_sha':proposal['sourceSha256'],'pdf_sha':pdf_sha,'width':width,'height':height,
        'ink_pixels':total,'matched_pixels':matched['alignment']['matchedInkPixels'],
        'unknown_pixels':sum(i['area'] for i in items if i['kind']=='unknown'),'component_count':len(parts),
        'source_kind':source_kind,'proposal_sha':matched['proposalSha256'],'annotation_revision':doc['revision']}
    word_rows=[]
    for w in proposal['words']:
        paths=[path_index[pid] for pid in w['pathIds']];b=bounding_box(paths)
        word_rows.append({'id':page_id+':'+w['id'],'page_id':page_id,'line':w['line'],'ordinal':w['index'],
            'text':w['text'],'bases':plain(w['text']),'x':int(b[0]),'y':int(b[1]),'w':int(b[2]+1),'h':int(b[3]+1),
            'flags':w['legacyFlags'],'transcript_matches':int(w['transcriptMatches'])})
    sub_rows=[]
    for s in subs.values():
        members=[i for i in items if i['subword_id']==page_id+':'+s['id']]
        body=[i for i in members if i['kind'] in ('subword-ink','body','kashida')]
        b=bounding_box([{'bbox':[i['x'],i['y'],i['w'],i['h']]} for i in (body or members)])
        sub_rows.append({'id':page_id+':'+s['id'],'page_id':page_id,'word_id':page_id+':'+s['wordId'] if s.get('wordId') else None,'line':s['line'],
            'text':s['text'],'bases':plain(s['text']),'x':int(b[0]),'y':int(b[1]),'w':int(b[2]),'h':int(b[3]),
            'expected':json.dumps(s['characters'],ensure_ascii=False),'issues':json.dumps(s['issues']+([] if body else ['No resolved body ink']),ensure_ascii=False)})
    # Worker cache is replaceable machine output; source annotations are never written.
    payload={'page':page,'items':items,'words':word_rows,'subwords':sub_rows}
    target=output_root/'pages'/f'{page_id}.json.zlib'
    temporary=target.with_suffix('.tmp');temporary.write_bytes(zlib.compress(json.dumps(payload,ensure_ascii=False,separators=(',',':')).encode(),5));temporary.replace(target)
    np.save(output_root/'pages'/f'{page_id}.features.npy',np.asarray(features,dtype='u1'))
    return {'pageId':page_id,'items':len(items),'inkPixels':total,'unknownPixels':page['unknown_pixels'],'humanReviewed':sum(i['human_reviewed'] for i in items)}


def insert(db,table,rows):
    if not rows:return
    columns=list(rows[0]);sql=f"INSERT INTO {table} ({','.join(columns)}) VALUES ({','.join('?' for _ in columns)})"
    db.executemany(sql,([row.get(c) for c in columns] for row in rows))


def build(data_root,output_root,pdf_root,pages,jobs,progress,proposal_root=None,rebuild_pages=None):
    import numpy as np
    data_root,output_root,pdf_root=map(Path,(data_root,output_root,pdf_root))
    pdftoppm=shutil.which('pdftoppm') or '/opt/homebrew/bin/pdftoppm'
    for folder in ['render','pages','models']: (output_root/folder).mkdir(parents=True,exist_ok=True)
    corpus=json.loads((data_root/'corpus.json').read_text());source=Path(corpus['sourceDir']);page_index={p['id']:p for p in corpus['pages']}
    for page_id in pages:
        if digest_file(source/page_index[page_id]['filename'])!=page_index[page_id]['sha256']:raise ValueError(f'{page_id}: source SVG changed')
    proposal_root=Path(proposal_root) if proposal_root else data_root/'proposals'
    if rebuild_pages is not None:
        for p in pages:
            if p not in rebuild_pages and not all((output_root/'pages'/f'{p}{suffix}').exists() for suffix in ('.json.zlib','.features.npy')):
                raise ValueError(f'Missing reusable page cache: {p}')
    tasks=[(p,str(data_root),str(output_root),str(pdf_root),pdftoppm,str(proposal_root)) for p in pages if rebuild_pages is None or p in rebuild_pages]
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for n,result in enumerate(pool.map(_page,tasks),1):progress('build',n,len(pages),result)
    target=output_root/'corpus.sqlite';temporary=output_root/'corpus.building.sqlite'
    for suffix in ['','-shm','-wal']:
        path=Path(str(temporary)+suffix)
        if path.exists():path.unlink()
    db=sqlite3.connect(temporary);db.executescript((Path(__file__).parent/'schema.sql').read_text())
    row=0;summary={'pages':len(pages),'items':0,'inkPixels':0,'unknownPixels':0,'humanReviewed':0,'classes':{},'allSourceInkConserved':True}
    with (output_root/'features.u8').open('wb') as feature_file:
        for page_id in pages:
            payload=json.loads(zlib.decompress((output_root/'pages'/f'{page_id}.json.zlib').read_bytes()))
            insert(db,'pages',[payload['page']]);insert(db,'words',payload['words']);insert(db,'subwords',payload['subwords'])
            records=[]
            for item in payload['items']:
                item.pop('candidate_id',None);item.pop('region_ids',None);item['runs']=pack_runs(item['runs']);item['feature_row']=row;row+=1
                # Preparation supplies training examples, not final predictions.
                item.update(final_kind=item['kind'] if item['human_reviewed'] else 'unknown',
                    final_type=item['mark_type'] if item['human_reviewed'] else 'unknown',refined=0)
                records.append(item);summary['classes'][item['kind']]=summary['classes'].get(item['kind'],0)+1
                summary['humanReviewed']+=item['human_reviewed']
            insert(db,'items',records)
            features=np.load(output_root/'pages'/f'{page_id}.features.npy');feature_file.write(features.tobytes())
            summary['inkPixels']+=payload['page']['ink_pixels'];summary['unknownPixels']+=payload['page']['unknown_pixels']
            db.commit()
    summary['items']=row;summary['featureDim']=FEATURE_DIM
    db.execute("UPDATE pages SET unknown_pixels=COALESCE((SELECT SUM(area) FROM items i INDEXED BY item_page WHERE i.page_id=pages.page_id AND i.final_kind='unknown'),0)")
    db.execute('INSERT INTO meta VALUES (?,?)',('build',json.dumps(summary)))
    db.commit();db.execute('ANALYZE');db.execute('PRAGMA wal_checkpoint(TRUNCATE)');db.close()
    from .transcript_marks import replay_existing_index
    replay_existing_index(target, temporary)
    temporary.replace(target)
    return summary
