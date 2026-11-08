"""Train the two-role model and select a focused replay on validation pages."""
from collections import Counter, defaultdict
from pathlib import Path
from types import SimpleNamespace
import math
import random

from workspace import read, run_path

import numpy as np
import torch

from comparison import cached_example, checkpoint, previous_reference
from data import connected_rows, document, generation_for, identifier, load_page, open_audit
from letter_bases import check_training_records
from letter_corpus_audit import batch_probabilities, prepare_example, write_json
from letter_retest import CANVAS, SEED, decoder_options, load_example, loss_for, mask_decode, metric, model_for, summarize, train
from letter_template_repeat import complete_row, sha
from seen import layout, measure, summarize as summarize_seen


def examples(folder, records):
    return [load_example(folder, record) for record in records]


def focus_validation(folder, run, validation, excluded, split='validation'):
    """Use all comparable multi-letter seen and teeth on the chosen held-out pages."""
    selected = {ex['record']['id']: ex for ex in validation}
    snapshot = Path(run['snapshot'])
    generation = generation_for(snapshot)
    reference = previous_reference(run)
    previous = open_audit(reference) if reference else None
    try:
        for page in read(folder / 'inputs' / 'corpus.json')['pages']:
            if page['split'] != split:
                continue
            pid = page['id']
            cache = load_page(snapshot, generation, int(pid[4:]))
            source = None
            for row in connected_rows(cache):
                letters = row['letters']
                sid = identifier(pid, row)
                if not any(layout(letters)) or not complete_row(row, excluded, sid):
                    continue
                if source is None:
                    source = document(pid, cache, page)
                item = cached_example(previous, row, source) if previous else None
                if item is None:
                    item = prepare_example(row, source)
                if not item['status']['valid']:
                    continue
                sample = dict(item['example'], labels=item['reference'], roles=item['roles'])
                general = selected.pop(sid, None)
                if general is not None and not np.array_equal(general['labels'] > 0, item['ink']):
                    raise ValueError(f'Validation source crop changed: {sid}')
                yield dict(example=sample, ink=item['ink'], general=general, letters=letters)
        for sample in selected.values():
            yield dict(example=sample, ink=sample['labels'] > 0, general=sample, letters=None)
    finally:
        if previous:
            previous.close()


def decoded(model, items, settings, device):
    canvas_shape = tuple(settings.get('canvas', CANVAS))
    batch_size = 8 if canvas_shape != CANVAS else 32
    for start in range(0, len(items), batch_size):
        batch = items[start:start + batch_size]
        probabilities = batch_probabilities(model, batch, device, settings['threshold'], canvas_shape)
        for item, (masks, classes) in zip(batch, probabilities, strict=True):
            yield item, mask_decode(masks, classes, item['ink'], settings['threshold'],
                **decoder_options(settings.get('decoder') or 3, item['example']['record']))


@torch.inference_mode()
def validate(model, items, settings, device):
    model.eval()
    general, focused = [], []
    for item, (pred, roles, _) in decoded(model, items, settings, device):
        if target := item['general']:
            general.append(metric(target['labels'], pred, target['roles'], roles))
        if item['letters']:
            focused.append(measure(item['letters'], item['example']['labels'], pred, roles))
    return dict(general=summarize(general), focus=summarize_seen(focused))


def draws(pool, rng):
    while True:
        order = pool.copy()
        rng.shuffle(order)
        yield from order


def focused_replay(folder, model, fitting, validation, settings, steps, device):
    seen = [e for e in fitting if e['record'].get('trainingFocus', '').startswith('seen-')]
    teeth = [e for e in fitting if e['record'].get('trainingFocus', '').startswith('separate-')]
    general = [e for e in fitting if not e['record'].get('trainingFocus')]
    if not all((seen, teeth, general)):
        print('Focused replay requires accepted seen, separate teeth and general training pools.', flush=True)
        return None
    model.eval()
    origin = validate(model, validation, settings, device)
    base = origin['focus']
    if not base['seenCrops'] or not base['tripleRuns']:
        print('Focused replay requires both seen and genuine tooth cases on validation pages.', flush=True)
        return None
    f1 = origin['general']['roleAwareInstanceF1AtIoU50']
    encoder, head = [], []
    for name, param in model.named_parameters():
        (encoder if name.startswith('model.pixel_level_module.encoder.') else head).append(param)
    rates = (5e-6, 5e-5)
    optimizer = torch.optim.AdamW([dict(params=params, lr=rate)
                                  for params, rate in zip((encoder, head), rates)], weight_decay=.01)
    rng = random.Random(SEED)
    torch.manual_seed(SEED)
    seen_draws, tooth_draws, general_draws = (draws(pool, rng) for pool in (seen, teeth, general))
    checks = set(range(120, steps + 1, 120)) | {steps}
    counts = Counter()
    history = [dict(origin, step=0, eligible=True)]
    best = None

    def rank(result):
        focus = result['focus']
        return (focus['fragmentedSeenCrops'], focus['straySeenCrops'], focus['mergedTriples'],
                -result['general']['roleAwareInstanceF1AtIoU50'])

    key = rank(origin)
    weights = folder / 'mask2former' / 'replay.pt'
    for step in range(1, steps + 1):
        model.train()
        batch = [next(seen_draws), next(tooth_draws), next(general_draws), next(general_draws)]
        counts.update(e['record']['id'] for e in batch)
        factor = min(1, step / 20) * (.15 + .85 * (1 + math.cos(math.pi * step / steps)) / 2)
        for group, rate in zip(optimizer.param_groups, rates):
            group['lr'] = factor * rate
        optimizer.zero_grad(set_to_none=True)
        loss = loss_for(model, 'mask2former', batch, device, rng)
        if not torch.isfinite(loss):
            raise ValueError(f'Nonfinite replay loss at step {step}')
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), .1)
        optimizer.step()
        if step not in checks:
            continue
        result = validate(model, validation, settings, device)
        focus = result['focus']
        eligible = (result['general']['roleAwareInstanceF1AtIoU50'] >= f1 - .005
                    and focus['separateTriples'] >= base['separateTriples']
                    and focus['mergedTriples'] <= base['mergedTriples']
                    and focus['seenMatched'] >= base['seenMatched'])
        history.append(dict(result, step=step, eligible=eligible))
        print(f"Replay {step}. Seen splits {focus['fragmentedSeenCrops']}, separate tooth runs "
              f"{focus['separateTriples']}, validation guard passed {eligible}.", flush=True)
        if eligible and rank(result) < key:
            key, best = rank(result), step
            temporary = weights.with_suffix('.tmp')
            torch.save({name: tensor.detach().cpu() for name, tensor in model.state_dict().items()}, temporary)
            temporary.replace(weights)
            write_json(folder / 'mask2former' / 'replay-draws-at-best.json', dict(counts))
        write_json(folder / 'mask2former' / 'replay-history.json', history)
    write_json(folder / 'mask2former' / 'replay-recipe.json',
               dict(steps=steps, seed=SEED, selectedStep=best, threshold=settings['threshold'],
                    batchComposition=dict(seen=1, teeth=1, general=2),
                    pools=dict(seen=len(seen), teeth=len(teeth), general=len(general)),
                    learningRates=dict(encoder=rates[0], head=rates[1]),
                    validationGuards=dict(f1Floor=f1 - .005, separateTriples=base['separateTriples'],
                                          maxMergedTriples=base['mergedTriples'], seenMatched=base['seenMatched'])))
    return best


def fit(folder, device_name, steps, validate_every, replay_steps, *, canvas_shape=CANVAS, decoder=None,
        thresholds=(.35, .5, .65), grouping_weight=0, geometry_input=False):
    run = read(folder / 'run.json')
    manifest = read(folder / 'dataset.json')
    if sha(folder / 'dataset.json') != run['datasetSha256']:
        raise ValueError('The frozen dataset changed')
    if sha(folder / 'inputs' / 'corpus.json') != run['corpusSha256']:
        raise ValueError('The frozen page split changed')
    for name, digest in run['pretrainedFiles'].items():
        if sha(folder / 'pretrained' / name) != digest:
            raise ValueError(f'The frozen pretrained file changed: {name}')
    pages = {p['id']: p['split'] for p in read(folder / 'inputs' / 'corpus.json')['pages']}
    records = manifest['records']
    excluded = set(manifest.get('excludedReviewedIds', [])) | set(manifest.get('excludedCropIds', []))
    for record in records:
        if (pages.get(record['page']) != record['split'] or record['provenance'] != 'machine-weak'
                or record['bodyRegions'] < 2 or record['group'] != 'weak_' + record['split']
                or record['id'] in excluded):
            raise ValueError(f"Invalid connected crop or split: {record['id']}")
    check_training_records(records)
    fit_records = [r for r in records if r['split'] == 'train']
    validation = [r for r in records if r['split'] == 'validation']
    if steps <= 0 or validate_every <= 0 or replay_steps < 0 or steps * 4 < len(fit_records):
        raise ValueError('Use positive training steps and enough updates to sample every fitting crop')
    torch.set_num_threads(4)
    device = torch.device(device_name)
    if device.type == 'mps' and not torch.backends.mps.is_available():
        raise ValueError('MPS is unavailable in this process. Select CPU or run from VS Code on macOS.')
    fitting = examples(folder, fit_records)
    tuning = examples(folder, validation)
    args = SimpleNamespace(folder=folder, model='mask2former', checkpoint=folder / 'pretrained',
                           initialization='pretrained', steps=steps, batch=4, validate_every=validate_every,
                           canvas_shape=canvas_shape, decoder=decoder, thresholds=thresholds,
                           grouping_weight=grouping_weight, geometry_input=geometry_input)
    train(args, manifest, fitting + tuning, device)
    model_path = folder / 'mask2former'
    settings = read(model_path / 'settings.json')
    selected = dict(weights='best.pt', settings=settings, stage='base')
    if replay_steps:
        model = model_for('mask2former', str(folder / 'pretrained'), device,
                          seen_body_class=settings.get('seenBodyClass', False),
                          geometry_input=settings.get('geometryInput', False))
        model.load_state_dict(torch.load(model_path / 'best.pt', map_location='cpu', weights_only=True))
        tuning_focus = list(focus_validation(folder, run, tuning, excluded))
        best = focused_replay(folder, model, fitting, tuning_focus, settings, replay_steps, device)
        if best is not None:
            selected = dict(weights='replay.pt', settings=dict(settings, step=best),
                            stage='focused-replay', parentStep=settings['step'])
    write_json(model_path / 'selected.json', selected)
    print(f"Selected {selected['stage']} checkpoint.", flush=True)


@torch.inference_mode()
def evaluate(folder, device_name):
    manifest = read(folder / 'dataset.json')
    weights, settings = checkpoint(folder)
    if settings['datasetSha256'] != sha(folder / 'dataset.json'):
        raise ValueError('The frozen dataset changed')
    torch.set_num_threads(4)
    device = torch.device(device_name)
    model = model_for('mask2former', str(folder / 'pretrained'), device,
                      seen_body_class=settings.get('seenBodyClass', False),
                      geometry_input=settings.get('geometryInput', False))
    model.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
    model.eval()
    samples = examples(folder, [r for r in manifest['records'] if r['split'] != 'train'])
    items = [dict(example=e, ink=e['labels'] > 0) for e in samples]
    groups = defaultdict(list)
    counts = defaultdict(Counter)
    for item, (pred, roles, raw) in decoded(model, items, settings, device):
        sample = item['example']
        split = sample['record']['split']
        expected = sample['roles']
        for mode, result in (('completed', pred), ('raw', raw)):
            groups[split, mode].append(metric(sample['labels'], result, expected, roles))
        predicted = roles[np.unique(pred)]
        counts[split]['wrongBodyCount'] += int((predicted == 1).sum() != (expected == 1).sum())
        counts[split]['wrongKashidaCount'] += int((predicted == 2).sum() != (expected == 2).sum())
    report = dict(checkpointSha256=sha(weights), threshold=settings['threshold'],
                  reference='Automatic cutter agreement on held-out connected subwords',
                  groups={f'{s}/{mode}': summarize(rows) for (s, mode), rows in groups.items()},
                  countErrors={s: dict(group) for s, group in counts.items()})
    write_json(folder / 'evaluation.json', report)
    for split in ('validation', 'test'):
        result = report['groups'].get(f'{split}/completed')
        if result:
            print(f"{split} {result['examples']} crops, role-aware F1 {result['roleAwareInstanceF1AtIoU50']:.2%}.",
                  flush=True)


@torch.inference_mode()
def compare_heldout(folder, device_name, geometry_cohort=None):
    """Score both frozen checkpoints against the same current reference masks."""
    run = read(folder / 'run.json')
    if geometry_cohort is None:
        geometry_cohort = checkpoint(folder)[1].get('geometryInput', False)
    previous = run_path(run['previous']) if run['previous'] else None
    if previous is None:
        raise ValueError('Paired evaluation requires a previous run')
    dataset = folder / 'dataset.json'
    if sha(dataset) != run['datasetSha256']:
        raise ValueError('The frozen dataset changed')
    manifest = read(dataset)
    excluded = set(manifest['excludedReviewedIds']) | set(manifest['excludedCropIds'])
    device = torch.device(device_name)
    torch.set_num_threads(4)
    diagnostics = {}
    diagnostic_items = []
    if added := manifest.get('addedTestFocusIds'):
        snapshot = Path(run['snapshot'])
        generation = generation_for(snapshot)
        pages = {p['id']: p for p in read(folder / 'inputs' / 'corpus.json')['pages']}
        for page in manifest['promotedTestPages']:
            cache = load_page(snapshot, generation, int(page[4:]))
            source = document(page, cache, pages[page])
            for row in connected_rows(cache):
                if identifier(page, row) in added:
                    diagnostic_items.append(prepare_example(row, source))
        if len(diagnostic_items) != len(added) or any(not item['status']['valid'] for item in diagnostic_items):
            raise ValueError('Cannot prepare the added crops for a separate training diagnostic')
    results = {}
    for split in ('validation', 'test'):
        records = [r for r in manifest['records'] if r['split'] == split]
        heldout = examples(folder, records)
        items = list(focus_validation(folder, run, heldout, excluded, split))
        if geometry_cohort:
            from tooth_geometry import observations
            from tooth_replay import evaluate as evaluate_geometry

            coverage = Counter()
            low_rows = [row for item in items if item['letters']
                        for row in observations(item['example'], item['letters'], coverage)]
        report = {}
        for name, origin in (('previous', previous), ('trial', folder)):
            weights, settings = checkpoint(origin)
            model = model_for('mask2former', str(origin / 'pretrained'), device,
                              seen_body_class=settings.get('seenBodyClass', False),
                              geometry_input=settings.get('geometryInput', False))
            model.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
            metrics = (evaluate_geometry(model, items, settings, device, low_rows) if geometry_cohort
                       else validate(model, items, settings, device))
            report[name] = dict(metrics,
                                checkpointSha256=sha(weights), threshold=settings['threshold'],
                                canvas=settings.get('canvas', list(CANVAS)))
            if split == 'test' and diagnostic_items:
                diagnostics[name] = {}
                for item, (pred, roles, _) in decoded(model, diagnostic_items, settings, device):
                    sid = item['example']['record']['id']
                    diagnostics[name][sid] = measure(item['row']['letters'], item['reference'], pred, roles)
            del model
        report['frozenSampleIds'] = [r['id'] for r in records]
        results[split] = report
        for name in ('previous', 'trial'):
            general, focus = (report[name][key] for key in ('general', 'focus'))
            print(f"{split} {name}: F1 {general['roleAwareInstanceF1AtIoU50']:.2%}, "
                  f"split seen {focus['fragmentedSeenCrops']}/{focus['seenCrops']}, "
                  f"merged genuine triples {focus['mergedTriples']}/{focus['tripleRuns']}.", flush=True)
    paired = dict(reference='Same current automatic cutter masks',
                  datasetSha256=run['datasetSha256'], previous=str(previous), groups=results)
    if diagnostics:
        paired['addedTrainingDiagnostics'] = diagnostics
    write_json(folder / 'paired-heldout.json', paired)
