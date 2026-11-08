"""Train/evaluate image-only Mask2Former and SegFormer on a frozen letter dataset."""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import random
import shutil
import time

import numpy as np
from PIL import Image
from scipy import ndimage as ndi
from scipy.optimize import linear_sum_assignment
from skimage.segmentation import watershed
import torch
from torch.nn import functional as F

CANVAS = (160, 224)
SEED = 63


def boundary(labels):
    result = np.zeros(labels.shape, bool)
    for dy, dx in ((1, 0), (0, 1), (1, 1), (1, -1)):
        other = np.roll(labels, (dy, dx), (0, 1))
        hit = (labels > 0) & (other > 0) & (labels != other)
        if dy: hit[:dy] = False
        if dx > 0: hit[:, :dx] = False
        if dx < 0: hit[:, dx:] = False
        result |= hit | np.roll(hit, (-dy, -dx), (0, 1))
    return result


def metric(truth, prediction, truth_roles=None, predicted_roles=None):
    a, b = np.unique(truth[truth > 0]), np.unique(prediction[prediction > 0])
    matches, iou_sum, hits75, role_matches = 0, 0., 0, 0
    if len(a) and len(b):
        inter = np.array([[( (truth == i) & (prediction == j)).sum() for j in b] for i in a])
        sizes_a = np.array([(truth == i).sum() for i in a])
        sizes_b = np.array([(prediction == j).sum() for j in b])
        iou = inter / (sizes_a[:, None] + sizes_b - inter)
        rows, cols = linear_sum_assignment(-iou)
        matched = iou[rows, cols]; matches = int((matched >= .5).sum())
        hits75 = int((matched >= .75).sum()); iou_sum = float(matched.sum())
        if truth_roles is not None and predicted_roles is not None:
            role_matches = int(sum(v >= .5 and truth_roles[a[r]] == predicted_roles[b[c]]
                                   for r, c, v in zip(rows, cols, matched)))
    ta, pb = boundary(truth), boundary(prediction)
    hit_p = int((pb & (ndi.distance_transform_edt(~ta) <= 2)).sum()) if ta.any() else 0
    hit_t = int((ta & (ndi.distance_transform_edt(~pb) <= 2)).sum()) if pb.any() else 0
    return dict(referenceRegions=len(a), predictedRegions=len(b), matchesAtIoU50=matches,
        matchesAtIoU75=hits75, matchedIoUSum=iou_sum, roleCorrectMatchesAtIoU50=role_matches,
        coveredInkPixels=int(((truth > 0) & (prediction > 0)).sum()), sourceInkPixels=int((truth > 0).sum()),
        boundaryPredicted=int(pb.sum()), boundaryReference=int(ta.sum()),
        boundaryMatchedPredicted=hit_p, boundaryMatchedReference=hit_t,
        absoluteCountError=abs(len(a)-len(b)), exactCount=int(len(a)==len(b)), examples=1)


def summarize(rows):
    if not rows: return {}
    totals = {k: sum(r[k] for r in rows) for k in rows[0]}
    denom = totals['referenceRegions']+totals['predictedRegions']
    for name, key in [('instanceF1AtIoU50','matchesAtIoU50'),('instanceF1AtIoU75','matchesAtIoU75'),
                      ('roleAwareInstanceF1AtIoU50','roleCorrectMatchesAtIoU50')]:
        totals[name] = 2*totals[key]/max(1, denom)
    totals['meanIoUPerReference'] = totals['matchedIoUSum']/max(1,totals['referenceRegions'])
    totals['inkCoverage'] = totals['coveredInkPixels']/max(1,totals['sourceInkPixels'])
    p = totals['boundaryMatchedPredicted']/max(1,totals['boundaryPredicted'])
    r = totals['boundaryMatchedReference']/max(1,totals['boundaryReference'])
    totals['boundaryF1Within2px'] = 2*p*r/max(1e-12,p+r)
    totals['meanAbsoluteCountError'] = totals['absoluteCountError']/totals['examples']
    totals['exactCountRate'] = totals['exactCount']/totals['examples']
    return totals


def canvas_example(record, labels, roles, canvas_shape=CANVAS):
    h, w = labels.shape
    height, width = canvas_shape
    scale = min(height * (1 - 16 / CANVAS[0]) / h, width * (1 - 16 / CANVAS[1]) / w)
    shape = (max(1,round(h*scale)), max(1,round(w*scale)))
    resized = np.array(Image.fromarray(labels).resize(shape[::-1],Image.Resampling.NEAREST))
    canvas = np.zeros(canvas_shape, np.int64)
    oy, ox = (height-shape[0])//2, (width-shape[1])//2
    canvas[oy:oy+shape[0],ox:ox+shape[1]] = resized
    return dict(record=record, labels=labels, roles=roles, canvas=torch.from_numpy(canvas),
                box=(oy,ox,*shape))


def resize_example(example, canvas_shape):
    if canvas_shape is None or tuple(canvas_shape) == tuple(example['canvas'].shape):
        return example
    resized = canvas_example(example['record'], example['labels'], example['roles'], canvas_shape)
    return {**example, 'canvas': resized['canvas'], 'box': resized['box']}


def load_example(folder, record):
    path = folder/record['file']
    if hashlib.sha256(path.read_bytes()).hexdigest() != record['maskSha256']:
        raise ValueError(f'Changed frozen mask: {path}')
    with np.load(path) as source:
        return canvas_example(record, source['instances'], source['roles'])


def load_dataset(folder, *, for_training=False):
    manifest = json.loads((folder/'dataset.json').read_text())
    if 'excludedReviewedIds' in manifest:
        excluded = set(manifest['excludedReviewedIds'])
        for record in manifest['records']:
            if (record['id'] in excluded or record['provenance'] != 'machine-weak'
                    or record['group'] != 'weak_' + record['split']):
                raise ValueError(f"Cutter-only dataset contains an ineligible crop: {record['id']}")
    if for_training and manifest.get('trainingReview', {}).get('status') == 'open':
        raise ValueError('This training set is open for review. Freeze the included examples with letter_training_review.py freeze and use the resulting folder.')
    if for_training:
        from letter_bases import check_training_records
        check_training_records(manifest['records'])
    return manifest, [load_example(folder, record) for record in manifest['records']]


def inputs(labels, device):
    image = (labels == 0).float()[:,None].expand(-1,3,-1,-1).to(device)
    mean = torch.tensor([.485,.456,.406],device=device)[None,:,None,None]
    std = torch.tensor([.229,.224,.225],device=device)[None,:,None,None]
    return (image-mean)/std


def native(logits, example):
    oy,ox,h,w = example['box']
    resized = F.interpolate(logits[None],example['canvas'].shape,mode='bilinear',align_corners=False)
    return F.interpolate(resized[:,:,oy:oy+h,ox:ox+w],example['labels'].shape,
                         mode='bilinear',align_corners=False)[0]


def minimum_region_area(policy=3, record=None):
    if not isinstance(policy, dict): return policy
    transform=(record or {}).get('pdfToRaster')
    scale=abs(transform[0]*transform[3]-transform[1]*transform[2]) if transform else policy['referenceRasterAreaScale']
    return policy['fraction']*policy['meanToothAnalyzerArea']*scale


def decoder_options(policy=3, record=None):
    return dict(min_region_area=minimum_region_area(policy, record),
                minimum_owned_fraction=policy.get('minimumOwnedFraction', 0) if isinstance(policy, dict) else 0)


def mask_decode(mask_probs, class_probs, ink, threshold=.5, complete=True, min_region_area=3,
                minimum_owned_fraction=0):
    if not math.isfinite(minimum_owned_fraction) or not 0 <= minimum_owned_fraction <= 1:
        raise ValueError('Minimum owned fraction must be between zero and one')
    confidence = class_probs[:,:-1].max(1)
    # A shared sīn/shīn class still exports the ordinary letter-body role.
    role = np.where(class_probs[:,:-1].argmax(1) == 1, 2, 1)
    best_class = class_probs.argmax(1)
    valid = (confidence >= threshold) & (best_class != class_probs.shape[1] - 1)
    if not valid.any(): return np.zeros(ink.shape,'uint16'), np.zeros(1,'uint8'), np.zeros(ink.shape,'uint16')
    selected = np.flatnonzero(valid)
    if minimum_owned_fraction:
        candidates = mask_probs[selected]
        winner = (candidates * confidence[selected, None, None]).argmax(0)
        support = (candidates >= .5) & ink
        owned = support & (winner == np.arange(len(selected))[:, None, None])
        fraction = owned.sum(axis=(1, 2)) / np.maximum(1, support.sum(axis=(1, 2)))
        selected = selected[fraction >= minimum_owned_fraction]
        if not len(selected):
            return np.zeros(ink.shape,'uint16'), np.zeros(1,'uint8'), np.zeros(ink.shape,'uint16')
    strengths = mask_probs[selected]*confidence[selected,None,None]
    winner = strengths.argmax(0); strength = strengths.max(0)
    raw = np.where(ink & (mask_probs[selected][winner,*np.indices(ink.shape)] >= .5),winner+1,0)
    # Discard queries with no supported ink and keep only real source pixels.
    if not math.isfinite(min_region_area) or min_region_area < 0:
        raise ValueError('Minimum region area must be finite and nonnegative')
    keep = [i for i in np.unique(raw) if i and (raw == i).sum() >= min_region_area]
    result = np.zeros(ink.shape,'uint16'); output_roles=[0]
    for new, old in enumerate(keep,1): result[raw == old] = new; output_roles.append(role[selected[old-1]])
    raw = result.copy()
    if complete and result.any():
        # Deterministic ink-conservation decoder; this adds no stroke pixels.
        near = ndi.distance_transform_edt(result == 0,return_distances=False,return_indices=True)
        result[ink & (result == 0)] = result[tuple(near)][ink & (result == 0)]
    return result, np.asarray(output_roles,'uint8'), raw


def seg_decode(probabilities, ink, threshold=.5):
    roles = np.argmax(probabilities[1:3],axis=0)+1
    markers = np.zeros(ink.shape,'int32'); output_roles=[0]
    minimum = max(3,int(ink.sum()*.003))
    for role in (1,2):
        seeds, n = ndi.label(ink & (roles == role) & (probabilities[3] < threshold), np.ones((3,3)))
        for i in range(1,n+1):
            region = seeds == i
            if region.sum() < minimum: continue
            markers[region] = len(output_roles); output_roles.append(role)
    # Unseeded source components remain explicit single-region predictions.
    components, n = ndi.label(ink,np.ones((3,3)))
    for i in range(1,n+1):
        component = components == i
        if not markers[component].any():
            markers[component] = len(output_roles)
            output_roles.append(int(np.bincount(roles[component],minlength=3)[1:].argmax()+1))
    result = watershed(probabilities[3],markers,mask=ink,connectivity=np.ones((3,3))).astype('uint16')
    return result,np.array(output_roles,'uint8'),result.copy()


def model_for(name, checkpoint, device, initialization='pretrained', seen_body_class=False,
              geometry_input=False):
    if initialization not in ('pretrained','random'):
        raise ValueError('Initialization must be pretrained or random')
    if name == 'mask2former':
        from transformers import Mask2FormerConfig, Mask2FormerForUniversalSegmentation
        labels = ['body', 'kashida']
        if seen_body_class:
            labels.append('seen-body')
        options=dict(num_labels=len(labels),train_num_points=2048,use_auxiliary_loss=True,
                     id2label=dict(enumerate(labels)),label2id={label: i for i, label in enumerate(labels)})
        if initialization=='random':
            config=Mask2FormerConfig.from_pretrained(checkpoint,local_files_only=True,
                use_pretrained_backbone=False,**options)
            if config.backbone_config is None:
                raise ValueError('Random initialization requires a bundled backbone configuration')
            model=Mask2FormerForUniversalSegmentation(config)
        else:
            model=Mask2FormerForUniversalSegmentation.from_pretrained(checkpoint,
                ignore_mismatched_sizes=True,**options)
    else:
        from transformers import SegformerConfig, SegformerForSemanticSegmentation
        options=dict(num_labels=4,id2label={0:'background',1:'body',2:'kashida',3:'boundary'},
                     label2id={'background':0,'body':1,'kashida':2,'boundary':3})
        if initialization=='random':
            config=SegformerConfig.from_pretrained(checkpoint,local_files_only=True,**options)
            model=SegformerForSemanticSegmentation(config)
        else:
            model=SegformerForSemanticSegmentation.from_pretrained(checkpoint,
                ignore_mismatched_sizes=True,**options)
    if geometry_input:
        from letter_ink_geometry import GeometryProjection

        patch = model.model.pixel_level_module.encoder.embeddings.patch_embeddings
        patch.projection = GeometryProjection(patch.projection, geometry_input)
    return model.to(device)


def state_digest(model):
    result=hashlib.sha256()
    for name,value in sorted(model.state_dict().items()):
        result.update(name.encode());result.update(value.detach().cpu().contiguous().numpy().tobytes())
    return result.hexdigest()


@torch.inference_mode()
def predict(model, name, records, device, threshold=.5, save_probabilities=False, min_region_area=3,
            canvas_shape=None):
    model.eval(); result=[]
    for e in records:
        e = resize_example(e, canvas_shape)
        out = model(pixel_values=inputs(e['canvas'][None],device))
        ink = e['labels'] > 0
        if name == 'mask2former':
            prob = native(out.masks_queries_logits[0],e).sigmoid().cpu().numpy()
            classes = out.class_queries_logits[0].softmax(-1).cpu().numpy()
            pred, roles, raw = mask_decode(prob,classes,ink,threshold,
                **decoder_options(min_region_area,e['record']))
        else:
            prob = native(out.logits[0],e).softmax(0).cpu().numpy(); classes=None
            pred, roles, raw = seg_decode(prob,ink,threshold)
        row=dict(id=e['record']['id'],group=e['record']['group'],prediction=pred,roles=roles,raw=raw,
                 metrics=metric(e['labels'],pred,e['roles'],roles),rawMetrics=metric(e['labels'],raw,e['roles'],roles))
        if save_probabilities: row.update(probabilities=prob,classes=classes)
        result.append(row)
    return result


def seen_target_classes(letters, roles):
    indices = [letter['letterIndex'] for letter in letters]
    if indices != np.flatnonzero(roles == 1).tolist():
        raise ValueError('Body instance order does not match the letter metadata')
    classes = roles.copy()
    for letter in letters:
        if letter['character'] in 'سش':
            classes[letter['letterIndex']] = 3
    return classes


def query_ownership_loss(mask_logits, class_logits, targets, matches, points_per_instance=128):
    """Give each labeled ink pixel the query matched to its instance."""
    foreground = class_logits.log_softmax(-1)[..., :-1].amax(-1)
    losses = []
    for masks, confidence, regions, (query_ids, region_ids) in zip(
            mask_logits, foreground, targets, matches, strict=True):
        height, width = regions.shape[-2:]
        coordinates = []
        for region_id in region_ids.tolist():
            pixels = regions[region_id].nonzero()
            # Even sampling gives thin connectors as much weight as large bodies.
            indices = torch.arange(points_per_instance, device=pixels.device) * len(pixels) // points_per_instance
            coordinates.append(pixels[indices])
        if not coordinates:
            continue
        pixels = torch.cat(coordinates).to(dtype=masks.dtype)
        grid = (pixels[:, [1, 0]] + .5) * masks.new_tensor([2 / width, 2 / height]) - 1
        sampled = F.grid_sample(masks[None], grid[None, None], align_corners=False)[0, :, 0]
        # Match the decoder's confidence × mask probability competition.
        ownership = F.logsigmoid(sampled) + confidence[:, None]
        owners = query_ids.to(masks.device).repeat_interleave(points_per_instance)
        losses.append(F.cross_entropy(ownership.T, owners))
    return torch.stack(losses).mean() if losses else mask_logits.sum() * 0


def augmented_labels(batch, device, rng):
    labels = torch.stack([e['canvas'] for e in batch]).to(device)
    # Small rotations and translations preserve calligraphic direction and
    # never mirror Arabic shapes. Both image and target use the same transform.
    theta=[]
    for _ in batch:
        a=math.radians(rng.uniform(-3,3)); s=rng.uniform(.92,1.05)
        theta.append([[s*math.cos(a),-s*math.sin(a),rng.uniform(-.035,.035)],
                      [s*math.sin(a),s*math.cos(a),rng.uniform(-.035,.035)]])
    grid=F.affine_grid(torch.tensor(theta,device=device),[len(batch),1,*labels.shape[-2:]],align_corners=False)
    return F.grid_sample(labels[:,None].float(),grid,mode='nearest',align_corners=False)[:,0].long()


def loss_for(model,name,batch,device,rng,grouping_weight=0):
    labels = augmented_labels(batch, device, rng)
    image=inputs(labels,device)
    if name == 'mask2former':
        targets=[]; classes=[]
        for label,e in zip(labels,batch):
            ids=label.unique();ids=ids[ids>0]
            targets.append((label[None]==ids[:,None,None]).float())
            classes.append(torch.tensor(e.get('classRoles', e['roles']),device=device)[ids].long()-1)
        if not grouping_weight:
            return model(pixel_values=image,mask_labels=targets,class_labels=classes).loss
        matches = []

        def save_final_matches(_module, _inputs, output):
            # The criterion matches the final layer before its auxiliary layers.
            if not matches:
                matches.extend(output)

        handle = model.criterion.matcher.register_forward_hook(save_final_matches)
        try:
            output = model(pixel_values=image, mask_labels=targets, class_labels=classes)
        finally:
            handle.remove()
        grouping = query_ownership_loss(output.masks_queries_logits, output.class_queries_logits, targets, matches)
        return output.loss + grouping_weight * grouping
    semantic=[]
    for label,e in zip(labels.cpu().numpy(),batch):
        target=e['roles'][label].astype('int64'); target[boundary(label)]=3;semantic.append(target)
    target=torch.tensor(np.stack(semantic),device=device)
    logits=F.interpolate(model(pixel_values=image).logits,labels.shape[-2:],mode='bilinear',align_corners=False)
    weights=torch.tensor([.08,1.,3.,5.],device=device)
    ce=F.cross_entropy(logits,target,weight=weights)
    prob=logits.softmax(1);onehot=F.one_hot(target,4).permute(0,3,1,2)
    inter=(prob*onehot).sum((0,2,3));den=(prob+onehot).sum((0,2,3))
    return ce+(1-(2*inter[1:]+1)/(den[1:]+1)).mean()


def train(args,manifest,records,device):
    torch.manual_seed(SEED);random.seed(SEED);np.random.seed(SEED)
    run=args.folder/args.model
    if (run/'best.pt').exists() or (run/'training.json').exists():
        raise ValueError('Training output already exists; use a fresh experiment directory')
    seen_body_class=bool(manifest.get('seenBodyClass'))
    geometry_input = getattr(args, 'geometry_input', False)
    model=model_for(args.model,str(args.checkpoint),device,args.initialization,seen_body_class,
                    geometry_input=geometry_input)
    initial_hash=state_digest(model)
    backbone=[];head=[]
    for name,p in model.named_parameters():
        (backbone if name.startswith(('model.pixel_level_module.encoder.','segformer.')) else head).append(p)
    backbone_lr=2e-4 if args.initialization=='random' else 2e-5
    optimizer=torch.optim.AdamW([{'params':backbone,'lr':backbone_lr},{'params':head,'lr':2e-4}],weight_decay=.01)
    training=[e for e in records if e['record']['group']=='weak_train']
    human=[e for e in records if e['record']['group']=='human_train']
    if seen_body_class:
        for sample in training + human:
            sample['classRoles'] = seen_target_classes(sample['record']['letters'], sample['roles'])
    human_probability = .1 if human else 0.
    validation=[e for e in records if e['record']['group']==manifest['tuningGroup']]
    assert not ({e['record']['page'] for e in training+human} & {e['record']['page'] for e in validation})
    run.mkdir(exist_ok=True)
    (run/'code').mkdir(exist_ok=True)
    for filename in ('letter_retest.py','letter_retest_data.py','letter_bases.py'):
        shutil.copyfile(Path(__file__).with_name(filename),run/'code'/filename)
    if geometry_input:
        shutil.copyfile(Path(__file__).with_name('letter_ink_geometry.py'), run/'code'/'letter_ink_geometry.py')
    checkpoint_hash=(hashlib.sha256((args.checkpoint/'model.safetensors').read_bytes()).hexdigest()
                     if args.initialization=='pretrained' else None)
    configuration_hash=hashlib.sha256((args.checkpoint/'config.json').read_bytes()).hexdigest()
    initialization=dict(mode=args.initialization,seed=SEED,initialStateSha256=initial_hash,
                        pretrainedWeightsSha256=checkpoint_hash,configSha256=configuration_hash,
                        backboneLearningRate=backbone_lr,headLearningRate=2e-4,optimizerReset=True)
    (run/'initialization.json').write_text(json.dumps(initialization,indent=2)+'\n')
    minimum_area = getattr(args, 'decoder', None) or manifest.get('decoder') or 3
    canvas_shape = getattr(args, 'canvas_shape', CANVAS)
    thresholds = getattr(args, 'thresholds', (.35, .5, .65))
    grouping_weight = getattr(args, 'grouping_weight', 0)
    # A frozen coverage schedule guarantees every selected automatic example
    # is actually sampled, including rare templates. Legacy runs retain their
    # original random-with-replacement sampling recipe.
    coverage=manifest.get('sampling',{}).get('coverEveryTrainingExample',False)
    schedule=training.copy() if coverage else []
    rng=random.Random(SEED)
    if coverage:rng.shuffle(schedule)
    cursor=0;sample_counts=defaultdict(int)
    best=-1;history=[];start=time.monotonic();window=[]
    initial=[g['lr'] for g in optimizer.param_groups]
    for step in range(1,args.steps+1):
        model.train()
        batch=[]
        for _ in range(args.batch):
            if human_probability and rng.random()<human_probability:
                example=rng.choice(human)
            elif coverage:
                if cursor==len(schedule):rng.shuffle(schedule);cursor=0
                example=schedule[cursor];cursor+=1
            else:example=rng.choice(training)
            batch.append(example);sample_counts[example['record']['id']]+=1
        ratio=min(1,step/50)*(.1+.9*(1+math.cos(math.pi*step/args.steps))/2)
        for group,rate in zip(optimizer.param_groups,initial):group['lr']=rate*ratio
        optimizer.zero_grad(set_to_none=True)
        loss=loss_for(model,args.model,[resize_example(e, canvas_shape) for e in batch],device,rng,grouping_weight)
        if not torch.isfinite(loss):raise RuntimeError(f'Nonfinite loss at {step}')
        loss.backward();torch.nn.utils.clip_grad_norm_(model.parameters(),.1);optimizer.step()
        window.append(float(loss.detach().cpu()))
        if step%20==0 or step==1:
            print(json.dumps(dict(step=step,steps=args.steps,loss=float(np.mean(window)),elapsedSeconds=round(time.monotonic()-start,1))),flush=True);window=[]
        if step%args.validate_every==0 or step==args.steps:
            predictions=predict(model,args.model,validation,device,save_probabilities=True,
                                min_region_area=minimum_area,canvas_shape=canvas_shape)
            selected=None
            for threshold in thresholds:
                rows=[]
                for e,p in zip(validation,predictions):
                    if args.model=='mask2former':pred,roles,_=mask_decode(p['probabilities'],p['classes'],e['labels']>0,threshold,
                        **decoder_options(minimum_area,e['record']))
                    else:pred,roles,_=seg_decode(p['probabilities'],e['labels']>0,threshold)
                    rows.append(metric(e['labels'],pred,e['roles'],roles))
                totals=summarize(rows);score=totals['instanceF1AtIoU50']+.1*totals['meanIoUPerReference']
                if selected is None or score>selected[0]:selected=(score,threshold,totals)
            score,threshold,totals=selected
            history.append(dict(step=step,threshold=threshold,totals=totals,elapsedSeconds=time.monotonic()-start))
            print(json.dumps(dict(validationStep=step,threshold=threshold,instanceF1=totals['instanceF1AtIoU50'],meanIoU=totals['meanIoUPerReference'])),flush=True)
            covered=not coverage or all(sample_counts[e['record']['id']] for e in training)
            if score>best and covered:
                best=score
                torch.save({k:v.detach().cpu() for k,v in model.state_dict().items()},run/'best.pt')
                settings=dict(model=args.model,checkpoint=str(args.checkpoint),step=step,threshold=threshold,
                    initialization=args.initialization,initialStateSha256=initial_hash,
                    pretrainedWeightsSha256=checkpoint_hash,configSha256=configuration_hash,
                    backboneLearningRate=backbone_lr,headLearningRate=2e-4,
                    canvas=list(canvas_shape),seed=SEED,encoderFrozen=False,trainSteps=args.steps,batchSize=args.batch,
                    humanSampleProbability=human_probability,transcriptAtInference=False,target=manifest['target'],
                    minimumRegionAreaPixels=manifest.get('decoder',{}).get('minimumRegionAreaPixels',3),
                    decoder=minimum_area if isinstance(minimum_area, dict) else {},
                    allSelectedTrainingExamplesSeen=covered, groupingWeight=grouping_weight,
                    geometryInput=geometry_input,
                    selectionGroup=manifest['tuningGroup'],selectionTotals=totals,seenBodyClass=seen_body_class,
                    datasetSha256=hashlib.sha256((args.folder/'dataset.json').read_bytes()).hexdigest())
                (run/'settings.json').write_text(json.dumps(settings,indent=2)+'\n')
                if coverage:
                    (run/'sample-counts-at-best.json').write_text(json.dumps(dict(sorted(sample_counts.items())),indent=2)+'\n')
            (run/'history.json').write_text(json.dumps(history,indent=2)+'\n')
            model.train()
    (run/'training.json').write_text(json.dumps(dict(elapsedSeconds=time.monotonic()-start,device=str(device),
        steps=args.steps,batch=args.batch,torchVersion=torch.__version__,trainableParameters=sum(p.numel() for p in model.parameters() if p.requires_grad)),indent=2)+'\n')
    (run/'sample-counts.json').write_text(json.dumps(dict(sorted(sample_counts.items())),indent=2)+'\n')
    if coverage and any(not sample_counts[e['record']['id']] for e in training):
        raise ValueError('Training budget did not cover every selected automatic example')


def evaluate(args,manifest,records,device):
    run=args.folder/args.model;settings=json.loads((run/'settings.json').read_text())
    if settings['datasetSha256']!=hashlib.sha256((args.folder/'dataset.json').read_bytes()).hexdigest():
        raise ValueError('Training dataset changed')
    model=model_for(args.model,settings['checkpoint'],device,settings.get('initialization','pretrained'),
                    settings.get('seenBodyClass',False), geometry_input=settings.get('geometryInput', False))
    model.load_state_dict(torch.load(run/'best.pt',map_location='cpu',weights_only=True))
    # The reviewed set is never used to select a step, threshold or training round.
    evaluation=[e for e in records if e['record']['split']!='train']
    start=time.monotonic();predictions=predict(model,args.model,evaluation,device,settings['threshold'],
        min_region_area=settings.get('decoder') or settings.get('minimumRegionAreaPixels',3),
        canvas_shape=settings.get('canvas'))
    grouped=defaultdict(list);raw_grouped=defaultdict(list);rows=[]
    (run/'predictions').mkdir(exist_ok=True)
    for e,p in zip(evaluation,predictions):
        grouped[p['group']].append(p['metrics']);raw_grouped[p['group']].append(p['rawMetrics'])
        file=f"predictions/{p['id'].replace(':','__')}.npz"
        np.savez_compressed(run/file,instances=p['prediction'],roles=p['roles'],raw=p['raw'])
        rows.append(dict(id=p['id'],group=p['group'],file=file,metrics=p['metrics'],rawMetrics=p['rawMetrics']))
    caveat = 'Automatic targets measure teacher agreement.'
    if 'human_validation' in grouped:
        caveat += ' Six reviewed validation words are small and previously inspected; not a fresh blind test.'
    report=dict(settings=settings,groups={g:summarize(r) for g,r in grouped.items()},
                rawGroups={g:summarize(r) for g,r in raw_grouped.items()},examples=rows,
                inferenceSeconds=time.monotonic()-start,
        caveat=caveat)
    (run/'evaluation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report['groups'],indent=2),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['train','evaluate','smoke'])
    parser.add_argument('--folder',type=Path,required=True)
    parser.add_argument('--model',choices=['mask2former','segformer'],required=True)
    parser.add_argument('--checkpoint',type=Path,help='Local weights/configuration; random initialization reads configuration only')
    parser.add_argument('--initialization',choices=['pretrained','random'],default='pretrained')
    parser.add_argument('--device',default='mps')
    parser.add_argument('--steps',type=int,default=1800)
    parser.add_argument('--batch',type=int,default=4)
    parser.add_argument('--validate-every',type=int,default=300)
    args=parser.parse_args();torch.set_num_threads(4)
    device=torch.device(args.device)
    manifest,records=load_dataset(args.folder, for_training=args.action in ('train', 'smoke'))
    if args.action=='smoke':
        torch.manual_seed(SEED);m=model_for(args.model,str(args.checkpoint),device,args.initialization);m.train()
        examples = [e for e in records if e['record']['group']=='human_train']
        if not examples:
            examples = [e for e in records if e['record']['group']=='weak_train']
        batch = examples[:args.batch]
        t=time.monotonic();loss=loss_for(m,args.model,batch,device,random.Random(SEED));loss.backward()
        if device.type=='mps':torch.mps.synchronize()
        print(dict(loss=float(loss.detach().cpu()),stepSeconds=time.monotonic()-t,parameters=sum(p.numel() for p in m.parameters())),flush=True)
    elif args.action=='train':train(args,manifest,records,device)
    else:evaluate(args,manifest,records,device)


if __name__=='__main__':main()
