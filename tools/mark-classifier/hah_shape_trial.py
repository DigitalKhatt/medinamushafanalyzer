"""Reproducible, isolated medial-hah shape-ranking experiment (no live labels modified)."""
from workbench_paths import NATIVE
import argparse
import base64
from collections import Counter, defaultdict
import hashlib
import io
import itertools
import json
import math
import os
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi
from scipy.spatial import ConvexHull

ROOT = Path(__file__).resolve().parent
DEFAULT_BASELINE = ROOT/'data/automatic/experiments/letter-retest-20261104/tatweel-review'
FEATURE_NAMES = ['logAspect', 'density', 'logHeightPerStroke', 'logWidthPerStroke',
                 'strokeSpread', 'counterArea', 'largestConcavity', 'concavityX', 'concavityY',
                 'inkTopRight', 'inkTopLeft', 'inkBottomRight', 'inkBottomLeft']
CONFIG = {'version': 1, 'outlineWeight': .65, 'structureWeight': .25, 'portsWeight': .10,
          'counterPenalty': .08, 'componentPenalty': .12, 'ambiguityGap': .025,
          'sameMaskIoU': .98, 'calibrationQuantile': .95,
          'description': 'Whole-body contour, intrinsic concavity/topology, stroke-relative proportions, and normalized ports. No neighbor landmark features.'}

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)+'\n')

def decode(encoded):
    return np.asarray(Image.open(io.BytesIO(base64.b64decode(encoded))).convert('L')) > 127

def iou(a, b):
    union = np.count_nonzero(a|b)
    return float(np.count_nonzero(a&b)/union) if union else 1.

def boundary(mask):
    return mask & ~ndi.binary_erosion(mask)

def topology(mask):
    labels, count = ndi.label(mask)
    sizes = np.bincount(labels.ravel())[1:]
    components = int(np.count_nonzero(sizes >= max(2, mask.sum()*.003)))
    holes = ndi.binary_fill_holes(mask) & ~mask
    labels, count = ndi.label(holes)
    sizes = np.bincount(labels.ravel())[1:]
    return components, int(np.count_nonzero(sizes >= max(2, mask.sum()*.005))), holes

def ports(body, right=None, left=None):
    box = body['bounds']; origin = np.array(box[:2]); scale = max(box[2:])
    values = []
    for cut in (right or body.get('right'), left or body.get('left')):
        if cut is None: return None
        if isinstance(cut, dict): points = [cut['top']['xy'], cut['bottom']['xy']]
        else: points = cut
        values.extend(((np.asarray(points)-origin)/scale).ravel().tolist())
    return values

def baseline_ports(word, index):
    joins = {j['afterBody']: j for j in word.get('joins', [])}
    incoming, outgoing = joins.get(index), joins.get(index+1)
    if not incoming or not outgoing: return None
    right = incoming.get('endpoints', [incoming['anchor']])[-1]
    left = outgoing.get('endpoints', [outgoing['anchor']])[0]
    return ports(word['bodies'][index], right, left)

def descriptor(body, port_values=None):
    mask = decode(body['normalizedMask'])
    yy, xx = np.nonzero(mask)
    if len(xx) < 4: raise ValueError('empty-body')
    x0,x1,y0,y1 = xx.min(),xx.max(),yy.min(),yy.max()
    width,height = x1-x0+1, y1-y0+1
    area = len(xx)
    dist = ndi.distance_transform_edt(mask)
    ridge = mask & (dist >= ndi.maximum_filter(dist, size=3)-1e-8)
    radii = dist[ridge]
    stroke = max(1.,2*float(np.median(radii)))
    components, holes, hole_mask = topology(mask)
    hull = np.zeros_like(mask)
    pts = np.column_stack([xx,yy])
    try:
        polygon = pts[ConvexHull(pts).vertices]
        im = Image.new('1', (128,128));ImageDraw.Draw(im).polygon([tuple(x) for x in polygon], fill=1)
        hull = np.asarray(im, dtype=bool)
    except Exception:
        hull = mask.copy()
    concavity = hull & ~mask & ~hole_mask
    labels,n = ndi.label(concavity)
    if n:
        sizes = np.bincount(labels.ravel());sizes[0]=0
        cy,cx = np.nonzero(labels==sizes.argmax())
        largest = len(cx)/max(1,hull.sum());center_x=(cx.mean()-x0)/width;center_y=(cy.mean()-y0)/height
    else: largest,center_x,center_y = 0.,.5,.5
    mx,my=(x0+x1)/2,(y0+y1)/2
    quadrants = [float(np.count_nonzero((xx>mx)&(yy<=my))/area),float(np.count_nonzero((xx<=mx)&(yy<=my))/area),
                 float(np.count_nonzero((xx>mx)&(yy>my))/area),float(np.count_nonzero((xx<=mx)&(yy>my))/area)]
    features = [math.log(width/height),area/(width*height),math.log(height/stroke),math.log(width/stroke),
                float(np.percentile(radii,90)/max(.5,np.percentile(radii,10))),hole_mask.sum()/area,
                float(largest),float(center_x),float(center_y),*quadrants]
    small = np.asarray(Image.fromarray(mask.astype('uint8')*255).resize((48,48),Image.Resampling.BOX))>127
    edge = boundary(small)
    return {'features':np.asarray(features,dtype=float), 'edge':edge, 'dt':ndi.distance_transform_edt(~edge),
            'holes':holes,'components':components,'ports':port_values if port_values is not None else ports(body),
            'strokeInNativeUnits':stroke*max(body['bounds'][2:])/112}

def serial_descriptor(d):
    return {'features':d['features'].tolist(), 'edge':np.flatnonzero(d['edge']).tolist(),
            'holes':d['holes'],'components':d['components'],'ports':d['ports'],
            'strokeInNativeUnits':d['strokeInNativeUnits']}

def restore_descriptor(d):
    edge = np.zeros((48,48),bool);edge.flat[d['edge']]=True
    return {**d,'features':np.array(d['features']),'edge':edge,'dt':ndi.distance_transform_edt(~edge)}

def distance(a,b,scales,config=CONFIG):
    outline = float((a['dt'][b['edge']].mean()+b['dt'][a['edge']].mean())/32)
    structure = float(np.clip(np.abs(a['features']-b['features'])/scales,0,4).mean()/4)
    port_distance = (float(np.abs(np.array(a['ports'])-b['ports']).mean())
                     if a['ports'] is not None and b['ports'] is not None else 0.)
    topology_cost = config['counterPenalty']*abs(a['holes']-b['holes'])+config['componentPenalty']*abs(a['components']-b['components'])
    score = config['outlineWeight']*outline+config['structureWeight']*structure+config['portsWeight']*port_distance+topology_cost
    return score, {'outline':outline,'structure':structure,'ports':port_distance,'topology':topology_cost}

def load_pages(folder, split=None):
    manifest=json.loads((folder/'inputs.json').read_text())
    for path in sorted((folder/'candidates').glob('page*.json')):
        assigned=manifest['pageSplits'][path.stem]
        if split and assigned!=split:continue
        doc=json.loads(path.read_text());doc['split']=assigned
        yield doc

def fit(folder):
    if (folder/'model.json').exists():raise ValueError('Frozen model already exists; use a new experiment folder to refit')
    prototypes=[]
    for page in load_pages(folder,'train'):
        for word in page['words']:
            if not word['complete'] or word['provisional']:continue
            for target in word['targets']:
                if target['status']!='matched':continue
                index=target['letterIndex']-1
                if index>=len(word['bodies']):continue
                body=word['bodies'][index]
                desc=descriptor(body,baseline_ports(word,index))
                prototypes.append({'id':word['id']+f':letter{index+1}', 'page':page['page'],
                    'template':target['template'],'provenance':'machine-weak','humanReviewed':False,
                    'body':body,'descriptor':serial_descriptor(desc)})
    if len(prototypes)<10:raise ValueError('Insufficient train-only examples')
    restored=[restore_descriptor(p['descriptor']) for p in prototypes]
    values=np.array([d['features'] for d in restored])
    scales=np.maximum(np.median(np.abs(values-np.median(values,axis=0)),axis=0)*1.4826,.07)
    # Page-disjoint calibration among training pages only. This is weak-template agreement,
    # not a held-out accuracy measurement and not calibrated probability.
    lo=[]
    for p,d in zip(prototypes,restored):
        pool=[distance(d,e,scales)[0] for q,e in zip(prototypes,restored) if q['page']!=p['page']]
        lo.append(min(pool))
    model={'schemaVersion':1,'config':CONFIG,'featureNames':FEATURE_NAMES,'scales':scales.tolist(),
           'distanceLimit':float(np.quantile(lo,CONFIG['calibrationQuantile'])),
           'calibration':{'method':'leave-one-train-page-out nearest positive prototype','count':len(lo),
                          'medianDistance':float(np.median(lo)),'p95Distance':float(np.quantile(lo,.95))},
           'trainingPages':sorted({p['page'] for p in prototypes}), 'prototypes':prototypes,
           'inputsHash':digest(folder/'inputs.json'),'scorerHash':digest(__file__),
           'provenance':'machine-experimental','humanReviewed':False}
    write_json(folder/'model.json',model)
    print(json.dumps({k:v for k,v in model.items() if k not in ['prototypes','featureNames','scales']},indent=2))

def partition_check(candidate, source):
    masks=np.stack([decode(x) for x in candidate['partitionMasks']]); counts=masks.sum(0)
    missing=int(np.count_nonzero(source & (counts==0)))
    extra=int(np.count_nonzero(~source & (counts>0)))
    overlap=int(np.count_nonzero(source & (counts>1)))
    tolerance=max(8,round(source.sum()*.01))
    return {'missing':missing,'extra':extra,'overlap':overlap,'tolerance':tolerance,
            'valid':missing+overlap<=tolerance and extra<=tolerance}

def assignments(candidates,count):
    """Enumerate ordered, nonoverlapping hah bodies. No neighboring letter anchors."""
    by_id={c['id']:c for c in candidates if c['conservation']['valid']}
    states=[(c['distance'],[c['id']]) for c in by_id.values()]
    for _ in range(1,count):
        extended=[]
        for total,path in states:
            for other in by_id[path[-1]]['following']:
                if other not in by_id or other in path:continue
                if any(iou(decode(by_id[other]['mask']),decode(by_id[previous]['mask']))>.02 for previous in path):continue
                extended.append((total+by_id[other]['distance'],path+[other]))
        states=sorted(extended)[:256]
    return sorted(states,key=lambda x:(x[0],x[1]))

def human_mask(annotation, part, bounds):
    x,y,w,h=bounds;out=np.zeros((h,w),bool)
    runs=part['runs']; width=annotation['raster']['width'];outside=0
    for start,length in zip(runs[::2],runs[1::2]):
        yy,xx=np.divmod(np.arange(start,start+length),width);valid=(xx>=x)&(xx<x+w)&(yy>=y)&(yy<y+h)
        outside+=int((~valid).sum());out[yy[valid]-y,xx[valid]-x]=True
    return out,outside

def boundary_f1(a,b,tolerance=2):
    aa,bb=boundary(a),boundary(b)
    if not aa.any() or not bb.any():return 0.
    precision=float((ndi.distance_transform_edt(~bb)[aa]<=tolerance).mean())
    recall=float((ndi.distance_transform_edt(~aa)[bb]<=tolerance).mean())
    return 2*precision*recall/(precision+recall) if precision+recall else 0.

def evaluate(folder):
    model=json.loads((folder/'model.json').read_text())
    if model['scorerHash']!=digest(__file__):raise ValueError('Scorer changed after freezing; use a new run')
    scales=np.array(model['scales']);config=model['config']
    protos=[(p,restore_descriptor(p['descriptor'])) for p in model['prototypes']]
    report={'schemaVersion':1,'modelHash':digest(folder/'model.json'),'provenance':'machine-experimental',
            'humanReviewed':False,'productionChanged':False,'splits':{},'words':[]}
    stats=defaultdict(Counter);ious=defaultdict(list);human=[]
    for page in load_pages(folder):
        split=page['split']
        for word in page['words']:
            source=decode(word['sourceMask'])
            for c in word['candidates']:
                desc=descriptor(c);matches=[]
                for p,d in protos:
                    # Even train inspection excludes its entire own page from references.
                    if p['page']==page['page']:continue
                    dis,terms=distance(desc,d,scales,config)
                    matches.append((dis,p['id'],p['template'],terms))
                best=min(matches)
                c.update(distance=best[0],prototype=best[1],prototypeForm=best[2],terms=best[3],
                         features=dict(zip(FEATURE_NAMES,desc['features'].tolist())),
                         holes=desc['holes'],components=desc['components'],stroke=desc['strokeInNativeUnits'],
                         conservation=partition_check(c,source))
            sequences=assignments(word['candidates'],len(word['targets']))
            by_id={c['id']:c for c in word['candidates']}
            selected=sequences[0][1] if sequences else []
            alternatives=[]
            for cost,seq in sequences[1:]:
                if all(iou(decode(by_id[a]['mask']),decode(by_id[b]['mask']))>=config['sameMaskIoU'] for a,b in zip(selected,seq)):continue
                alternatives.append((cost,seq))
            gap=(alternatives[0][0]-sequences[0][0])/len(selected) if alternatives and selected else None
            result={**word,'page':page['page'],'split':split,'selected':selected,'gap':gap,
                    'assignmentAlternatives':[{'distance':cost,'ids':seq} for cost,seq in alternatives[:8]],'comparisons':[]}
            for i,target in enumerate(word['targets']):
                stats[split]['targets']+=1
                index=target['letterIndex']-1
                choice=by_id[selected[i]] if selected else None
                if choice is None:status='no-candidate'
                elif choice['distance']>model['distanceLimit']:status='out-of-library'
                elif gap is not None and gap<config['ambiguityGap']:status='ambiguous'
                else:status='distinct-proposal'
                stats[split][status]+=1
                comparison={'letterIndex':index+1,'character':target['character'],'baselineStatus':target['status'],
                            'baselineForm':target.get('template'),'selected':choice['id'] if choice else None,'status':status}
                # Partial words may not map bodies to letters reliably. Never call those masks ground truth.
                if word['complete'] and len(word['bodies'])==len(word['letters']):
                    baseline=decode(word['bodies'][index]['mask'])
                    comparison['baselineMaskIndex']=index
                    comparison['nativeAgreementIoU']=iou(baseline,decode(choice['mask'])) if choice else 0.
                    comparison['candidateOracleAgreementIoU']=max((iou(baseline,decode(c['mask'])) for c in word['candidates'] if c['conservation']['valid']),default=0.)
                    ious[split].append(comparison['nativeAgreementIoU'])
                    stats[split]['nativeComparable']+=1
                    stats[split]['nativeAgreementAt90']+=comparison['nativeAgreementIoU']>=.90
                if word['id']=='page590:l7-w7-s2' and index==1:
                    annotation=json.loads((ROOT/'data/annotations/page590.json').read_text())
                    if annotation['sourceSha256']!=page['sourceSha256']:raise ValueError('Human/source hash mismatch')
                    part=next(p for p in annotation['parts'] if p.get('owner')=='char-0002' and p.get('role')=='body' and p.get('reviewed'))
                    manual,outside=human_mask(annotation,part,word['rasterBounds'])
                    if outside:raise ValueError('Human mask is outside crop')
                    baseline=decode(word['bodies'][index]['mask'])
                    trial=decode(choice['mask']) if choice else np.zeros_like(manual)
                    valid=[c for c in word['candidates'] if c['conservation']['valid']]
                    oracle=max(valid,key=lambda c:iou(manual,decode(c['mask']))) if valid else None
                    metrics={'id':word['id'],'letterIndex':2,'reviewedPixels':int(manual.sum()),
                             'baselineIoU':iou(manual,baseline),'trialIoU':iou(manual,trial),
                             'baselineBoundaryF1':boundary_f1(manual,baseline),'trialBoundaryF1':boundary_f1(manual,trial),
                             'boundaryTolerancePixels':2,'oracleCandidate':oracle['id'] if oracle else None,
                             'oracleIoU':iou(manual,decode(oracle['mask'])) if oracle else 0.,
                             'warning':'One reviewed mask; candidate oracle uses review truth and is not achievable selection accuracy.'}
                    buffer=io.BytesIO();Image.fromarray(manual.astype('uint8')*255).save(buffer,format='PNG')
                    result['humanMask']=base64.b64encode(buffer.getvalue()).decode();result['humanMetrics']=metrics;human.append(metrics)
                result['comparisons'].append(comparison)
            report['words'].append(result)
    for split,counts in stats.items():
        report['splits'][split]={**dict(counts),'meanNativeAgreementIoU':float(np.mean(ious[split])) if ious[split] else None}
    report['humanEvaluation']=human
    report['interpretation']='Native agreement measures agreement with automatic templates, not accuracy. All proposals require review. The candidate generator still uses existing contour routes; ranking cannot recover a boundary never proposed.'
    write_json(folder/'report.json',report)
    summary={k:v for k,v in report.items() if k!='words'}
    write_json(folder/'summary.json',summary);print(json.dumps(summary,ensure_ascii=False,indent=2))

def export(folder, executable, baseline, split):
    manifest=json.loads((folder/'inputs.json').read_text());(folder/'candidates').mkdir(exist_ok=True)
    if split!='train' and not (folder/'model.json').exists():raise ValueError('Freeze train-only scorer before exporting held-out pages')
    for page in manifest['pages']:
        name=f'page{page:03}'
        assigned=manifest['pageSplits'][name]
        if split!='all' and assigned!=split:continue
        target=folder/'candidates'/f'{name}.json'
        subprocess.run([str(executable),str(page),str(baseline/'after-cache'/f'{name}.json'),
                        str(baseline/'vector'/f'{name}.json'),str(target),assigned],cwd=NATIVE,check=True,
                       env={**os.environ,'QT_QPA_PLATFORM':'offscreen','MEDINA_LETTER_FORMS':str(NATIVE/'templates/letter-forms.json')})

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=['export','fit','evaluate'])
    parser.add_argument('--folder',type=Path,required=True)
    parser.add_argument('--baseline',type=Path,default=DEFAULT_BASELINE)
    parser.add_argument('--executable',type=Path,default=Path('/private/tmp/medina-hah-shape-candidates'))
    parser.add_argument('--split',choices=['train','validation','test','all'],default='train')
    args=parser.parse_args();folder=args.folder.resolve()
    if args.command=='export':export(folder,args.executable,args.baseline,args.split)
    elif args.command=='fit':fit(folder)
    else:evaluate(folder)

if __name__=='__main__':main()
