"""Separate ink-role recognition from unresolved transcript/ownership assignment.

These are reversible machine proposals. Human annotations and original teacher
labels are never promoted or overwritten by this stage.
"""
from __future__ import annotations
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
import json
from pathlib import Path
import sqlite3
import zlib

FIELDS={'unresolved_reason':'TEXT','resolution_method':'TEXT','resolution_score':'REAL','assignment_status':'TEXT',
        'source_diagnostics':'TEXT','context_bbox':'TEXT','context_text':'TEXT'}


def migrate(db):
    columns={r[1] for r in db.execute('PRAGMA table_info(items)')}
    for name,type in FIELDS.items():
        if name not in columns:db.execute(f'ALTER TABLE items ADD COLUMN {name} {type}')
    db.execute('CREATE INDEX IF NOT EXISTS item_resolution ON items(resolution_method,assignment_status)')
    db.execute('CREATE INDEX IF NOT EXISTS item_unresolved_reason ON items(unresolved_reason)')
    from .scope import browse_indexes
    browse_indexes(db)
    db.commit()


def classify_horizontal_rule(item,page,text_band):
    """Rectangular, detached long rules are a separate geometric symbol family.
    Only rules within the observed Quran text band are proposed as sajda bars;
    matching shapes outside that band are margin rules, not letter connections.
    """
    x,y,w,h=(item[k] for k in ('x','y','w','h'));area=item['area']
    if h<1 or h>6 or w<80 or w/h<25 or area/(w*h)<.96:return None
    if w>page['width']*.32 or x<page['width']*.06 or x+w>page['width']*.94:return None
    if y<page['height']*.12 or y+h>page['height']*.89:return None
    if text_band and x>=text_band[0]-16 and x+w<=text_band[1]+16:return 'sajda-bar'
    return 'margin-rule'


def recognize_unmapped_body(item,diagnostic):
    """A coarse body role is independent of which transcript subword it belongs to.
    Reject thin rules, tiny marks, and components containing competing mark ink.
    No Unicode identity, exact subword boundary or ownership is inferred here.
    """
    if item.get('prediction_kind')!='subword-ink':return False
    if (item.get('model_confidence') or 0)<.995 or (item.get('model_margin') or 0)<.95:return False
    if item['h']<24 or item['w']<6 or item['area']<80 or item['w']/item['h']>=30:return False
    for match in diagnostic.get('matches',[]):
        if match['kind'] in ('dot','mark','symbol') and match['regionCoverage']>=.15:return False
    return True


def _diagnose(task):
    root,page_id=task;root=Path(root)
    from review_bridge import reconcile
    payload=json.loads(zlib.decompress((root/'pages'/f'{page_id}.json.zlib').read_bytes()))
    proposal=json.loads((root/'proposals'/f'{page_id}.json').read_text())
    parts=[]
    for item in payload['items']:
        parts.append({'id':item['id'],'runs':item['runs'],'bbox':[item[k] for k in ('x','y','w','h')],
            'area':item['area'],'role':'unknown','reviewed':False,'owner':None,'line':item.get('line')})
    page=payload['page']
    doc={'schemaVersion':1,'pageId':page_id,'sourceSha256':page['source_sha'],'raster':proposal['raster'],
        'parts':parts,'inkPixels':page['ink_pixels'],'groups':{}}
    matched=reconcile(doc,proposal)
    if matched['proposalSha256']!=page['proposal_sha']:raise ValueError(f'{page_id}: automatic proposals changed; rebuild the corpus before resolving it')
    diagnostics=matched['sourceDiagnostics']
    output={}
    for item in payload['items']:
        if item['teacher_kind']!='unknown':continue
        if item['origin']=='human-draft':diagnostic={'reason':'human-draft','matches':[]}
        elif item['origin']=='machine-weak':diagnostic={'reason':'inventory-unassigned','matches':[]}
        else:diagnostic=diagnostics.get(item['id'],{'reason':'legacy-omission','matches':[]})
        output[item['id']]=diagnostic
    return page_id,output


def context_box(item,page,words,rule=False):
    x,y,w,h=(item[k] for k in ('x','y','w','h'))
    candidates=[]
    if rule:
        for word in words:
            overlap=min(x+w,word['x']+word['w'])-max(x,word['x'])
            if overlap>0 and abs(word['y']-y)<110:candidates.append(word)
    if candidates:
        distance=min(abs(word['y']-y) for word in candidates)
        candidates=[word for word in candidates if abs(word['y']-y)<=distance+28]
        left=min(x,min(a['x'] for a in candidates));top=min(y,min(a['y'] for a in candidates))
        right=max(x+w,max(a['x']+a['w'] for a in candidates));bottom=max(y+h,max(a['y']+a['h'] for a in candidates))
        text=' '.join(word['text'] for word in sorted(candidates,key=lambda a:a['ordinal']))
    else:
        left,top,right,bottom=x,y,x+w,y+h;text=''
        if rule:bottom=max(bottom,y+110)
    pad=28;left=max(0,left-pad);top=max(0,top-pad)
    return [left,top,min(page['width'],right+pad)-left,min(page['height'],bottom+pad)-top],text


def resolve_corpus(root,jobs,progress):
    root=Path(root);db=sqlite3.connect(root/'corpus.sqlite');db.row_factory=sqlite3.Row;migrate(db)
    # Re-evaluate previous machine recoveries in memory; a failed diagnostic
    # pass must not reset an already-published corpus.
    eligible="(final_kind='unknown' OR resolution_method IN ('visual-body','horizontal-rule'))"
    before=dict(db.execute(f"SELECT COUNT(*) items,COALESCE(SUM(area),0) pixels FROM items WHERE {eligible}").fetchone())
    pages=[dict(r) for r in db.execute('SELECT * FROM pages ORDER BY page_id')]
    diagnostic_map={}
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for n,(page,diagnostics) in enumerate(pool.map(_diagnose,((str(root),p['page_id']) for p in pages)),1):
            diagnostic_map.update(diagnostics);progress('diagnose-unresolved',n,len(pages),{'pageId':page,'regions':len(diagnostics)})
    updates=[];changes=[];reasons=Counter();recovered=Counter();blocked_mixed=0
    for page in pages:
        words=[dict(w) for w in db.execute('SELECT * FROM words WHERE page_id=?',(page['page_id'],))]
        text_band=(min(w['x'] for w in words),max(w['x']+w['w'] for w in words)) if words else None
        rows=[dict(r) for r in db.execute(f"SELECT * FROM items WHERE page_id=? AND {eligible}",(page['page_id'],))]
        for item in rows:
            diagnostic=diagnostic_map.get(item['id'],{'reason':'legacy-omission','matches':[]});reason=diagnostic['reason'];reasons[reason]+=1
            kind,type,method,score='unknown','unknown',None,None
            assignment='linked' if item['subword_id'] or item['word_id'] else 'unassigned'
            box,text=context_box(item,page,words)
            if item['origin'] not in ('human-reviewed','human-draft') and not item['human_reviewed']:
                rule=classify_horizontal_rule(item,page,text_band)
                if rule:
                    kind,type,method,score='symbol',rule,'horizontal-rule',1.
                    assignment='not-applicable';box,text=context_box(item,page,words,True)
                elif recognize_unmapped_body(item,diagnostic):
                    kind,type,method,score='subword-ink','unknown','visual-body',item['model_confidence']
                    assignment='text-unassigned' if not item['subword_id'] else 'linked'
                elif item.get('prediction_kind')=='subword-ink' and any(m['kind'] in ('dot','mark','symbol') and m['regionCoverage']>=.15 for m in diagnostic.get('matches',[])):blocked_mixed+=1
            if method:
                recovered['sajda-bar' if type=='sajda-bar' else 'margin-rule' if type=='margin-rule' else 'body-ink']+=1
                changes.append({'id':item['id'],'beforeRole':'unknown','afterRole':kind,'type':type,'method':method,'assignment':assignment})
            updates.append((kind,type,reason,method,score,assignment,json.dumps(diagnostic,ensure_ascii=False),json.dumps(box),text,item['id']))
    db.executemany('UPDATE items SET final_kind=?,final_type=?,unresolved_reason=?,resolution_method=?,resolution_score=?,assignment_status=?,source_diagnostics=?,context_bbox=?,context_text=? WHERE id=?',updates)
    # Keep the historical reason available for all original unknown targets,
    # including regions already recovered by transcript refinement.
    for id,diagnostic in diagnostic_map.items():
        db.execute('UPDATE items SET unresolved_reason=COALESCE(unresolved_reason,?),source_diagnostics=COALESCE(source_diagnostics,?) WHERE id=?',
            (diagnostic['reason'],json.dumps(diagnostic,ensure_ascii=False),id))
    db.execute("UPDATE items SET assignment_status=CASE WHEN subword_id IS NOT NULL OR word_id IS NOT NULL THEN 'linked' ELSE 'not-applicable' END WHERE assignment_status IS NULL")
    db.execute("UPDATE pages SET unknown_pixels=COALESCE((SELECT SUM(area) FROM items i INDEXED BY item_page WHERE i.page_id=pages.page_id AND i.final_kind='unknown'),0)")
    if 'scope_unknown_pixels' in {r[1] for r in db.execute('PRAGMA table_info(pages)')}:
        db.execute("UPDATE pages SET scope_unknown_pixels=COALESCE((SELECT SUM(area) FROM items i INDEXED BY item_page WHERE i.page_id=pages.page_id AND i.in_scope=1 AND i.final_kind='unknown'),0) WHERE scope_ink_pixels IS NOT NULL")
    after=dict(db.execute("SELECT COUNT(*) items,COALESCE(SUM(area),0) pixels FROM items WHERE final_kind='unknown'").fetchone())
    remaining=[dict(r) for r in db.execute("SELECT unresolved_reason reason,COUNT(*) items,SUM(area) pixels FROM items WHERE final_kind='unknown' GROUP BY unresolved_reason ORDER BY pixels DESC")]
    mismatches=db.execute('SELECT COUNT(*) FROM pages p WHERE ink_pixels<>(SELECT SUM(area) FROM items i WHERE i.page_id=p.page_id)').fetchone()[0]
    if mismatches:raise ValueError('Resolution changed source coverage')
    summary={'before':before,'after':after,'recovered':dict(recovered),'remainingReasons':remaining,'blockedMixedRoleComponents':blocked_mixed,
        'masksChanged':False,'transcriptIdentitiesInvented':False,'humanAnnotationsModified':False,
        'note':'Body recognition is independent of text assignment; model scores are uncalibrated. Sajda bars use explicit detached-rule geometry within the Quran text band.'}
    db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('resolution',json.dumps(summary)));db.commit()
    db.execute('PRAGMA optimize');db.execute('PRAGMA wal_checkpoint(TRUNCATE)');db.close()
    (root/'resolution.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2))
    (root/'resolution-changes.json.zlib').write_bytes(zlib.compress(json.dumps(changes,separators=(',',':')).encode()))
    progress('resolve-complete',len(pages),len(pages),{'recovered':dict(recovered),'unknownBefore':before['items'],'unknownAfter':after['items']})
    return summary
