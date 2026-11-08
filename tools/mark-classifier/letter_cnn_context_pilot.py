"""Evaluate wider CNN context with spatial pooling and the existing cutter."""
import argparse
import json
from pathlib import Path

import numpy as np
import torch
from torch import nn

from letter_cut_pilot import EXAMPLES, candidate_patches, example, reviewed_partition, select_points
from letter_mask_pilot import metrics, split_at_points

PATCH_SIZE = 31
STEPS = 160


def train(records):
    torch.manual_seed(63)
    model = nn.Sequential(
        nn.Conv2d(1, 12, 5, padding=2), nn.ReLU(), nn.MaxPool2d(2),
        nn.Conv2d(12, 16, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2),
        nn.AdaptiveAvgPool2d((3, 3)), nn.Flatten(), nn.Linear(144, 1),
    )
    optimizer = torch.optim.AdamW(model.parameters(), lr=.001)
    rng = np.random.default_rng(63)
    positives = np.concatenate([patches[positive] for patches, positive, _ in records])
    negatives = np.concatenate([patches[negative] for patches, _, negative in records])
    target = torch.tensor([1.] * 24 + [0.] * 24)
    for _ in range(STEPS):
        batch = np.concatenate((positives[rng.choice(len(positives), 24)],
                                negatives[rng.choice(len(negatives), 24)]))
        optimizer.zero_grad()
        logits = model(torch.from_numpy(batch)[:, None]).flatten()
        loss = nn.functional.binary_cross_entropy_with_logits(logits, target)
        loss.backward()
        optimizer.step()
    return model.eval()


@torch.inference_mode()
def scores(model, patches):
    return model(torch.from_numpy(patches)[:, None]).flatten().sigmoid().numpy()


def predict(ink, points, values, settings):
    chosen = select_points(points, values, *settings)
    return split_at_points(ink, points[chosen])


def summarize(rows):
    totals = {key: sum(row[key] for row in rows) for key in rows[0]}
    totals['instanceF1AtIoU50'] = 2 * totals['matchesAtIoU50'] / (totals['referenceRegions'] + totals['predictedRegions'])
    totals['meanIoUPerReference'] = totals['matchedIoUSum'] / totals['referenceRegions']
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path('data'))
    args = parser.parse_args()
    torch.set_num_threads(4)
    training = []
    for page, part in EXAMPLES.items():
        _, reference = reviewed_partition(args.data, page, part)
        _, positive, negative, _, expected_points = example(args.data, page, part)
        points, patches = candidate_patches(reference > 0, PATCH_SIZE)
        assert np.array_equal(points, expected_points)
        training.append((reference, points, (patches, positive, negative)))
    folds = []
    for index, (reference, points, record) in enumerate(training):
        model = train([other for j, (_, _, other) in enumerate(training) if j != index])
        folds.append((reference, points, scores(model, record[0])))
        print(f'Training fold {index + 1}/{len(training)} complete', flush=True)

    def assess(settings):
        return summarize([metrics(ref, predict(ref > 0, points, values, settings))
                          for ref, points, values in folds])

    grid = [(threshold, spacing) for threshold in (.4, .5, .6, .7, .8, .9)
            for spacing in (6, 9, 12, 15)]
    settings = max(grid, key=lambda pair: assess(pair)['instanceF1AtIoU50'])
    fold_totals = assess(settings)
    model = train([record for _, _, record in training])
    folder = args.data / 'automatic/experiments/letter-cut-benchmark'
    baseline = json.loads((folder / 'validation-mask-results.json').read_text())
    rows, masks = [], {}
    with np.load(folder / 'validation-instance-masks.npz') as arrays:
        for row in baseline['examples']:
            page = row['page']
            reference = arrays[f'{page}_reference']
            ink = reference > 0
            points, patches = candidate_patches(ink, PATCH_SIZE)
            predicted = predict(ink, points, scores(model, patches), settings)
            assert np.array_equal(predicted > 0, ink)
            masks[page] = predicted
            rows.append({'page': page, 'contextCnn': metrics(reference, predicted), 'originalCnn': row['cnn']})
    totals = summarize([row['contextCnn'] for row in rows])
    report = {'split': 'validation', 'transcriptAtInference': False,
              'patchSize': PATCH_SIZE, 'trainingSteps': STEPS,
              'threshold': settings[0], 'minimumSpacingPixels': settings[1],
              'trainingFoldTotals': fold_totals, 'totals': totals,
              'originalCnnTotals': baseline['totals']['cnn'], 'examples': rows}
    torch.save(model.state_dict(), folder / 'context-cnn-weights.pt')
    np.savez_compressed(folder / 'validation-context-cnn-masks.npz', **masks)
    (folder / 'validation-context-cnn-results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(totals, indent=2))


if __name__ == '__main__':
    main()
