"""Compare height-matched connector examples with ordinary senna/seen replay."""
import argparse
from collections import Counter, defaultdict
from pathlib import Path
import math
import random
import time

from workspace import read, run_path

import numpy as np
from scipy.optimize import linear_sum_assignment
import torch

from comparison import checkpoint
from letter_corpus_audit import write_json
from letter_retest import CANVAS, SEED, loss_for, metric, model_for, seen_target_classes, summarize
from letter_template_repeat import sha
from seen import layout, measure, summarize as summarize_seen
from tooth_geometry import observations
from train import decoded, draws, examples, focus_validation


def pairs_for(rows):
    pools, low = {}, {}
    for kind in ('seen', 'senna'):
        ranked = sorted((r for r in rows if r['kind'] == kind), key=lambda r: (r['highTooth'], r['id']))
        unique = {}
        for row in ranked:
            unique.setdefault(row['id'], row)
        pools[kind] = list(unique)
        low[kind] = [r for r in unique.values() if r['highTooth'] <= 1]
    seen, senna = low['seen'], low['senna']
    if not seen or not senna:
        raise ValueError('Both low-tooth classes need measurable training examples')
    costs = np.abs(np.subtract.outer([r['highTooth'] for r in seen], [r['highTooth'] for r in senna]))
    a, b = linear_sum_assignment(costs)
    targeted = [(seen[i]['id'], senna[j]['id']) for i, j in zip(a, b, strict=True)]
    rng = random.Random(SEED)
    control = list(zip(rng.sample(pools['seen'], len(targeted)), rng.sample(pools['senna'], len(targeted)), strict=True))
    return dict(control=control, geometry=targeted)


@torch.inference_mode()
def evaluate(model, items, settings, device, low_rows):
    targets = defaultdict(set)
    for row in low_rows:
        if row['highTooth'] <= 1:
            targets[row['id']].add(tuple(row['letterIndices']))
    model.eval()
    general, focus, low = [], [], []
    for item, (prediction, roles, _) in decoded(model, items, settings, device):
        if sample := item['general']:
            general.append(metric(sample['labels'], prediction, sample['roles'], roles))
        if item['letters']:
            result = measure(item['letters'], item['example']['labels'], prediction, roles)
            focus.append(result)
            ids = targets[item['example']['record']['id']]
            if ids:
                _, triples = layout(item['letters'])
                bodies = [b for b in result['seen'] if (b['index'],) in ids]
                runs = [r for t, r in zip(triples, result['triples'], strict=True) if tuple(t) in ids]
                low.append(dict(seen=bodies, triples=runs,
                                fragments=max((b['fragments'] for b in bodies if b['coverage'] >= .65), default=0)))
    return dict(general=summarize(general), focus=summarize_seen(focus), lowTooth=summarize_seen(low))


def fit(model, fitting, pairs, device, steps):
    encoder, head = [], []
    for name, parameter in model.named_parameters():
        (encoder if name.startswith('model.pixel_level_module.encoder.') else head).append(parameter)
    rates = (2e-6, 2e-5)
    optimizer = torch.optim.AdamW([dict(params=p, lr=lr) for p, lr in zip((encoder, head), rates, strict=True)],
                                  weight_decay=.01)
    rng = random.Random(SEED)
    ordinary = draws(list(fitting.values()), random.Random(SEED))
    focused = draws(pairs, random.Random(SEED))
    counts = Counter()
    started = time.monotonic()
    model.train()
    for step in range(1, steps + 1):
        batch = [next(ordinary) for _ in range(2 if step % 4 == 0 else 4)]
        if step % 4 == 0:
            batch.extend(fitting[id] for id in next(focused))
        counts.update(sample['record']['id'] for sample in batch)
        factor = min(1, step / 20) * (.15 + .85 * (1 + math.cos(math.pi * step / steps)) / 2)
        for group, rate in zip(optimizer.param_groups, rates, strict=True):
            group['lr'] = rate * factor
        optimizer.zero_grad(set_to_none=True)
        loss = loss_for(model, 'mask2former', batch, device, rng)
        if not torch.isfinite(loss):
            raise ValueError(f'Nonfinite loss at replay step {step}')
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), .1)
        optimizer.step()
        if step % 120 == 0:
            print(f'Replay {step}/{steps}, {time.monotonic() - started:.0f}s.', flush=True)
    return dict(draws=counts, elapsedSeconds=time.monotonic() - started)


def run_trial(folder, baseline, audit_path, device_name, steps=480):
    audit = read(audit_path)
    manifest, run = read(baseline / 'dataset.json'), read(baseline / 'run.json')
    weights, settings = checkpoint(baseline)
    dataset_hash = sha(baseline / 'dataset.json')
    if not (audit['datasetSha256'] == run['datasetSha256'] == settings['datasetSha256'] == dataset_hash):
        raise ValueError('Training and the geometry audit require identical frozen crops')
    if sha(baseline / 'inputs/corpus.json') != run['corpusSha256'] or settings['canvas'] != list(CANVAS):
        raise ValueError('Use the frozen page split and low-resolution baseline')
    if steps < 4 or steps % 4:
        raise ValueError('Use a positive multiple of four replay steps')
    pages = {p['id']: p['split'] for p in read(baseline / 'inputs/corpus.json')['pages']}
    excluded = set(manifest['excludedReviewedIds']) | set(manifest['excludedCropIds'])
    records = [r for r in manifest['records'] if r['split'] == 'train']
    if any(pages.get(r['page']) != 'train' or r['id'] in excluded or r['group'] != 'weak_train'
           or r['provenance'] != 'machine-weak' or r['bodyRegions'] < 2 for r in records):
        raise ValueError('Replay examples must come from the automatic training pages')
    pools = pairs_for(audit['groups']['train']['rows'])
    fitting = {e['record']['id']: e for e in examples(baseline, records)}
    if any(id not in fitting for pairs in pools.values() for pair in pairs for id in pair):
        raise ValueError('Geometry replay includes a crop outside the training split')
    if settings.get('seenBodyClass'):
        for sample in fitting.values():
            sample['classRoles'] = seen_target_classes(sample['record']['letters'], sample['roles'])
    folder.mkdir(parents=True)
    write_json(folder / 'recipe.json', dict(
        source=str(baseline), sourceWeightsSha256=sha(weights), datasetSha256=dataset_hash,
        geometryAudit=str(audit_path.resolve()), geometryAuditSha256=sha(audit_path),
        settings=settings, seed=SEED, steps=steps, rates=[2e-6, 2e-5],
        batchSize=4, replayPairEvery=4, pools=pools))
    torch.set_num_threads(4)
    device = torch.device(device_name)
    checkpoints = {'baseline': weights}
    for name, pairs in pools.items():
        print(f'Training {name} with {len(pairs)} pairs.', flush=True)
        torch.manual_seed(SEED)
        model = model_for('mask2former', str(baseline / 'pretrained'), device,
                          seen_body_class=settings.get('seenBodyClass', False),
                          geometry_input=settings.get('geometryInput', False))
        model.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
        statistics = fit(model, fitting, pairs, device, steps)
        path = folder / f'{name}.pt'
        torch.save(model.cpu().state_dict(), path)
        checkpoints[name] = path
        write_json(folder / f'{name}-training.json', statistics)
        del model
    del fitting
    report = {}
    for split in ('validation', 'test'):
        records = [r for r in manifest['records'] if r['split'] == split]
        items = list(focus_validation(baseline, run, examples(baseline, records), excluded, split))
        if split == 'validation':
            low_rows = audit['groups'][split]['rows']
        else:
            coverage = Counter()
            low_rows = [r for item in items if item['letters']
                        for r in observations(item['example'], item['letters'], coverage)]
        report[split] = {}
        for name, path in checkpoints.items():
            model = model_for('mask2former', str(baseline / 'pretrained'), device,
                              seen_body_class=settings.get('seenBodyClass', False),
                              geometry_input=settings.get('geometryInput', False))
            model.load_state_dict(torch.load(path, map_location='cpu', weights_only=True))
            result = evaluate(model, items, settings, device, low_rows)
            report[split][name] = result
            print(f"{split} {name}: F1 {result['general']['roleAwareInstanceF1AtIoU50']:.2%}, "
                  f"seen splits {result['focus']['fragmentedSeenCrops']}, "
                  f"low-tooth triples separated {result['lowTooth']['separateTriples']}/"
                  f"{result['lowTooth']['tripleRuns']}.", flush=True)
            del model
        write_json(folder / 'comparison.json', dict(groups=report, reference='Frozen automatic cutter masks',
                   checkpointSha256={name: sha(path) for name, path in checkpoints.items()}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--audit', type=Path, required=True)
    parser.add_argument('--device', default='mps')
    args = parser.parse_args()
    run_trial(run_path(args.run), run_path(args.baseline), args.audit, args.device)
