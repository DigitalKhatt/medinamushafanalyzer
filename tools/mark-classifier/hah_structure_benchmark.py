"""Reproducible read-only timing of native templates and both structural trials.

No model fitting, report-image generation, cache writes, or label modifications.
Warm timing uses three repetitions on exactly the same 238 raw subwords.
"""
from workbench_paths import NATIVE
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

import numpy as np
import hah_shape_trial as scorer

ROOT=Path(__file__).resolve().parent
BASE=ROOT/'data/automatic/experiments'
TRIALS={name:BASE/f'hah-structure-trial-20261105-{name}' for name in ['v2','v3']}


def module(path, name):
    spec=importlib.util.spec_from_file_location(name,path)
    obj=importlib.util.module_from_spec(spec);spec.loader.exec_module(obj)
    return obj


def bridge_source(destination):
    """Instrument a copy, leaving the frozen geometric implementation untouched."""
    text=(NATIVE/'tools/hah_structure_candidates.cpp').read_text()
    changes=[
        ('#include <QApplication>','#include <QApplication>\n#include <QElapsedTimer>'),
        ('if(argc!=5)qFatal(', 'if(argc!=6)qFatal('),
        ('QJsonArray words;', 'QJsonArray words;QJsonArray benchRounds;const int repeats=QString(argv[5]).toInt();'),
        ('    for(const auto& value:source.value("words").toArray()){',
         '    for(int benchRound=-1;benchRound<repeats;++benchRound){words={};QJsonArray benchWords;\n'
         '    for(const auto& value:source.value("words").toArray()){QElapsedTimer benchTimer;benchTimer.start();'),
        ('words.append(row);','words.append(row);benchWords.append(QJsonObject{{"id",row.value("id")},{"nanoseconds",static_cast<double>(benchTimer.nsecsElapsed())}});'),
        ('    source.insert("words",words);','    if(benchRound>=0)benchRounds.append(benchWords);}\n'
         '    source.insert("benchmarkNanoseconds",benchRounds);\n    source.insert("words",words);')]
    for before,after in changes:
        if text.count(before)!=1:raise ValueError(f'Instrumentation anchor changed: {before}')
        text=text.replace(before,after)
    for header in ['cutter/detail.h','lettersegmentationcache.h']:
        text=text.replace(f'"../{header}"',f'"{NATIVE/header}"')
    destination.write_text(text)


def score_word(word, page, model, protos):
    """The existing inference operations, excluding evaluation against labels."""
    source=scorer.decode(word['sourceMask']);scales=np.array(model['scales']);cfg=model['config']
    for candidate in word['candidates']:
        desc=scorer.descriptor(candidate)
        best=min((scorer.distance(desc,d,scales,cfg)[0],p['id']) for p,d in protos if p['page']!=page)
        candidate['distance']=best[0]
        candidate['conservation']=scorer.partition_check(candidate,source)
    sequences=scorer.assignments(word['candidates'],len(word['targets']))
    by_id={c['id']:c for c in word['candidates']}
    selected=sequences[0][1] if sequences else []
    # Ambiguity checking is part of the present inference path too.
    alternatives=[]
    for cost,seq in sequences[1:]:
        if all(scorer.iou(scorer.decode(by_id[a]['mask']),scorer.decode(by_id[b]['mask']))>=cfg['sameMaskIoU'] for a,b in zip(selected,seq)):continue
        alternatives.append((cost,seq))
    return selected


def stats(rows, repeats):
    ids=sorted(rows)
    matrix=np.array([rows[k] for k in ids],float)
    if matrix.shape!=(len(ids),repeats):raise ValueError('Unbalanced timing repetitions')
    totals=matrix.sum(0)
    medians=np.median(matrix,axis=1)
    return {'words':len(ids),'repetitions':repeats,'totalSecondsMedian':float(np.median(totals)),
            'totalSecondsByRepeat':totals.tolist(),'meanMillisecondsPerWord':float(np.median(totals)/len(ids)*1000),
            'medianMillisecondsPerWord':float(np.median(medians)*1000),'p95MillisecondsPerWord':float(np.percentile(medians,95)*1000)}


def run(folder,repeats):
    folder.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0, str(NATIVE/'tools'))
    from build_native_exporter import build
    source=folder/'native-materialization-timing.cpp';bridge_source(source)
    executable=Path('/private/tmp/medina-hah-materialization-timing')
    build(executable,str(source))
    native_executable=Path('/private/tmp/medina-hah-timing')
    build(native_executable,'hah_timing.cpp')
    env={**os.environ,'QT_QPA_PLATFORM':'offscreen'}
    subprocess.run([str(native_executable),str(TRIALS['v3']/'geometry'),str(TRIALS['v3']/'inputs.json'),
                    str(folder/'native-templates.json'),str(repeats)],cwd=NATIVE,env=env,check=True)
    native=json.loads((folder/'native-templates.json').read_text())
    rows={w['id']:[t/1e9 for t in w['nanoseconds']] for w in native['words']}
    results={'environment':{'platform':platform.platform(),'machine':platform.machine(),'logicalCpus':os.cpu_count(),
                            'python':sys.version,'numpy':np.__version__},'native':stats(rows,repeats),'trials':{},
             'scope':'CPU, warm process, fixed 238 subwords/39 pages, one unmeasured warmup then repeated runs. No GPU, display, page/file loading, process startup, report generation, or human-mask evaluation in core times. Native regenerates every letter in each word; structural trials generate/rank medial hah candidates only.'}
    print('native',results['native'],flush=True)
    for label,trial in TRIALS.items():
        detector=module(trial/'source-snapshot/hah_structure.py',f'hah_{label}')
        config=json.loads((trial/'config.json').read_text());documents=[]
        for path in sorted((trial/'geometry').glob('page*.json')):documents.append(json.loads(path.read_text()))
        detector_rows={};vector_rows={};score_rows={};materialization_wall=0.;generated=0
        for document in documents:
            for word in document['words']:
                detector_rows[word['id']]=[]
        # Alternate detector iterations across whole data rather than hot-looping
        # one tiny crop. Inputs are preloaded, detection and rasterization rerun.
        expected={w['id']:w for path in sorted((trial/'probes').glob('page*.json')) for w in json.loads(path.read_text())['words']}
        for repeat in range(-1,repeats):
            for document in documents:
                for word in document['words']:
                    start=time.perf_counter();found=detector.detect(word['ink'],config);elapsed=time.perf_counter()-start
                    if repeat>=0:detector_rows[word['id']].append(elapsed)
                    if repeat==-1 and found!={k:v for k,v in expected[word['id']].items() if k!='id'}:raise ValueError('Detector output drifted')
            print(label,'detection warmup' if repeat<0 else f'detection repeat {repeat+1}',flush=True)
        output=folder/label;output.mkdir(exist_ok=True)
        for document in documents:
            name=f"page{document['page']:03}"
            start=time.perf_counter()
            subprocess.run([str(executable),str(trial/'geometry'/f'{name}.json'),str(trial/'probes'/f'{name}.json'),
                            str(trial/'config.json'),str(output/f'{name}.json'),str(repeats)],cwd=NATIVE,env=env,check=True)
            materialization_wall+=time.perf_counter()-start
            value=json.loads((output/f'{name}.json').read_text());samples=value.pop('benchmarkNanoseconds')
            original=json.loads((trial/'candidates'/f'{name}.json').read_text())
            if value!=original:raise ValueError('Materialized candidate output drifted')
            for repeat,sample in enumerate(samples):
                for item in sample:vector_rows.setdefault(item['id'],[]).append(item['nanoseconds']/1e9)
            generated+=sum(len(w['candidates']) for w in value['words'])
        print(label,'materialization complete',flush=True)
        model=json.loads((trial/'model.json').read_text())
        protos=[(p,scorer.restore_descriptor(p['descriptor'])) for p in model['prototypes']]
        candidates=[json.loads(path.read_text()) for path in sorted((trial/'candidates').glob('page*.json'))]
        expected_choices={w['id']:w['selected'] for w in json.loads((trial/'report.json').read_text())['words']}
        for repeat in range(-1,repeats):
            for document in candidates:
                for word in document['words']:
                    start=time.perf_counter();selected=score_word(word,document['page'],model,protos);elapsed=time.perf_counter()-start
                    if selected!=expected_choices[word['id']]:raise ValueError('Scorer selection drifted')
                    if repeat>=0:score_rows.setdefault(word['id'],[]).append(elapsed)
            print(label,'scoring warmup' if repeat<0 else f'scoring repeat {repeat+1}',flush=True)
        total_rows={k:[a+b+c for a,b,c in zip(detector_rows[k],vector_rows[k],score_rows[k])] for k in detector_rows}
        results['trials'][label]={'detection':stats(detector_rows,repeats),'vectorCutsAndMasks':stats(vector_rows,repeats),
             'scoringAndChecks':stats(score_rows,repeats),'coreTotal':stats(total_rows,repeats),'candidates':generated,
             'exactOutputsUnchanged':True,'bridgeProcessWallSecondsIncludingWarmupAndAllRepeats':materialization_wall}
        scorer.write_json(folder/f'{label}-raw.json',{'detection':detector_rows,'vectorCutsAndMasks':vector_rows,'scoringAndChecks':score_rows})
        scorer.write_json(folder/'timing.json',results)
        print(label,results['trials'][label]['coreTotal'],flush=True)
    results['native']['stableOutputs']=all(w['stable'] for w in native['words'])
    results['native']['completeWords']=sum(w['complete'] for w in native['words'])
    from cutter_source import cutter_source_digest
    results['preservation']={str(p):scorer.digest(p) for p in [ROOT/'hah_structure.py',ROOT/'hah_shape_trial.py',NATIVE/'templates/letter-forms.json',TRIALS['v3']/'model.json']}
    results['preservation'][str(NATIVE/'segmentsubword.cpp')]=cutter_source_digest(NATIVE)
    results['nativeVsV3CoreRatio']=results['trials']['v3']['coreTotal']['totalSecondsMedian']/results['native']['totalSecondsMedian']
    scorer.write_json(folder/'timing.json',results)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder',type=Path,default=BASE/'hah-timing-20261105')
    parser.add_argument('--repeats',type=int,default=3)
    args=parser.parse_args();run(args.folder.resolve(),args.repeats)
