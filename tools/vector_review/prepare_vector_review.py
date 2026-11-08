#!/usr/bin/env python3
"""Turn legacy vector caches into weak, reviewable labels. No training labels are auto-approved."""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, ProcessPoolExecutor
import functools
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import subprocess
import unicodedata
from .dot_geometry import triple_layout, has_counter
from .stroke_geometry import component_path, connected_ink, stroke_pair, stroke_paths

ROOT = Path(__file__).resolve().parents[2]
MARKS = {0x64e:'fatha',0x650:'kasra',0x64f:'damma',0x64b:'tanwin-fath',0x64d:'tanwin-kasr',
  0x64c:'tanwin-damm',0x8f0:'tanwin-fath',0x8f2:'tanwin-kasr',0x8f1:'tanwin-damm',
  0x651:'shadda',0x652:'sukun',0x6e1:'sukun',0x653:'maddah',0x654:'hamza-above',0x655:'hamza-below',
  0x670:'small-alef',0x6e2:'small-meem',0x6ed:'small-low-meem',0x6e5:'small-waw',0x8f3:'small-waw',
  0x6e6:'small-yeh',0x6e7:'small-high-yeh',0x6e8:'small-noon',0x6dc:'small-seen',0x6e3:'small-low-seen',
  0x6d9:'waqf-lam',0x6d7:'waqf-qaf',0x6da:'waqf-jeem',0x6d6:'waqf-sad',0x6d8:'waqf-meem',
  0x6db:'waqf-three-dots',0x6df:'rounded-zero',0x6e0:'rectangular-zero',0x6ec:'rounded-filled-high',0x65c:'rounded-filled-low'}
CAPACITY = {'triple-dot':2,'tanwin-fath':2,'tanwin-kasr':2,'tanwin-damm':2,'waqf-qaf':2,'waqf-jeem':2,'waqf-three-dots':3,'small-noon':2}
DOT_ONE = set('بجخذزضظغفن')
DOT_TWO = set('تقة')

def has_letter_body(text):
    return any(unicodedata.category(c).startswith('L') and ord(c) not in MARKS and c!='ـ' for c in text)

def slots(subword):
    result=[]
    for i, record in enumerate(subword['characters']):
        char, position=record['text'],record['position']
        name=MARKS.get(ord(char))
        if not name:
            if char in DOT_ONE: name='single-dot'
            elif char in DOT_TWO or char=='ي' and position in ('initial','medial'): name='double-dot'
            elif char in 'ثش': name='triple-dot'
            elif char in 'أؤئ': name='hamza-above'
            elif char=='إ': name='hamza-below'
            elif char=='آ': name='maddah'
            elif char=='ٱ': name='wasla'
            elif char=='ك' and position in ('isolated','final'): name='small-kaf-body'
        if name:
            side='below' if name in ('kasra','tanwin-kasr','hamza-below','small-low-meem','small-low-seen') or name=='single-dot' and char in 'بج' or name=='double-dot' and char=='ي' else 'above' if name.endswith('-dot') or name in ('fatha','damma','tanwin-fath','tanwin-damm','shadda','sukun','maddah','wasla','hamza-above') else None
            result.append({'type':name,'characterOffset':i,'capacity':CAPACITY.get(name,1),'side':side})
    return result

def strong_type(path):
    scores=path['templateScores']
    choices=[(scores.get(name,1),label) for name,label in [('onedot','single-dot'),('twodots','double-dot'),('damma','damma')]]
    score,label=min(choices)
    if score<0.18:return label
    return 'double-dot' if path.get('dotShapeEvidence') else None

def triple_parts(paths):
    if len(paths)!=2:return None
    singles=[i for i,p in enumerate(paths) if strong_type(p)=='single-dot']
    if len(singles)!=1:return None
    single=singles[0];pair=1-single
    # Joined lower pairs in triple-dot clusters are slightly deformed relative
    # to the standalone two-dot template. Accept that variation only when the
    # complete cluster has the correct layout and scale.
    # A joined pair within a complete triple-dot layout can deform more than
    # a standalone double dot. Keep the single-dot anchor, native contour
    # count, layout, scale and assignment-distance gates; this allowance never
    # changes standalone double-dot recognition.
    if paths[pair]['templateScores'].get('twodots',1)>.35 and strong_type(paths[pair])!='double-dot':return None
    if not triple_layout(paths[single]['bbox'],paths[pair]['bbox']):return None
    return single,pair

def features(path):
    _,_,w,h=path['bbox']
    return int(path.get('signature','0'),16),w/max(h,.01),h

def distance(a,b):
    aa,ar,ah=a; bb,br,bh=b
    union=(aa|bb).bit_count()
    return (1-(aa&bb).bit_count()/max(1,union)) + .12*abs(math.log(ar/br)) + .12*abs(math.log(ah/bh))


def double_dot_evidence(path, bank, width):
    """Back up native contour matching with existing train-only shape exemplars."""
    _,_,w,h=path['bbox']
    if path['contourCount']!=1 or h<=0 or not 1.2<=w/h<=2.8 or not bank.get('double-dot'):return None
    feat=features(path);score=min(distance(feat,p) for p in bank['double-dot'])
    if score>.12:return None
    other=min((distance(feat,p) for name,prototypes in bank.items()
               if name!='double-dot' for p in prototypes),default=1.)
    if other-score<.2:return None
    pieces=connected_ink(path['runs'],width)
    if len(pieces)!=1 or has_counter(pieces[0]):return None
    return {'method':'train-page-signature','type':'double-dot','distance':score,'otherTypeDistance':other}


def signed_component(path, width, piece, suffix):
    part=component_path(path,width,piece,suffix);x,y,w,h=part['bbox'];ink=set(piece);signature=0
    for row in range(16):
        for col in range(16):
            pixel=y+(2*row+1)*h//32,x+(2*col+1)*w//32
            signature=(signature<<1)|(pixel in ink)
    part['signature']=format(signature,'064x')
    return part


def split_dot_pairs(path, width, bank):
    """A single SVG path may pack two unrelated joined pairs of letter dots."""
    if path['contourCount']!=2:return [path]
    pieces=connected_ink(path['runs'],width)
    if len(pieces)!=2:return [path]
    parts=[]
    for index,piece in enumerate(pieces,1):
        part=signed_component(path,width,piece,f'dot-pair-{index}')
        evidence=double_dot_evidence(part,bank,width)
        if not evidence:return [path]
        parts.append({**part,'dotShapeEvidence':evidence})
    return parts

def seed_bank(documents, sources=None):
    """Seed only single-residual-class cases from unflagged transcript-matching subwords.
    These remain weak templates, not human ground truth. Bank is learned on train pages only.
    """
    bank=defaultdict(list)
    for doc in documents:
        if split_for(doc['pageId'])!='train': continue
        paths={p['id']:p for p in doc['paths']}
        for word in doc['words']:
            if not word['transcriptMatches'] or word['legacyFlags']!=1: continue
            for sub in word['subwords']:
                remaining=Counter(s['type'] for s in slots(sub))
                residual=[]
                for pid in (sub['pathIds'][1:] if has_letter_body(sub['text']) else sub['pathIds']):
                    path=paths[pid]; label=strong_type(path)
                    if label and remaining[label]: remaining[label]-=1
                    else: residual.append(path)
                types=[name for name,count in remaining.items() if count]
                if len(types)==1 and remaining[types[0]]==len(residual) and CAPACITY.get(types[0],1)==1:
                    for path in residual:
                        feat=features(path)
                        if len(bank[types[0]])<24 and all(distance(feat,known)>.04 for known in bank[types[0]]):
                            bank[types[0]].append(feat)
                            if sources is not None: sources.append({'pageId':doc['pageId'],'sourceSha256':doc['sourceSha256'],'pathId':path['id'],'type':types[0]})
    return bank

def split_for(page_id):
    # Match the workbench's existing page split, so validation/test cannot seed templates.
    import hashlib
    bucket=int(hashlib.sha256(f'hafs1441-v1:{page_id}'.encode()).hexdigest()[:8],16)%100
    return 'test' if bucket<10 else 'validation' if bucket<20 else 'train'

def split_dammas(path, width, prototypes):
    """Recover two detached dammas packed into a single SVG path."""
    pieces = connected_ink(path['runs'], width)
    if len(pieces) != 2:
        return [path]
    pieces.sort(key=lambda piece: sum(x for _, x in piece) / len(piece), reverse=True)
    parts = []
    for index, piece in enumerate(pieces, 1):
        part = signed_component(path, width, piece, f'damma-{index}')
        x, y, w, h = part['bbox']
        score = min(distance(features(part), prototype) for prototype in prototypes)
        if score > .24:
            return [path]
        part['templateScores']['damma'] = score
        parts.append(part)
    return parts


def assignment(paths, expected, bank, baseline=None, stroke_grouping=False):
    """Small set-assignment DP. One semantic mark can own several nearby paths.
    Explicit missing and unused costs permit abstention rather than forced inventory fits.
    """
    if len(paths)>14 or len(expected)>14: return [],'assignment-too-large'
    n=len(paths)
    def score(indices, slot):
        kind=slot['type']; cap=slot['capacity']
        if stroke_grouping and kind in ('tanwin-fath','tanwin-kasr'):
            if not stroke_pair([paths[i] for i in indices]):
                return None
        cohesion=0.
        physical=sum(max(1,paths[i]['contourCount']) for i in indices)
        if physical!=cap: return None
        triple=triple_parts([paths[i] for i in indices]) if kind=='triple-dot' else None
        if kind=='triple-dot' and triple is None:return None
        if baseline is not None and slot.get('side'):
            for i in indices:
                box=paths[i]['bbox'];cy=box[1]+box[3]/2
                # Above-dot clusters cannot be assembled from below-letter dots.
                # Jeem's below-dot may sit inside a raised bowl, so below placement
                # is a soft prior rather than a universal hard boundary.
                if kind.endswith('-dot') and slot['side']=='above' and cy>baseline+2:return None
        if len(indices)>1:
            rects=[paths[i]['bbox'] for i in indices]
            left=min(b[0] for b in rects); right=max(b[0]+b[2] for b in rects)
            top=min(b[1] for b in rects); bottom=max(b[1]+b[3] for b in rects)
            if right-left>90 or bottom-top>90: return None
            # A count match alone can combine unrelated letters' dots. Require
            # a compact cluster at the scale of the actual ink pieces.
            for a,b in itertools.combinations(rects,2):
                pair_distance=math.hypot(a[0]+a[2]/2-b[0]-b[2]/2,a[1]+a[3]/2-b[1]-b[3]/2)
                multiplier=1.4 if kind=='triple-dot' else 2.4
                if pair_distance>max(14,multiplier*max(a[2],a[3],b[2],b[3])):return None
                cohesion+=pair_distance/max(1,a[2],a[3],b[2],b[3])
        value=0
        for at,i in enumerate(indices):
            path=paths[i]; strong=strong_type(path)
            if triple is not None:strong='single-dot' if at==triple[0] else 'double-dot'
            if kind.endswith('-dot'):
                if strong not in ('single-dot','double-dot'): return None
                if kind!='triple-dot' and strong!=kind: return None
                value+=.06
                if kind=='triple-dot' and strong=='double-dot':
                    value+=.2*min(path['templateScores'].get('twodots',1),path.get('dotShapeEvidence',{}).get('distance',1))
            elif strong in ('single-dot','double-dot'):
                value+=1.4  # Keep dot shapes available for dot slots.
            elif kind=='damma' or kind=='tanwin-damm':
                value+=min(1.3,path['templateScores'].get('damma',1))
            elif bank.get(kind) and len(indices)==1:
                value+=min(distance(features(path),prototype) for prototype in bank[kind])
            else: value+=.65
        value=value/max(1,len(indices))
        if baseline is not None and slot.get('side'):
            for i in indices:
                box=paths[i]['bbox'];cy=box[1]+box[3]/2
                wrong=max(0,cy-baseline) if slot['side']=='above' else max(0,baseline-cy)
                value+=min(1.5,wrong/18)
        return value+.12*cohesion
    options=[]
    for slot in expected:
        choices=[]
        for count in range(1,min(slot['capacity'],n)+1):
            for indices in itertools.combinations(range(n),count):
                cost=score(indices,slot)
                if cost is not None:
                    mask=sum(1<<i for i in indices)
                    choices.append((mask,cost,indices))
        options.append(choices)
    @functools.lru_cache(None)
    def solve(j,used):
        if j==len(expected): return .95*(n-used.bit_count()),()
        value,tail=solve(j+1,used)
        best=(value+1.1,((j,(),1.1),)+tail)
        for mask,cost,indices in options[j]:
            if not used&mask:
                value,tail=solve(j+1,used|mask)
                if value+cost<best[0]: best=(value+cost,((j,indices,cost),)+tail)
        return best
    _,selected=solve(0,0)
    missing=sum(not indices for _,indices,_ in selected)
    return [(expected[j],indices,cost) for j,indices,cost in selected if indices],f'{missing} expected marks unmatched' if missing else None

def prepare(doc,bank,stroke_grouping=False):
    paths={p['id']:{**p} for p in doc['paths']}
    candidates=[]; subwords=[]
    split_paths={}
    ownership=Counter(pid for word in doc['words'] for sub in word['subwords'] for pid in sub['pathIds'])
    for word in doc['words']:
        word_issues=[]
        if not word['transcriptMatches']: word_issues.append('Transcript/cache word mismatch')
        if word['legacyFlags']!=1:
            word_issues.extend(message for bit,message in [(2,'Contour count differs from transcript'),(4,'Legacy contour overflow correction'),(8,'Unresolved contour overflow'),(16,'Legacy subword boundary correction'),(32,'No body intersects the baseline')] if word['legacyFlags'] & bit)
        numeric=[s for s in word['subwords'] if s['text'] and all(c.isdigit() for c in s['text'])]
        if numeric:
            candidates.append({'id':word['id']+'-number','kind':'symbol','markType':'verse-number',
                'pathIds':[pid for sub in numeric for pid in sub['pathIds']],'subwordId':None,
                'wordId':word['id'],'line':word['line'],'issues':list(word_issues)})
        parent=None
        for sub in word['subwords']:
            if sub in numeric: continue
            ids=sub['pathIds']; expected=slots(sub); body=has_letter_body(sub['text'])
            if not body and not expected:
                candidates.append({'id':sub['id']+'-sign','kind':'symbol','markType':'hizb-sign' if '۞' in sub['text'] else 'quranic-sign',
                    'pathIds':ids,'subwordId':None,'wordId':word['id'],'line':word['line'],'issues':list(word_issues)})
                continue
            ink=[paths[pid] for pid in (ids[1:] if body else ids)]
            types=[slot['type'] for slot in expected]
            check_strokes=any(t in ('fatha','kasra','tanwin-fath','tanwin-kasr') for t in types)
            check_dammas=types.count('damma')==2 and 'tanwin-damm' not in types and bank.get('damma')
            if stroke_grouping and (check_strokes or check_dammas):
                expanded=[]
                for path in ink:
                    can_split=ownership[path['id']]==1 and strong_type(path) is None
                    width=doc['raster']['width']
                    parts=[path]
                    if check_dammas and can_split and path['contourCount']==2:
                        parts=split_dammas(path,width,bank['damma'])
                    if len(parts)==1 and check_strokes:
                        parts=stroke_paths(path,width,split=can_split)
                    if len(parts)>1:
                        split_paths[path['id']]=parts
                    expanded.extend(parts)
                ink=expanded
            if stroke_grouping:
                width=doc['raster']['width'];expanded=[];budget=max(0,14-len(ink))
                for path in ink:
                    parts=split_dot_pairs(path,width,bank) if budget and ownership[path['id']]==1 else [path]
                    if len(parts)>1:
                        budget-=len(parts)-1;split_paths[path['id']]=parts
                    for part in parts:
                        if strong_type(part) is None:
                            evidence=double_dot_evidence(part,bank,width)
                            if evidence:part={**part,'dotShapeEvidence':evidence}
                        if part['id'] in paths:paths[part['id']]=part
                        expanded.append(part)
                ink=expanded
            assigned,reason=assignment(ink,expected,bank,word.get('baseline'),stroke_grouping)
            used=set(); cids=[]; body_ids=[]
            sid=sub['id'] if body else parent['id'] if parent else None
            def add(pids,kind,mark_type='unknown',issues=(),**extra):
                cid=f"{sub['id']}-c{len(cids)+1}"
                candidates.append({'id':cid,'kind':kind,'markType':mark_type,'pathIds':pids,'subwordId':sid,
                    'wordId':word['id'],'line':word['line'],'issues':word_issues+list(issues),**extra})
                cids.append(cid)
                if kind=='subword-ink': body_ids.append(cid)
            if body and ids: add([ids[0]],'subword-ink',issues=['Subword body includes uncut letters and connections'])
            for slot,indices,cost in assigned:
                used.update(indices)
                kind='dot' if slot['type'].endswith('-dot') else 'symbol' if slot['type'].startswith('waqf-') else 'mark'
                if slot['type']=='small-kaf-body': kind='subword-ink'
                issues=[]
                if cost>.45: issues.append('Weak shape evidence; inventory-based proposal')
                if reason: issues.append(reason)
                # Offset is a hint in the original cached subword, never an approved letter owner.
                add([ink[i]['id'] for i in indices],kind,slot['type'],issues,shapeCost=round(cost,4),characterOffsetHint=slot['characterOffset'])
            for i,path in enumerate(ink):
                if i not in used:
                    known=strong_type(path)
                    add([path['id']],'dot' if known and known.endswith('-dot') else 'unknown',known or 'unknown',
                        ['Ink not assigned to transcript inventory'])
            if body:
                parent={**sub,'wordId':word['id'],'wordText':word['text'],'line':word['line'],
                    'candidateIds':cids,'bodyCandidateIds':body_ids,'issues':word_issues+([reason] if reason else [])}
                subwords.append(parent)
            elif parent:
                # Quranic small waw/yeh are detached marks, not new letter bodies.
                parent['text']+=sub['text']
                parent['candidateIds'].extend(cids)
                parent['characters']=parent.get('characters',[])+sub.get('characters',[])
                parent['issues'].extend([reason] if reason else [])
    counts=Counter(pid for c in candidates for pid in c['pathIds'])
    for path in doc['paths']:
        if not counts[path['id']] and path['id'] not in split_paths:
            candidates.append({'id':path['id']+'-unassigned','kind':'unknown','markType':'unknown',
                'pathIds':[path['id']],'subwordId':None,'wordId':path['wordId'],'line':path['line'],
                'issues':['Legacy path has no unique subword assignment']})
    for c in candidates:
        if any(counts[pid]>1 for pid in c['pathIds']): c['issues'].append('Overlapping legacy subword membership')
    return {**doc,'paths':list(paths.values())+[part for parts in split_paths.values() for part in parts],
        'subwords':subwords,'candidates':candidates,
        'method':'Legacy transcript contours + template distances + semantic mark set-assignment DP',
        'templatePolicy':'Weak exemplars from unflagged train pages only; no validation/test template fitting',
        'reviewRequired':True,'summary':dict(Counter(c['kind'] for c in candidates))}

def prepare_file(task):
    file,bank,bank_sha,destination=task
    doc=json.loads(file.read_text())
    result=prepare(doc,bank)
    result['provenance']['weakTemplateBankSha256']=bank_sha
    result['provenance']['transcriptSha256']=hashlib.sha256('\n'.join(w['text'] for w in doc['words']).encode()).hexdigest()
    encoded=json.dumps(result,ensure_ascii=False,separators=(',',':'))
    file=destination/f"{doc['pageId']}.json"
    # Proposals are replaceable; approved annotations are never touched.
    temporary=file.with_suffix('.tmp'); temporary.write_text(encoded); temporary.replace(file)
    return {'pageId':doc['pageId'],'subwords':len(result['subwords']),**result['summary']}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--svg-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'output/vector-review')
    parser.add_argument('--workbench-data',type=Path,required=True)
    parser.add_argument('--pages',default='all')
    parser.add_argument('--jobs',type=int,default=3)
    parser.add_argument('--skip-export',action='store_true')
    args=parser.parse_args()
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    pages=set()
    for part in args.pages.split(','):
        a,b=(1,604) if part=='all' else (int(part.split('-')[0]),int(part.split('-')[-1]))
        if not 1<=a<=b<=604: parser.error('Pages must be 1–604')
        pages.update(range(a,b+1))
    if not 1<=args.jobs<=16: parser.error('jobs must be 1–16')
    if not args.skip_export:
        batches=[sorted(pages)[i::args.jobs] for i in range(args.jobs)]
        def run(batch):
            if not batch: return
            # Each worker has its own manifests; per-page outputs are disjoint.
            dest=output/f'worker-{batch[0]}'
            subprocess.run([str(ROOT/'build/ninja-nmc/Debug/medinamushafanalyzer'),
                '--export-proposals',str(dest),'--svg-dir',str(args.svg_dir.resolve()),
                '--pages',','.join(map(str,batch))],cwd=ROOT,env={**os.environ,'QT_QPA_PLATFORM':'offscreen'},check=True)
            for file in dest.glob('page*.raw.json'): file.replace(output/file.name)
        with ThreadPoolExecutor(max_workers=args.jobs) as pool: list(pool.map(run,batches))
    files=[]
    for number in sorted(pages):
        file=output/f'page{number:03d}.raw.json'
        if not file.exists(): raise SystemExit(f'Missing {file}')
        files.append(file)
    seed_sources=[]
    bank=seed_bank((json.loads(file.read_text()) for file in files),seed_sources)
    bank_record={'policy':'Weak, unreviewed exemplars from train pages only','prototypes':dict(bank),'sources':seed_sources}
    bank_bytes=json.dumps(bank_record,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode()
    bank_sha=hashlib.sha256(bank_bytes).hexdigest()
    (output/'weak-template-bank.json').write_bytes(bank_bytes)
    (output/'weak-template-summary.json').write_text(json.dumps({name:len(values) for name,values in bank.items()},indent=2))
    destination=args.workbench_data.resolve()/'proposals'
    destination.mkdir(parents=True,exist_ok=True)
    manifest=[]
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(prepare_file,((file,dict(bank),bank_sha,destination) for file in files)):
            manifest.append(result)
            print(f"Prepared {result['pageId']}: {result}",flush=True)
    (output/'prepared-manifest.json').write_text(json.dumps(manifest,indent=2))

if __name__=='__main__': main()
