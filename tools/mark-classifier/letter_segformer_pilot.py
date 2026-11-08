"""Benchmark a SegFormer-B2 boundary head with the existing geometric cutter."""
import argparse
import json
from pathlib import Path

import numpy as np
import torch
from torch.nn import functional as F
from transformers import SegformerForSemanticSegmentation

from letter_cut_pilot import EXAMPLES, candidate_patches, example, reviewed_partition, select_points
from letter_mask_pilot import metrics, split_at_points

CANVAS = (128, 192)


def prepare(model, mask):
    height, width = mask.shape
    scale = min(120 / height, 184 / width)
    shape = (max(1, round(height * scale)), max(1, round(width * scale)))
    raster = F.interpolate(torch.from_numpy(mask.astype('f4'))[None, None], shape, mode='nearest')
    image = torch.ones(1, 3, *CANVAS)
    image[:, :, :shape[0], :shape[1]] = 1 - raster
    mean = torch.tensor([.485, .456, .406])[None, :, None, None]
    std = torch.tensor([.229, .224, .225])[None, :, None, None]
    with torch.no_grad():
        features = model.segformer((image - mean) / std, output_hidden_states=True).hidden_states
    return features, shape, mask.shape


def boundary_map(head, prepared):
    features, (height, width), native = prepared
    canvas = F.interpolate(head(features), CANVAS, mode='bilinear', align_corners=False)
    return F.interpolate(canvas[:, :, :height, :width], native, mode='bilinear',
                         align_corners=False)[0, 0]


def fit(head, starting_weights, records, steps):
    head.load_state_dict(starting_weights)
    head.train()
    optimizer = torch.optim.AdamW(head.parameters(), lr=.0002)
    torch.manual_seed(63)
    for step in range(steps):
        prepared, points, positive, negative = records[step % len(records)]
        logits = boundary_map(head, prepared)[tuple(points.T)]
        loss = .5 * (F.softplus(-logits[positive]).mean() + F.softplus(logits[negative]).mean())
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()
    head.eval()


@torch.no_grad()
def scores(head, prepared, points):
    return boundary_map(head, prepared)[tuple(points.T)].sigmoid().numpy()


def summarize(rows):
    totals = {key: sum(row[key] for row in rows) for key in rows[0]}
    totals['instanceF1AtIoU50'] = 2 * totals['matchesAtIoU50'] / (totals['referenceRegions'] + totals['predictedRegions'])
    totals['meanIoUPerReference'] = totals['matchedIoUSum'] / totals['referenceRegions']
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--data', type=Path, default=Path('data'))
    parser.add_argument('--steps', type=int, default=48)
    args = parser.parse_args()
    torch.set_num_threads(4)
    torch.manual_seed(63)
    model = SegformerForSemanticSegmentation.from_pretrained(
        args.checkpoint, num_labels=1, ignore_mismatched_sizes=True).eval()
    head = model.decode_head
    starting_weights = {key: value.clone() for key, value in head.state_dict().items()}

    training = []
    for page, part in EXAMPLES.items():
        _, reference = reviewed_partition(args.data, page, part)
        _, positive, negative, _, points = example(args.data, page, part)
        training.append((reference, (prepare(model, reference > 0), points, positive, negative)))
    folds = []
    for index, (reference, record) in enumerate(training):
        fit(head, starting_weights, [item for j, (_, item) in enumerate(training) if j != index], args.steps)
        prepared, points, _, _ = record
        folds.append((reference, points, scores(head, prepared, points)))
        print(f'Training fold {index + 1}/{len(training)} complete', flush=True)

    def decode(reference, points, probabilities, threshold, spacing):
        selected = select_points(points, probabilities, threshold, spacing)
        return split_at_points(reference > 0, points[selected])

    def assess(settings):
        return summarize([metrics(ref, decode(ref, points, values, *settings))
                          for ref, points, values in folds])

    settings = [(threshold, spacing) for threshold in (.5, .55, .6, .65, .7, .75)
                for spacing in (6, 9, 12, 15)]
    chosen = max(settings, key=lambda pair: assess(pair)['instanceF1AtIoU50'])
    fold_result = assess(chosen)
    print(f'Frozen settings {chosen}, training fold F1 {fold_result["instanceF1AtIoU50"]:.3f}', flush=True)
    fit(head, starting_weights, [record for _, record in training], args.steps)

    folder = args.data / 'automatic/experiments/letter-cut-benchmark'
    benchmark = json.loads((folder / 'validation-mask-results.json').read_text())
    rows, masks = [], {}
    with np.load(folder / 'validation-instance-masks.npz') as arrays:
        for record in benchmark['examples']:
            page = record['page']
            reference = arrays[f'{page}_reference']
            ink = reference > 0
            points, _ = candidate_patches(ink)
            probabilities = scores(head, prepare(model, ink), points)
            predicted = decode(reference, points, probabilities, *chosen)
            assert np.array_equal(predicted > 0, ink)
            masks[page] = predicted
            rows.append({'page': page, **metrics(reference, predicted)})
    totals = summarize([{k: v for k, v in row.items() if k != 'page'} for row in rows])
    result = {'checkpoint': 'nvidia/segformer-b2-finetuned-ade-512-512',
              'trainingStepsPerFold': args.steps, 'encoderFrozen': True,
              'transcriptAtInference': False, 'split': 'validation',
              'threshold': chosen[0], 'minimumSpacingPixels': chosen[1],
              'trainingFoldTotals': fold_result, 'totals': totals, 'examples': rows}
    np.savez_compressed(folder / 'validation-segformer-masks.npz', **masks)
    (folder / 'validation-segformer-results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(totals, indent=2))


if __name__ == '__main__':
    main()
