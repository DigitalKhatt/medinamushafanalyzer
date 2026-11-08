"""Render Mask2Former instance targets for review or replay a frozen run."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import re
import shutil
import tempfile
from types import SimpleNamespace

import numpy as np
from PIL import Image
import torch


APP = Path(__file__).resolve().parent / 'app'
BODY_COLORS = ['28769f', '984879', '298362', '8862ab', '9e5035', '526e34', '465db8', 'a14f65']
KASHIDA = 'dfac32'


def read(path):
    return json.loads(path.read_bytes())


def digest(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def frozen_trainer(run):
    path = run / 'mask2former/code/letter_retest.py'
    if digest(path) != read(run / 'inputs/code-hashes.json')['letter_retest.py']:
        raise ValueError('The archived trainer differs from the training snapshot')
    spec = importlib.util.spec_from_file_location('frozen_letter_trainer', path)
    trainer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(trainer)
    return trainer


def save_instances(path, labels, roles):
    # Palette indices retain instance IDs. Transparent paper lets the gallery
    # display the identical silhouettes as either black ink or colored targets.
    palette = [(255, 255, 255)]
    bodies = 0
    for role in roles[1:]:
        if role not in (1, 2):
            raise ValueError(f'Unknown training role {role}')
        color = KASHIDA if role == 2 else BODY_COLORS[bodies % len(BODY_COLORS)]
        bodies += int(role == 1)
        palette.append(tuple(bytes.fromhex(color)))
    bits = next(n for n in (1, 2, 4, 8) if len(palette) <= 2**n)
    image = Image.fromarray(labels).convert('P')
    image.putpalette(bytes(c for rgb in palette for c in rgb))
    image.save(path, bits=bits, transparency=0)


def reviewed_display_text(run, records):
    # The reviewed targets contain no transcription. Read captions separately
    # from the page snapshot used at preparation, if it is still available.
    snapshot = read(run / 'inputs/snapshot.json')
    result = {}
    for page in sorted({r['page'] for r in records if r['group'] == 'human_train'}):
        path = Path(snapshot['directory']) / 'pages' / f'{page}.json'
        if path.is_file() and digest(path) == snapshot['pageHashes'][str(int(page[4:]))]:
            for row in read(path)['subwords']:
                key = f"{page}:l{row['line']}-w{row['word']}-s{row['subword']}"
                result[key] = row['text']
    return result


def build_candidates(run, out, previous_gallery=None):
    """Render one unaugmented target per fitting example, before any training."""
    from letter_retest import CANVAS, load_example
    manifest = read(run / 'dataset.json')
    if manifest.get('trainingReview', {}).get('status') != 'open':
        raise ValueError('Prepare the candidate dataset with --review-training')
    splits = {p['id']: p['split'] for p in read(run / 'inputs/corpus.json')['pages']}
    records = [r for r in manifest['records'] if r['group'] in ('weak_train', 'human_train')]
    if len(records) != len({r['id'] for r in records}) or any(
            r['split'] != 'train' or splits[r['page']] != 'train' for r in records):
        raise ValueError('Invalid or repeated training subword')
    captions = reviewed_display_text(run, records)
    previous = read(previous_gallery / 'gallery.json') if previous_gallery else None
    prior_rows = {r['id']: r for r in previous['rows']} if previous else {}
    for name in ('base', 'native'):
        (out / name).mkdir()
    rows = []
    for r in records:
        example = load_example(run, r)
        roles = example['roles']
        native = example['labels']
        canvas = example['canvas'].numpy()
        if len(roles) > 256 or native.max() >= len(roles):
            raise ValueError(f"Invalid instance labels for {r['id']}")
        file = Path(r['file']).stem
        for name, target in [('base', canvas), ('native', native)]:
            save_instances(out / name / (file + '.png'), target.astype('uint8'), roles)
        comparison = None
        if previous:
            old = prior_rows.get(r['id'])
            comparison = 'new'
            if old:
                with np.load(previous_gallery / 'raw' / (old['file'] + '.npz')) as archived:
                    same = np.array_equal(archived['instances'], native) and np.array_equal(archived['roles'], roles)
                comparison = 'same' if same else 'changed'
        rows.append(dict(id=r['id'], file=file, text=r.get('textForDisplayOnly') or captions.get(r['id'], ''),
                         source=r['provenance'], templates=r.get('templates', []),
                         bodies=r['bodyRegions'], kashidas=r['kashidaRegions'], comparison=comparison))
    rows.sort(key=lambda r: tuple(map(int, re.findall(r'\d+', r['id']))))
    index = dict(datasetSha256=digest(run / 'dataset.json'), rows=rows, canvas=CANVAS,
                 templateSha256=read(run / 'inputs/snapshot.json')['templateFileSha256'],
                 previousRun=previous['summary']['run'] if previous else None)
    (out / 'gallery.json').write_text(json.dumps(index, ensure_ascii=False, separators=(',', ':')))
    for ext, target in [('html', 'index.html'), ('mjs', 'review.mjs'), ('css', 'review.css')]:
        shutil.copyfile(APP / ('letter-training-review.' + ext), out / target)
    font = Path(read(run / 'inputs/snapshot.json')['directory']) / 'report-quran.woff'
    if font.is_file():
        shutil.copyfile(font, out / 'report-quran.woff')


def prepare_examples(run, out, records, trainer):
    captions = reviewed_display_text(run, records)
    examples = {}
    rows = {}
    # Run the archived loader in chunks. Its original int64 canvases would
    # otherwise occupy over a gigabyte while waiting for their next draw.
    with tempfile.TemporaryDirectory() as temporary:
        chunk = Path(temporary)
        (chunk / 'masks').symlink_to(run / 'masks', target_is_directory=True)
        for start in range(0, len(records), 100):
            batch = records[start:start + 100]
            (chunk / 'dataset.json').write_text(json.dumps({'records': batch}))
            _, loaded = trainer.load_dataset(chunk)
            for example in loaded:
                r = example['record']
                key = r['id']
                name = Path(r['file']).stem
                roles = example['roles']
                if len(roles) > 256 or example['canvas'].max().item() >= len(roles):
                    raise ValueError(f'Invalid instance IDs in {key}')
                canvas = example['canvas'].to(torch.uint8)
                examples[key] = dict(canvas=canvas, roles=roles)
                save_instances(out / 'base' / f'{name}.png', canvas.numpy(), roles)
                shutil.copyfile(run / r['file'], out / 'raw' / f'{name}.npz')
                ids = np.unique(canvas.numpy())
                present = roles[ids[ids > 0]]
                rows[key] = dict(id=key, file=name, source=r['provenance'],
                                 text=r.get('textForDisplayOnly') or captions.get(key, ''),
                                 templates=r.get('templates', []), bodies=int((present == 1).sum()),
                                 kashidas=int((present == 2).sum()), draws=[])
    return examples, rows


def replay(trainer, examples, rows, records, settings, device, out):
    # The frozen coverage sampler and the affine augmentation share one RNG.
    # Replay both in order, then reconcile per-ID totals with the checkpoint.
    rng = random.Random(settings['seed'])
    automatic = [r['id'] for r in records if r['group'] == 'weak_train']
    reviewed = [r['id'] for r in records if r['group'] == 'human_train']
    rng.shuffle(automatic)
    cursor = 0
    for step in range(1, settings['step'] + 1):
        keys = []
        for _ in range(settings['batchSize']):
            if reviewed and rng.random() < settings['humanSampleProbability']:
                keys.append(rng.choice(reviewed))
            else:
                if cursor == len(automatic):
                    rng.shuffle(automatic)
                    cursor = 0
                keys.append(automatic[cursor])
                cursor += 1

        def capture(*, pixel_values, mask_labels, class_labels):
            # These are the arguments that loss_for passes to model.forward.
            ink = (pixel_values[:, 0] < 0).cpu().numpy()
            for slot, (key, masks, classes) in enumerate(zip(keys, mask_labels, class_labels), 1):
                masks = masks.bool().cpu().numpy()
                roles = np.r_[0, classes.cpu().numpy() + 1]
                pixels = np.zeros(ink.shape[1:], dtype=np.uint8)
                for index, mask in enumerate(masks, 1):
                    pixels[mask] = index
                if not np.array_equal(pixels > 0, ink[slot - 1]) or masks.sum() != (pixels > 0).sum():
                    raise ValueError(f'Target partition differs from model input at step {step}, slot {slot}')
                save_instances(out / 'draws' / f'{step}-{slot}.png', pixels, roles)
                rows[key]['draws'].append([step, slot, int((roles == 1).sum()), int((roles == 2).sum())])
            return SimpleNamespace(loss=None)

        trainer.loss_for(capture, 'mask2former', [examples[k] for k in keys], device, rng)


def build(run, out, device):
    model = run / 'mask2former'
    settings = read(model / 'settings.json')
    log = read(model / 'training.json')
    manifest = read(run / 'dataset.json')
    counts = read(model / 'sample-counts-at-best.json')
    if digest(run / 'dataset.json') != settings['datasetSha256']:
        raise ValueError('The dataset does not match the selected checkpoint')
    if not manifest.get('sampling', {}).get('coverEveryTrainingExample'):
        raise ValueError('This exporter requires a recorded coverage-sampled training run')
    records = [r for r in manifest['records'] if r['group'] in ('human_train', 'weak_train')]
    if (len(records) != len(counts) or {r['id'] for r in records} != set(counts)
            or any(r['split'] != 'train' or counts[r['id']] <= 0 for r in records)):
        raise ValueError('The sampling ledger and training split disagree')
    trainer = frozen_trainer(run)
    if torch.__version__ != log['torchVersion'] or list(trainer.CANVAS) != settings['canvas']:
        raise ValueError('Use the recorded PyTorch version and training canvas for replay')
    device = device or log['device']
    if device == 'mps' and not torch.backends.mps.is_available():
        raise RuntimeError('The recorded Metal training device is unavailable. CPU replay requires --device cpu.')
    for name in ('base', 'draws', 'raw'):
        (out / name).mkdir()
    examples, rows = prepare_examples(run, out, records, trainer)
    with torch.inference_mode():
        replay(trainer, examples, rows, records, settings, device, out)
    if counts != {key: len(row['draws']) for key, row in rows.items()}:
        raise ValueError('Replayed draws differ from the checkpoint sampling log')
    source_counts = Counter(row['source'] for row in rows.values())
    draws_by_source = Counter()
    for row in rows.values():
        draws_by_source[row['source']] += len(row['draws'])
    summary = dict(run=run.name, checkpointSha256=digest(model / 'best.pt'),
                   datasetSha256=settings['datasetSha256'], canvas=settings['canvas'],
                   step=settings['step'], batchSize=settings['batchSize'], draws=sum(counts.values()),
                   sourceCounts=source_counts, drawsBySource=draws_by_source,
                   trainingDevice=log['device'], replayDevice=device, torchVersion=log['torchVersion'])
    order = sorted(rows, key=lambda key: tuple(map(int, re.findall(r'\d+', key))))
    splits = {p['id']: p['split'] for p in read(run / 'inputs/corpus.json')['pages']}
    (out / 'gallery.json').write_text(json.dumps(dict(summary=summary, pageSplits=splits,
        rows=[rows[key] for key in order]), ensure_ascii=False, separators=(',', ':')))
    for original, target in [('letter-training-gallery.html', 'index.html'), ('letter-training-gallery.mjs', 'gallery.mjs')]:
        shutil.copyfile(APP / original, out / target)
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('experiment', type=Path)
    parser.add_argument('output', type=Path, help='New gallery directory')
    parser.add_argument('--device', choices=('cpu', 'mps', 'cuda'), help='Defaults to the original training device')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Choose a new gallery directory to preserve the existing export')
    torch.set_num_threads(2)
    args.output.parent.mkdir(exist_ok=True, parents=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
        stage = Path(temporary) / 'gallery'
        stage.mkdir()
        build(args.experiment.resolve(), stage, args.device)
        stage.rename(args.output)
