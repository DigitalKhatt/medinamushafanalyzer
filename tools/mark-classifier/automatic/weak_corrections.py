"""Independent, conservative source-geometry checks for automatic labels.

No model outputs, human flags, source-mask edits, or train/test fitting are used.
Original proposals remain available as evidence rather than being overwritten.
"""
from collections import defaultdict
import hashlib
import json

from .common import unpack_runs

VERSION='source-mark-geometry-v2'


def holes(pixels):
    ink=set(pixels);ys,xs=zip(*ink)
    y0,y1=min(ys)-1,max(ys)+1;x0,x1=min(xs)-1,max(xs)+1
    empty={(y,x) for y in range(y0,y1+1) for x in range(x0,x1+1)}-ink
    count=0
    while empty:
        todo=[empty.pop()];outside=False
        while todo:
            y,x=todo.pop();outside|=y in (y0,y1) or x in (x0,x1)
            for point in ((y-1,x),(y+1,x),(y,x-1),(y,x+1)):
                if point in empty:empty.remove(point);todo.append(point)
        if not outside:count+=1
    return count


def pieces(runs,width):
    ink={divmod(p,width) for s,n in zip(runs[::2],runs[1::2]) for p in range(s,s+n)}
    result=[]
    while ink:
        todo=[ink.pop()];pixels=[]
        while todo:
            y,x=todo.pop();pixels.append((y,x))
            for dy in (-1,0,1):
                for dx in (-1,0,1):
                    point=(y+dy,x+dx)
                    if point in ink:ink.remove(point);todo.append(point)
        ys,xs=zip(*pixels)
        result.append({'x':min(xs),'y':min(ys),'w':max(xs)-min(xs)+1,
            'h':max(ys)-min(ys)+1,'area':len(pixels),'holes':holes(pixels)})
        if len(result)>2:return []
    return sorted(result,key=lambda p:p['area'])


def shape_type(parts):
    if len(parts)!=2:return None
    small,big=sorted(parts,key=lambda p:p['area'])
    inside=(big['x']<=small['x'] and big['y']<=small['y'] and
        small['x']+small['w']<=big['x']+big['w'] and small['y']+small['h']<=big['y']+big['h'])
    if (small['area']/big['area']<.18 and max(small['w'],small['h'])<=7
            and inside and .5<big['w']/big['h']<1.3):return 'waqf-jeem'
    if (small['area']/big['area']>.4 and all(p['w']/p['h']>1.4 for p in parts)):
        return 'tanwin-fath'
    return None


def double_damma(parts):
    """Two looped marks at the native 4x raster scale, without letter ownership."""
    if len(parts)!=2:return False
    a,b=parts
    return (all(p['holes']==1 and 18<=p['h']<=32 and 12<=p['w']<=26 and 80<=p['area']<=210 for p in parts)
        and min(a['area'],b['area'])/max(a['area'],b['area'])>.55
        and abs(a['y']+a['h']/2-b['y']-b['h']/2)<.25*max(a['h'],b['h'])
        and .35*min(a['w'],b['w'])<abs(a['x']+a['w']/2-b['x']-b['w']/2)<1.5*max(a['w'],b['w']))


def dammatan_family(parts):
    if double_damma(parts):return 'two-loops'
    if len(parts)!=2 or sorted(p['holes'] for p in parts)!=[0,1]:return None
    arc=next(p for p in parts if p['holes']==0);loop=next(p for p in parts if p['holes']==1)
    if (all(18<=p['h']<=32 and 12<=p['w']<=28 and 80<=p['area']<=210 for p in parts)
            and 3<=loop['y']-arc['y']<=14 and abs(arc['x']+arc['w']/2-loop['x']-loop['w']/2)<=5):
        return 'arc-and-loop'
    return None


def rule_labels(db):
    """Reuse the detached-rule teacher in preparation, never final inference."""
    from .resolve import classify_horizontal_rule
    if not db.execute("SELECT 1 FROM sqlite_master WHERE name='words'").fetchone():return {}
    result={}
    for page in db.execute('SELECT * FROM pages'):
        words=list(db.execute('SELECT x,w FROM words WHERE page_id=? AND in_scope=1',(page['page_id'],)))
        band=(min(w['x'] for w in words),max(w['x']+w['w'] for w in words)) if words else None
        for row in db.execute("SELECT * FROM items WHERE page_id=? AND in_scope=1 AND teacher_kind='unknown' AND human_reviewed=0 AND origin IN ('unresolved','machine-weak') AND w>=80 AND h<=6",(page['page_id'],)):
            if classify_horizontal_rule(row,page,band)!='sajda-bar':continue
            result[row['id']]={'itemId':row['id'],'pageId':page['page_id'],'split':page['split'],
                'originalRole':row['teacher_kind'],'originalType':row['teacher_type'],
                'role':'symbol','type':'sajda-bar','action':'supply','quality':.75,
                'originalQuality':row['quality'],'sourceSha256':page['source_sha'],
                'maskSha256':hashlib.sha256(json.dumps(unpack_runs(row['runs']),separators=(',',':')).encode()).hexdigest(),
                'provenance':'automatic-geometry','version':VERSION,'evidence':'Detached dense rectangular rule inside the Quran text band.'}
    return result


def alif_hamza_labels(db):
    """Recover a detached hamza below an alif in an explicitly written إِ."""
    columns = {row[1] for row in db.execute('PRAGMA table_info(items)')}
    if not {'text', 'x', 'y', 'w', 'h', 'components'} <= columns:
        return {}
    sql = """
        SELECT h.*, a.id AS alif_id, p.split, p.source_sha
        FROM items h JOIN items a ON a.subword_id=h.subword_id AND a.page_id=h.page_id
        JOIN pages p ON p.page_id=h.page_id
        WHERE h.text='إِ' AND h.teacher_kind='subword-ink' AND a.teacher_kind='unknown'
          AND h.in_scope=1 AND a.in_scope=1
          AND h.human_reviewed=0 AND a.human_reviewed=0
          AND h.origin='machine-weak' AND a.origin='machine-weak'
          AND h.components=1 AND a.components=1
          AND h.w BETWEEN 10 AND 22 AND h.h BETWEEN 10 AND 22
          AND a.w<=18 AND a.h BETWEEN 40 AND 90
          AND h.y-a.y-a.h BETWEEN -2 AND 20
          AND abs((a.x+a.w/2.0)-(h.x+h.w/2.0))<=12
          AND (SELECT COUNT(*) FROM items s WHERE s.subword_id=h.subword_id)=3
          AND EXISTS (SELECT 1 FROM items k WHERE k.subword_id=h.subword_id
                      AND k.teacher_type='kasra' AND k.y>=h.y+h-2
                      AND k.y-h.y-h<=12 AND abs(k.x+k.w/2.0-h.x-h.w/2.0)<=12)
    """
    result = {}
    for row in db.execute(sql):
        for item_id, role, kind in (
            (row['id'], 'mark', 'hamza-below'),
            (row['alif_id'], 'subword-ink', 'unknown'),
        ):
            result[item_id] = {
                'itemId': item_id, 'pageId': row['page_id'], 'split': row['split'],
                'role': role, 'type': kind, 'quality': .70, 'action': 'relabel',
                'originalType': 'unknown', 'provenance': 'automatic-geometry',
                'version': 'detached-hamza-below-v1',
                'sourceSha256': row['source_sha'],
                'evidence': 'Separate aligned alif, detached hamza below, and kasra in إِ.',
            }
    return result


def damma_high_yeh_labels(db, root):
    """Clean contaminated train targets using native templates and ink counters.

    A detached closed-loop damma is sometimes assigned to the transcript's
    small-high-yeh slot across a subword boundary. This is source evidence only,
    never a prediction-based target or a human approval.
    """
    from pathlib import Path
    from review_bridge import proposal_digest
    root=Path(root);result={};documents={}
    for row in db.execute("""SELECT i.*,p.width,p.source_sha,p.split,p.proposal_sha FROM items i JOIN pages p USING(page_id)
        WHERE p.split='train' AND i.in_scope=1 AND i.teacher_type='small-high-yeh'
          AND i.origin='machine-weak' AND i.human_reviewed=0"""):
        page=row['page_id']
        if page not in documents:
            path=root/'proposals'/f'{page}.json'
            documents[page]=json.loads(path.read_text()) if path.exists() else {}
        doc=documents[page]
        if doc.get('sourceSha256')!=row['source_sha']:continue
        candidate=next((c for c in doc.get('candidates',[]) if page+':'+c['id']==row['id']),None)
        if not candidate or len(candidate['pathIds'])!=1:continue
        native=next((p for p in doc['paths'] if p['id']==candidate['pathIds'][0]),None)
        if not native or native['contourCount']!=1:continue
        score=native['templateScores'].get('damma',1.)
        if score>=.18:continue  # Same strong-template threshold as preparation.
        if proposal_digest(doc)!=row['proposal_sha']:continue
        runs=unpack_runs(row['runs']);components=pieces(runs,row['width'])
        if len(components)!=1:continue
        ink=components[0]
        if not (ink['holes']==1 and 12<=ink['w']<=28 and 18<=ink['h']<=32 and 80<=ink['area']<=210):continue
        result[row['id']]={'itemId':row['id'],'pageId':page,'split':'train',
            'originalRole':row['teacher_kind'],'originalType':row['teacher_type'],
            'role':'mark','type':'damma','action':'relabel','quality':.9,
            'provenance':'automatic-geometry','version':'closed-loop-damma-v1',
            'sourceSha256':row['source_sha'],
            'maskSha256':hashlib.sha256(json.dumps(runs,separators=(',',':')).encode()).hexdigest(),
            'evidence':{'nativePathId':native['id'],'nativeDammaDistance':score,'rasterCounters':1}}
    return result


def corrections(db, root=None):
    """Repair only reciprocal swaps in the same prepared subword.

    Requiring one clear shape of each kind preserves the original inventory;
    a solitary disagreement or an ambiguous component receives no correction.
    """
    groups=defaultdict(list);supported={}
    sql="""SELECT i.*,p.width,p.source_sha,p.split FROM items i JOIN pages p USING(page_id)
        WHERE i.in_scope=1 AND i.origin='machine-weak' AND i.human_reviewed=0
        AND i.teacher_type IN ('waqf-jeem','tanwin-fath','tanwin-damm') AND i.subword_id IS NOT NULL"""
    for row in db.execute(sql):
        runs=unpack_runs(row['runs']);parts=pieces(runs,row['width']);label=shape_type(parts)
        family=dammatan_family(parts) if row['teacher_type']=='tanwin-damm' else None
        support=family is not None
        if (label and label!=row['teacher_type'] and row['teacher_type']!='tanwin-damm') or support:
            if support:label='tanwin-damm'
            record={
                'itemId':row['id'],'pageId':row['page_id'],'split':row['split'],
                'originalRole':row['teacher_kind'],'originalType':row['teacher_type'],
                'role':'symbol' if label=='waqf-jeem' else 'mark','type':label,'action':'support' if support else 'relabel',
                'sourceSha256':row['source_sha'],
                'maskSha256':hashlib.sha256(json.dumps(runs,separators=(',',':')).encode()).hexdigest(),
                'pieces':parts,'provenance':'automatic-geometry','version':VERSION}
            if support:
                record.update(quality=max(row['quality'],.65),originalQuality=row['quality'],family=family)
                supported[row['id']]=record
            else:groups[(row['page_id'],row['subword_id'])].append(record)
    result={}
    for group in groups.values():
        if len(group)==2 and {r['type'] for r in group}=={'waqf-jeem','tanwin-fath'}:
            for row in group:
                row['pairedItemId']=next(r['itemId'] for r in group if r is not row)
                result[row['itemId']]=row
    result.update(supported)
    result.update(rule_labels(db))
    result.update(alif_hamza_labels(db))
    if root is not None:result.update(damma_high_yeh_labels(db,root))
    mixed = db.execute("""
        SELECT i.id, i.runs, p.split FROM items i JOIN pages p USING(page_id)
        WHERE i.id='page197:l9-w6-s2-c6' AND i.in_scope=1
          AND i.origin='machine-weak' AND i.human_reviewed=0
          AND i.teacher_type='tanwin-kasr'
    """).fetchone()
    # Bind this exclusion to the inspected mask, since proposal IDs can be reused.
    mask_hash = '05c60168e51c5d1e3d0b7208fd7ed65b485c867ef8351c372a383ac4027c24d3'
    if mixed and hashlib.sha256(mixed['runs']).hexdigest() == mask_hash:
        result[mixed['id']] = {
            'itemId': mixed['id'], 'split': mixed['split'],
            'action': 'exclude', 'role': None, 'type': None,
            'originalType': 'tanwin-kasr',
            'evidence': 'This region combines a detached hamza below with a kasra.',
        }
    return result
