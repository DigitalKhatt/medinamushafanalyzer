"""Fine-tune the frozen two-role Mask2Former and compare on page-held-out ink."""
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import random
import sys
import time

if os.environ.get('MUSHAF_TORCH_DEPS'):
    sys.path.append(os.environ['MUSHAF_TORCH_DEPS'])
os.environ.setdefault('HF_HUB_OFFLINE','1')

import numpy as np
import torch

from letter_retest import (SEED, canvas_example, load_example, loss_for,
                           model_for, predict, summarize)
from letter_template_repeat import sha, write
from letter_bases import has_training_boundary

TOOTH = 'بتثنيئى'
SEEN = 'سش'
STEPS = (120, 240, 480)


def read(path):
    return json.loads(path.read_bytes())


def compact(folder, record):
    e = load_example(folder, record)
    if e['labels'].max() > 255:
        raise ValueError('Canvas exceeds eight-bit instance IDs')
    e['canvas'] = e['canvas'].byte()
    return e


def get_model(base, weights, device):
    model = model_for('mask2former', str(base/'pretrained'), device, 'random')
    model.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
    return model


def batches(pool, rng):
    if not pool:
        raise ValueError('Empty training group')
    pool = list(pool)
    while True:
        rng.shuffle(pool)
        yield from pool


def stats_for_letter(labels, prediction, roles, letter):
    truth = labels == letter
    overlap = np.bincount(prediction[truth], minlength=len(roles))
    area = np.bincount(prediction.ravel(), minlength=len(roles))
    iou = overlap / np.maximum(1, truth.sum() + area - overlap)
    major = (overlap >= max(10, .1 * truth.sum())) & (2 * overlap > area)
    eligible = np.flatnonzero(roles == 1)
    best = int(eligible[np.argmax(iou[eligible])]) if len(eligible) else 0
    return dict(best=best, iou=float(iou[best]) if best else 0.,
                bodyFragments=int((major & (roles == 1)).sum()),
                innerConnectors=int((major & (roles == 2)).sum()))


def shape_scores(example, prediction, roles):
    text = example['record']['text']
    seen, neighbors = [], []
    for i, c in enumerate(text):
        if c not in SEEN:
            continue
        l = stats_for_letter(example['labels'], prediction, roles, i + 1)
        l['seenReh'] = text[i+1:i+2] == 'ر'
        l['slot'] = i + 1
        seen.append(l)
        if i and text[i-1] in TOOTH:
            prev = stats_for_letter(example['labels'], prediction, roles, i)
            neighbors.append(bool(prev['best'] != l['best'] and min(prev['iou'],l['iou']) >= .5))
    return dict(seen=seen, neighbors=neighbors,
                predictedBodies=int(sum(roles == 1)), expectedBodies=len(text))


def overview(rows):
    def group(rows, reh_only=False):
        letters = [l for row in rows for l in row['shape']['seen'] if not reh_only or l['seenReh']]
        return dict(words=len(rows),seenLetters=len(letters),
                    fragmented=sum(l['bodyFragments'] >= 2 for l in letters),
                    intact75=sum(l['iou'] >= .75 and l['bodyFragments'] == 1 for l in letters),
                    meanSeenIoU=sum(l['iou'] for l in letters)/max(len(letters),1),
                    internalKashida=sum(bool(l['innerConnectors']) for l in letters),
                    overcounted=sum(r['shape']['predictedBodies'] > r['shape']['expectedBodies'] for r in rows),
                    metrics=summarize([r['metrics'] for r in rows]))
    neighbors = [x for row in rows for x in row['shape']['neighbors']]
    return dict(seenReh=group([r for r in rows if r['seenReh']],True),
                previouslyOvercounted=group([r for r in rows if r['previouslyOvercounted']]),
                separateTooth=dict(pairs=len(neighbors),bothRecovered=sum(neighbors)),
                all=group(rows))


def evaluate_targets(model, examples, device, settings, folder=None):
    results = []
    for example in examples:
        rec = example['record']
        p = predict(model,'mask2former',[example],device,settings['threshold'],min_region_area=settings['decoder'])[0]
        shape = shape_scores(example,p['prediction'],p['roles'])
        results.append(dict(id=rec['id'],seenReh=rec['seenReh'],previouslyOvercounted=rec['previouslyOvercounted'],
                            shape=shape,metrics=p['metrics']))
        if folder:
            save_prediction(folder,rec['id'],p)
    return results


def save_prediction(folder, sid, p):
    folder.mkdir(exist_ok=True,parents=True)
    np.savez_compressed(folder/(sid.replace(':','__')+'.npz'),instances=p['prediction'],roles=p['roles'],raw=p['raw'])


def general_metrics(model, examples, device, settings):
    rows = predict(model,'mask2former',examples,device,settings['threshold'],min_region_area=settings['decoder'])
    return summarize([p['metrics'] for p in rows])


def selection(targets, general, reference):
    panel = targets['seenReh']
    controls = targets['separateTooth']
    intact = panel['intact75']/max(1,panel['seenLetters'])
    hard = targets['previouslyOvercounted']
    hard_intact = hard['intact75']/max(1,hard['seenLetters'])
    neighbor = controls['bothRecovered']/max(1,controls['pairs'])
    general_f1 = general['roleAwareInstanceF1AtIoU50']
    eligible = (reference is None or
                (general_f1 >= reference['generalF1']-.015 and neighbor >= reference['neighborRate']-.05))
    return dict(score=.5*intact+.25*hard_intact+.15*neighbor+.10*general_f1, eligible=eligible,
                generalF1=general_f1,neighborRate=neighbor)


def fit(base, folder, device):
    if (folder/'training.json').exists() or (folder/'best.pt').exists():
        raise ValueError('Existing trial checkpoint; use a fresh folder')
    manifest = read(folder/'dataset.json')
    old = read(base/'dataset.json')
    if manifest['frozenOriginalDataset'] != sha(base/'dataset.json'):
        raise ValueError('Frozen baseline changed')
    settings = read(base/'mask2former/settings.json')
    seed = SEED
    torch.manual_seed(seed); np.random.seed(seed); rng=random.Random(seed)
    training = [compact(folder,r) for r in manifest['records'] if r['split']=='train' and has_training_boundary(r)]
    validation = [compact(folder,r) for r in manifest['records'] if r['split']=='validation']
    fresh_ids = {e['record']['id'] for e in training}
    replay_exclusions = fresh_ids | set(manifest.get('excludedFromOldReplay', ()))
    replay = [compact(base,r) for r in old['records'] if r['group']=='weak_train' and
              r['id'] not in replay_exclusions and has_training_boundary(r)]
    human_rows = [r for r in old['records'] if r['group']=='human_train']
    legacy = [r for r in human_rows if 'baseCharacterCount' not in r and 'textForDisplayOnly' not in r]
    if legacy:
        from letter_training_gallery import reviewed_display_text
        captions = reviewed_display_text(base, legacy)
        human_rows = [dict(r, textForDisplayOnly=captions[r['id']]) if r['id'] in captions else r for r in human_rows]
    reviewed = [compact(base,r) for r in human_rows if has_training_boundary(r)]
    val_records = sorted((r for r in old['records'] if r['group']=='weak_validation'),
                         key=lambda r:hashlib.sha256(r['id'].encode()).digest())[:160]
    general_val = [compact(base,r) for r in val_records]
    seen_pages=lambda examples:{e['record']['page'] for e in examples}
    all_fitting=training+replay+reviewed
    test_pages={r['page'] for r in old['records']+manifest['records'] if r['split']=='test'}
    if seen_pages(all_fitting) & (seen_pages(validation+general_val)|test_pages):
        raise ValueError('Page split leaked into fitting')
    hard = batches([e for e in training if e['record']['previouslyOvercounted']],rng)
    seen_reh = batches([e for e in training if e['record']['seenReh']],rng)
    tooth = batches([e for e in training if e['record']['separateTooth']],rng)
    ordinary = batches(replay,rng)
    human = batches(reviewed,rng)
    parent = base/'mask2former/best.pt'
    model = get_model(base,parent,device)
    encoder,head=[],[]
    for name,p in model.named_parameters():
        (encoder if name.startswith('model.pixel_level_module.encoder.') else head).append(p)
    rates=[5e-6,5e-5]
    opt=torch.optim.AdamW([{'params':encoder,'lr':rates[0]},{'params':head,'lr':rates[1]}],weight_decay=.01)
    history=[]; best_score=float('-inf');counts=Counter();start=time.monotonic()
    sb = dict(parentCheckpoint=str(parent),parentSha256=sha(parent),steps=list(STEPS),seed=seed,rates=rates,
              batch=['previously overcounted seen','seen-reh or separate tooth, alternating','ordinary replay',
                     'ordinary replay or a human-training crop every fifth batch'],
              checkpointSelection='0.50 seen-reh intact75 + 0.25 hard seen intact75 + 0.15 separate tooth + 0.10 general role F1; maximum general drop 0.015 and neighbor drop 0.05',
              decoder=settings['decoder'],threshold=settings['threshold'],classes=['body','kashida'],
              trainingPools=dict(targeted=len(training),seenReh=sum(e['record']['seenReh'] for e in training),
                     previouslyOvercounted=sum(e['record']['previouslyOvercounted'] for e in training),
                     separateTooth=sum(e['record']['separateTooth'] for e in training),
                     previousWeakReplay=len(replay),humanReplay=len(reviewed)),
              targetMaskSource='Native automatic templates; reviewed replay retains original provenance',
              generalValidationIds=[r['id'] for r in val_records],device=str(device),torchVersion=torch.__version__,
              datasetSha256=sha(folder/'dataset.json'),transcriptAtInference=False)
    write(folder/'recipe.json',sb)
    for step in range(STEPS[-1]+1):
        if step:
            model.train()
            batch=[next(hard),next(seen_reh if step%2 else tooth),next(ordinary),
                   next(human if reviewed and step%5==0 else ordinary)]
            counts.update(e['record']['id'] for e in batch)
            rate=min(1,step/20)*(.15+.85*(1+math.cos(math.pi*step/STEPS[-1]))/2)
            for group,lr in zip(opt.param_groups,rates):group['lr']=lr*rate
            opt.zero_grad(set_to_none=True)
            loss=loss_for(model,'mask2former',batch,device,rng)
            if not torch.isfinite(loss):raise ValueError(f'Nonfinite loss at {step}')
            loss.backward();torch.nn.utils.clip_grad_norm_(model.parameters(),.1);opt.step()
            if step%40==0:print('step',step,'loss',round(loss.item(),2),'seconds',round(time.monotonic()-start),flush=True)
        if step not in (0,*STEPS): continue
        t=overview(evaluate_targets(model,validation,device,settings))
        general=general_metrics(model,general_val,device,settings)
        choice=selection(t,general,history[0]['selection'] if history else None)
        history.append(dict(step=step,targets=t,general=general,selection=choice))
        print('validation',step,choice,flush=True)
        if choice['eligible'] and choice['score']>best_score:
            best_score=choice['score'];best_step=step
            # Use CPU tensors so this checkpoint is portable across devices.
            torch.save({k:v.detach().cpu() for k,v in model.state_dict().items()},folder/'best.tmp')
            (folder/'best.tmp').replace(folder/'best.pt')
            write(folder/'sample-counts-at-best.json',dict(counts))
        write(folder/'history.json',history)
    write(folder/'training.json',dict(selectedStep=best_step,steps=STEPS[-1],history='history.json',
          checkpointSha256=sha(folder/'best.pt'),seconds=time.monotonic()-start,
          fittingPages=sorted(seen_pages(all_fitting)),sampledExamples=len(counts),samples=sum(counts.values())))
    write(folder/'sample-counts.json',dict(counts))


def count_panel(base, model, settings, device):
    old_rows = read(base/'unfinished-report/items.json')
    rows=[]
    for r in old_rows:
        if r['split']=='train' or r['countDifference']<=0 or not any(c in SEEN for c in r['baseText']):
            continue
        with np.load(base/'unfinished-report'/r['predictionFile']) as data:
            ink = data['sourceInk'].astype('uint16')
            old_bodies = int(sum(data['roles']==1))
        if old_bodies!=r['predictedBodyCount']:
            raise ValueError('Archived prediction has inconsistent counts')
        rec=dict(id=r['id'],group='count_only',pdfToRaster=r['pdfToRaster'])
        e=canvas_example(rec,ink,np.array([0,1],'uint8'))
        p=predict(model,'mask2former',[e],device,settings['threshold'],min_region_area=settings['decoder'])[0]
        rows.append(dict(id=r['id'],split=r['split'],expected=len(r['baseText']),previous=old_bodies,
                         trial=int(sum(p['roles']==1))))
    return rows


def evaluate(base, folder, device):
    training=read(folder/'training.json')
    if sha(folder/'best.pt')!=training['checkpointSha256'] or sha(base/'mask2former/best.pt')!=read(folder/'recipe.json')['parentSha256']:
        raise ValueError('Changed checkpoint')
    manifest = read(folder/'dataset.json')
    settings=read(base/'mask2former/settings.json')
    old=read(base/'dataset.json')
    examples=[compact(folder,r) for r in manifest['records'] if r['split']!='train' or
              r['id'] in {'page116:l8-w8-s1','page109:l5-w8-s2','page005:l11-w5-s2'}]
    old_general=read(base/'mask2former/evaluation.json')['groups']
    scores={'general':{},'targets':{},'countPanel':{},'selectedStep':training['selectedStep']}
    for name,weights in [('previous',base/'mask2former/best.pt'),('trial',folder/'best.pt')]:
        model=get_model(base,weights,device)
        rows=evaluate_targets(model,examples,device,settings,folder/'predictions'/name)
        by_id={r['id']:r for r in rows}
        write(folder/(name+'-examples.json'),rows)
        scores['targets'][name]={}
        for split in ('validation','test'):
            matching=[by_id[e['record']['id']] for e in examples if e['record']['split']==split]
            scores['targets'][name][split]=overview(matching)
        if name=='previous':
            scores['general'][name]={k:old_general[k] for k in ['weak_validation','weak_test','human_validation']}
        else:
            scores['general'][name]={}
            for group in ('weak_validation','weak_test','human_validation'):
                subset=[compact(base,r) for r in old['records'] if r['group']==group]
                scores['general'][name][group]=general_metrics(model,subset,device,settings)
                print(group,scores['general'][name][group]['roleAwareInstanceF1AtIoU50'],flush=True)
            cp=count_panel(base,model,settings,device)
            write(folder/'count-panel.json',cp)
            for split in ('validation','test'):
                sub=[r for r in cp if r['split']==split]
                scores['countPanel'][split]=dict(words=len(sub),previousOver=len(sub),
                    trialOver=sum(r['trial']>r['expected'] for r in sub),
                    trialUnder=sum(r['trial']<r['expected'] for r in sub),
                    trialExact=sum(r['trial']==r['expected'] for r in sub))
        del model
    write(folder/'evaluation.json',scores)
    print('Evaluation saved',folder/'evaluation.json',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('fit','evaluate','run'))
    parser.add_argument('--baseline',type=Path, required=True)
    parser.add_argument('--folder',type=Path, required=True)
    parser.add_argument('--device',choices=('mps','cpu','cuda'),default='cpu')
    args=parser.parse_args()
    if args.device=='mps' and not torch.backends.mps.is_available():
        raise SystemExit('Metal unavailable in this process')
    torch.set_num_threads(4)
    if args.action in ('fit','run'):fit(args.baseline.resolve(),args.folder.resolve(),torch.device(args.device))
    if args.action in ('evaluate','run'):evaluate(args.baseline.resolve(),args.folder.resolve(),torch.device(args.device))
    

if __name__=='__main__':
    main()
