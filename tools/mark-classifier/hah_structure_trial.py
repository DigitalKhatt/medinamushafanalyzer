"""Run the route-independent structural-hah experiment with a frozen shape scorer."""
from workbench_paths import NATIVE
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import hah_structure
import hah_shape_trial as ranking

ROOT=Path(__file__).resolve().parent
CONFIG=NATIVE/'templates/experimental/hah-structure.json'
PREVIOUS=ROOT/'data/automatic/experiments/hah-shape-trial-20261105'

def generate(folder, split, pages, executable):
    inputs=json.loads((folder/'inputs.json').read_text())
    if not (folder/'config.json').exists():shutil.copyfile(CONFIG,folder/'config.json')
    config=json.loads((folder/'config.json').read_text())
    for sub in ['geometry','probes','candidates']:(folder/sub).mkdir(exist_ok=True)
    timing=[]
    for page in inputs['pages']:
        name=f'page{page:03}'
        if pages and page not in pages:continue
        if split!='all' and inputs['pageSplits'][name]!=split:continue
        document=json.loads((PREVIOUS/'candidates'/f'{name}.json').read_text())
        words=[];requests=[]
        for old in document['words']:
            # Old route proposals never enter the detector or vector bridge.
            word={k:v for k,v in old.items() if k!='candidates'}
            start=time.perf_counter();features=hah_structure.detect(word['ink'],config)
            elapsed=time.perf_counter()-start
            requests.append({'id':word['id'],**features});words.append(word)
            timing.append({'id':word['id'],'seconds':elapsed,'structures':len(features['structures']),'probes':len(features['probes'])})
        document['words']=words;ranking.write_json(folder/'geometry'/f'{name}.json',document)
        ranking.write_json(folder/'probes'/f'{name}.json',{'words':requests})
        start=time.perf_counter()
        subprocess.run([str(executable),str(folder/'geometry'/f'{name}.json'),str(folder/'probes'/f'{name}.json'),
                        str(folder/'config.json'),str(folder/'candidates'/f'{name}.json')],check=True,cwd=NATIVE,
                       env={**os.environ,'QT_QPA_PLATFORM':'offscreen'})
        output=json.loads((folder/'candidates'/f'{name}.json').read_text())
        print(name,'structures',sum(len(w['structuralDetections']) for w in output['words']),
              'candidates',sum(len(w['candidates']) for w in output['words']),
              'vector seconds',round(time.perf_counter()-start,3),flush=True)
    ranking.write_json(folder/f'timing-{split}.json',timing)
    if not (folder/'model.json').exists():shutil.copyfile(PREVIOUS/'model.json',folder/'model.json')

def freeze(folder):
    files=[Path(hah_structure.__file__),Path(__file__),NATIVE/'tools/hah_structure_candidates.cpp',folder/'config.json',PREVIOUS/'model.json']
    inputs=json.loads((folder/'inputs.json').read_text())
    ranking.write_json(folder/'generator-freeze.json',{'files':{str(p):ranking.digest(p) for p in files},
        'purpose':'Frozen before this revision is evaluated; human mask pixels are evaluation-only',
        'revisionContext':inputs.get('revisionContext','Original structural generation trial'),
        'ranker':'Reused unchanged from the first trial; no retraining',
        'humanReviewed':False,'provenance':'machine-experimental'})

def evaluate(folder):
    frozen=json.loads((folder/'generator-freeze.json').read_text())
    if any(ranking.digest(p)!=h for p,h in frozen['files'].items()):raise ValueError('Generator changed after freeze')
    start=time.perf_counter();ranking.evaluate(folder);elapsed=time.perf_counter()-start
    report=json.loads((folder/'report.json').read_text())
    old={w['id']:w for w in json.loads((PREVIOUS/'report.json').read_text())['words']}
    inputs=json.loads((folder/'inputs.json').read_text())
    comparison_folder=inputs.get('comparisonTrial')
    previous={w['id']:w for w in json.loads((ROOT/comparison_folder/'report.json').read_text())['words']} if comparison_folder else {}
    counts=Counter();novel=0;conservation=[]
    for word in report['words']:
        counts['subwords']+=1;counts['structures']+=len(word['structuralDetections']);counts['candidatePairs']+=len(word['candidates'])
        counts['subwordsWithCandidates']+=bool(word['candidates'])
        selected=set(word['selected'])
        counts['selectedWithAttachingStroke']+=sum(c['id'] in selected and c.get('structuralFeatures',{}).get('entryRule')=='attachment-turn' for c in word['candidates'])
        old_masks=[ranking.decode(c['mask']) for c in old[word['id']]['candidates']]
        for candidate in word['candidates']:
            m=ranking.decode(candidate['mask']);candidate['nearestOldCandidateIoU']=max((ranking.iou(m,a) for a in old_masks),default=0.)
            novel+=candidate['nearestOldCandidateIoU']<.98;conservation.append(candidate['conservation'])
        if 'humanMetrics' in word:
            word['humanMetrics']['oldCandidateOracleIoU']=old[word['id']]['humanMetrics']['oracleIoU']
            word['humanMetrics']['oldTrialIoU']=old[word['id']]['humanMetrics']['trialIoU']
            if word['id'] in previous:
                word['humanMetrics']['previousStructuralIoU']=previous[word['id']]['humanMetrics']['trialIoU']
    report.update(generator='intrinsic-structure',generation={**dict(counts),'novelMasksBelow98IoU':novel,
                  'validPartitions':sum(c['valid'] for c in conservation),'rankingAndEvaluationSeconds':elapsed},
                  evaluationScope='Frozen scorer and generator; known pages 590/600 are regression examples previously inspected, not pristine unseen end-to-end tests.')
    report['interpretation']='Cuts generated from detected mouth, lip, return stroke and attachment graph. No old route proposals or neighboring letter landmarks used. Automatic reference agreement is not accuracy. No labels or production rules changed.'
    report['revisionContext']=inputs.get('revisionContext')
    # Keep enhanced human diagnostics in both summary and per-word views.
    report['humanEvaluation']=[w['humanMetrics'] for w in report['words'] if 'humanMetrics' in w]
    ranking.write_json(folder/'report.json',report);ranking.write_json(folder/'summary.json',{k:v for k,v in report.items() if k!='words'})
    print(json.dumps(report['generation'],indent=2));print(json.dumps(report['humanEvaluation'],indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=['generate','freeze','evaluate'])
    parser.add_argument('--folder',type=Path,required=True)
    parser.add_argument('--split',choices=['train','validation','test','all'],default='train')
    parser.add_argument('--pages',type=int,nargs='*')
    parser.add_argument('--executable',type=Path,default=Path('/private/tmp/medina-hah-structure'))
    args=parser.parse_args();folder=args.folder.resolve()
    if args.command=='generate':generate(folder,args.split,args.pages,args.executable)
    elif args.command=='freeze':freeze(folder)
    else:evaluate(folder)
