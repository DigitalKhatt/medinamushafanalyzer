"""Continue a frozen segmentation model with balanced, low-rate training."""
from collections import Counter
import math
import random
import shutil
import time

import numpy as np
import torch

from comparison import checkpoint
from letter_corpus_audit import batch_probabilities, write_json
from letter_retest import (
    SEED, decoder_options, loss_for, mask_decode, metric, model_for,
    seen_target_classes, summarize,
)
from letter_bases import check_training_records
from letter_template_repeat import sha
from seen import measure, summarize as summarize_seen
from train import draws, examples, focus_validation
from workspace import read


@torch.inference_mode()
def calibrate(model, items, settings, device, thresholds=None, owned_fractions=None):
    model.eval()
    thresholds = sorted(set(thresholds or (.35, .5, .65)) | {settings['threshold']})
    current_fraction = decoder_options(settings.get('decoder') or 3)['minimum_owned_fraction']
    fractions = sorted(set(owned_fractions or (current_fraction,)) | {current_fraction})
    results = {(value, fraction): dict(general=[], focus=[]) for value in thresholds for fraction in fractions}
    for start in range(0, len(items), 32):
        batch = items[start:start + 32]
        probabilities = batch_probabilities(model, batch, device, min(thresholds), settings.get('canvas'))
        for item, (masks, classes) in zip(batch, probabilities, strict=True):
            options = decoder_options(settings.get('decoder') or 3, item['example']['record'])
            for (threshold, fraction), rows in results.items():
                prediction, roles, _ = mask_decode(masks, classes, item['ink'], threshold,
                    **dict(options, minimum_owned_fraction=fraction))
                if target := item['general']:
                    rows['general'].append(metric(target['labels'], prediction, target['roles'], roles))
                if item['letters']:
                    rows['focus'].append(measure(item['letters'], item['example']['labels'], prediction, roles))
    return [dict(threshold=value, minimumOwnedFraction=fraction, general=summarize(rows['general']),
                 focus=summarize_seen(rows['focus'])) for (value, fraction), rows in results.items()]


def eligible_result(row, baseline):
    focus, base = row['focus'], baseline['focus']
    return (row['general']['roleAwareInstanceF1AtIoU50'] >=
            baseline['general']['roleAwareInstanceF1AtIoU50'] - .001
            and focus['separateTriples'] >= base['separateTriples']
            and focus['mergedTriples'] <= base['mergedTriples']
            and focus['seenMatched'] >= base['seenMatched'])


def result_rank(row):
    focus = row['focus']
    return (focus['fragmentedSeenCrops'], focus['straySeenCrops'], -focus['seenMatched'],
            -row['general']['roleAwareInstanceF1AtIoU50'])


def refine(folder, previous, device_name, steps=2400, validate_every=400):
    if folder.exists():
        raise ValueError(f'Choose a fresh refinement directory: {folder}')
    manifest = read(previous / 'dataset.json')
    run = read(previous / 'run.json')
    weights, settings = checkpoint(previous)
    if sha(previous / 'dataset.json') != run['datasetSha256'] or settings['datasetSha256'] != run['datasetSha256']:
        raise ValueError('The parent dataset changed')
    if sha(previous / 'inputs' / 'corpus.json') != run['corpusSha256']:
        raise ValueError('The parent page split changed')
    for name, digest in run['pretrainedFiles'].items():
        if sha(previous / 'pretrained' / name) != digest:
            raise ValueError(f'The frozen pretrained file changed: {name}')
    pages = {p['id']: p['split'] for p in read(previous / 'inputs' / 'corpus.json')['pages']}
    excluded = set(manifest.get('excludedReviewedIds', [])) | set(manifest.get('excludedCropIds', []))
    for record in manifest['records']:
        if (pages.get(record['page']) != record['split'] or record['provenance'] != 'machine-weak'
                or record['bodyRegions'] < 2 or record['group'] != 'weak_' + record['split']
                or record['id'] in excluded):
            raise ValueError(f"Invalid connected crop or split: {record['id']}")
    check_training_records(manifest['records'])
    fitting_records = [r for r in manifest['records'] if r['split'] == 'train']
    validation_records = [r for r in manifest['records'] if r['split'] == 'validation']
    if steps <= 0 or validate_every <= 0 or steps * 4 < len(fitting_records):
        raise ValueError('Allow enough positive steps to visit every fitting crop')
    fitting_pages = {r['page'] for r in fitting_records}
    if fitting_pages.intersection(r['page'] for r in manifest['records'] if r['split'] != 'train'):
        raise ValueError('Fitting and held-out pages overlap')
    folder.mkdir(parents=True)
    for name in ('masks', 'inputs', 'pretrained'):
        shutil.copytree(previous / name, folder / name)
    shutil.copyfile(previous / 'dataset.json', folder / 'dataset.json')
    run.update(previous=str(previous), refinementParent=str(previous), refinementParentSha256=sha(weights))
    write_json(folder / 'run.json', run)
    destination = folder / 'mask2former'
    destination.mkdir()
    shutil.copyfile(weights, destination / 'best.pt')
    selected_settings = dict(settings, checkpoint=str(folder / 'pretrained'))
    print(f'Continuing {weights} with {len(fitting_records)} training crops.', flush=True)

    def publish(step, threshold, result):
        saved = dict(selected_settings, step=step, threshold=threshold, selectionTotals=result['general'],
                     selectionGroup='weak_validation', refinementParentStep=settings['step'],
                     refinement=dict(parent=str(previous), parentSha256=sha(weights), steps=step,
                                     plannedSteps=steps, backboneLearningRate=2e-6, headLearningRate=2e-5))
        write_json(destination / 'settings.json', saved)
        write_json(destination / 'selected.json', dict(weights='best.pt', settings=saved,
                   stage='balanced-refinement' if step else 'parent', parentStep=settings['step']))

    torch.set_num_threads(4)
    torch.manual_seed(SEED)
    np.random.seed(SEED)
    rng = random.Random(SEED)
    device = torch.device(device_name)
    model = model_for('mask2former', str(folder / 'pretrained'), device,
                      seen_body_class=manifest.get('seenBodyClass', False),
                      geometry_input=settings.get('geometryInput', False))
    model.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
    fitting = examples(folder, fitting_records)
    if manifest.get('seenBodyClass'):
        for sample in fitting:
            sample['classRoles'] = seen_target_classes(sample['record']['letters'], sample['roles'])
    items = list(focus_validation(folder, run, examples(folder, validation_records), excluded))
    initial = calibrate(model, items, settings, device)
    baseline = next(row for row in initial if row['threshold'] == settings['threshold'])

    publish(0, settings['threshold'], baseline)
    best = result_rank(baseline)
    history = []
    counts = Counter()

    def check(step, candidates):
        nonlocal best
        for result in candidates:
            allowed = eligible_result(result, baseline)
            history.append(dict(result, step=step, eligible=allowed))
            if allowed and result_rank(result) < best:
                best = result_rank(result)
                temporary = destination / 'best.tmp'
                torch.save({k: v.detach().cpu() for k, v in model.state_dict().items()}, temporary)
                temporary.replace(destination / 'best.pt')
                publish(step, result['threshold'], result)
                write_json(destination / 'sample-counts-at-best.json', dict(counts))
            focus = result['focus']
            print(f"Refine {step}, threshold {result['threshold']}: "
                  f"F1 {result['general']['roleAwareInstanceF1AtIoU50']:.2%}, "
                  f"seen splits {focus['fragmentedSeenCrops']}, "
                  f"separate triples {focus['separateTriples']}, accepted {allowed}.", flush=True)
        write_json(destination / 'refinement-history.json', history)

    check(0, initial)
    encoder, head = [], []
    for name, parameter in model.named_parameters():
        (encoder if name.startswith('model.pixel_level_module.encoder.') else head).append(parameter)
    rates = (2e-6, 2e-5)
    optimizer = torch.optim.AdamW([dict(params=parameters, lr=rate)
                                  for parameters, rate in zip((encoder, head), rates)], weight_decay=.01)
    stream = draws(fitting, rng)
    started = time.monotonic()
    for step in range(1, steps + 1):
        model.train()
        batch = [next(stream) for _ in range(4)]
        counts.update(sample['record']['id'] for sample in batch)
        factor = min(1, step / 40) * (.15 + .85 * (1 + math.cos(math.pi * step / steps)) / 2)
        for group, rate in zip(optimizer.param_groups, rates):
            group['lr'] = rate * factor
        optimizer.zero_grad(set_to_none=True)
        loss = loss_for(model, 'mask2former', batch, device, rng)
        if not torch.isfinite(loss):
            raise ValueError(f'Nonfinite refinement loss at step {step}')
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), .1)
        optimizer.step()
        if step % 100 == 0:
            print(f"Balanced training {step}/{steps}, {time.monotonic() - started:.0f}s.", flush=True)
        if step % validate_every == 0 or step == steps:
            check(step, calibrate(model, items, settings, device))
    if set(counts) != {record['id'] for record in fitting_records}:
        raise ValueError('Some fitting crops were never visited')
    write_json(destination / 'sample-counts.json', dict(counts))
    write_json(destination / 'refinement-recipe.json', dict(
        steps=steps, validateEvery=validate_every, seed=SEED, batch=4, device=device_name,
        sourceCheckpointSha256=sha(weights), optimizerReset=True,
        learningRates=dict(encoder=rates[0], head=rates[1]), thresholds=[.35, .5, .65],
        sampling='Shuffled complete passes through every training crop',
        validationF1Tolerance=.001, elapsedSeconds=time.monotonic() - started,
    ))
