"""Train a small cut-location CNN on the six reviewed workbench examples."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.optimize import linear_sum_assignment
from scipy.spatial.distance import cdist
from skimage.morphology import skeletonize

EXAMPLES = {
    'page063': 'part-01107', 'page364': 'part-00948', 'page003': 'part-00246',
    'page570': 'part-00533', 'page048': 'part-00806', 'page541': 'part-00905',
}
RADIUS = 3
PATCH = 15


def pixels(part):
    return {pixel for start, count in zip(part['runs'][::2], part['runs'][1::2])
            for pixel in range(start, start + count)}


def reviewed_partition(data, page, region, revision=1):
    original = json.loads((data / 'history' / page / f'revision-{revision:06}.json').read_text())
    reviewed = json.loads((data / 'annotations' / f'{page}.json').read_text())
    assert original['sourceSha256'] == reviewed['sourceSha256']
    assert original['raster'] == reviewed['raster']
    body = next(part for part in original['parts'] if part['id'] == region)
    source = pixels(body)
    x, y, w, h = body['bbox']
    width = original['raster']['width']
    owners = np.zeros((h, w), dtype=int)
    seen = set()
    for index, part in enumerate(reviewed['parts'], 1):
        px, py, pw, ph = part['bbox']
        if px + pw <= x or x + w <= px or py + ph <= y or y + h <= py:
            continue
        ink = pixels(part)
        if not ink & source:
            continue
        owner = reviewed['groups'].get(part.get('owner'), {})
        if (not ink <= source or ink & seen or not part['reviewed'] or
                part['role'] not in ('body', 'kashida') or not owner.get('reviewed')):
            raise ValueError(f'{page}: incomplete or inconsistent reviewed cut {part["id"]}')
        seen.update(ink)
        for pixel in ink:
            row, col = divmod(pixel, width)
            owners[row - y, col - x] = index
    if seen != source:
        raise ValueError(f'{page}: reviewed pieces do not conserve the original ink')

    return body['bbox'], owners


def example(data, page, region):
    _, owners = reviewed_partition(data, page, region)
    h, w = owners.shape
    # Each interface between two reviewed regions supplies one reference cut.
    boundaries = {}
    for dy, dx in ((0, 1), (1, 0), (1, 1), (1, -1)):
        for row, col in np.argwhere(owners):
            r, c = row + dy, col + dx
            if 0 <= r < h and 0 <= c < w and owners[r, c] and owners[r, c] != owners[row, col]:
                key = tuple(sorted((owners[row, col], owners[r, c])))
                boundaries.setdefault(key, []).append(((row + r) / 2, (col + c) / 2))
    mask = owners > 0
    candidates, inputs = candidate_patches(mask)
    distances = np.stack([cdist(candidates, edge).min(axis=1) for edge in boundaries.values()], axis=1)
    closest = distances.min(axis=1)
    return inputs, closest <= RADIUS, closest >= 6, distances, candidates


def candidate_patches(mask, patch_size=PATCH):
    candidates = np.argwhere(skeletonize(mask))
    patches = np.lib.stride_tricks.sliding_window_view(np.pad(mask, patch_size // 2), (patch_size, patch_size))
    return candidates, patches[tuple(candidates.T)].astype('f4')


def select_points(candidates, scores, threshold, spacing):
    """Keep the strongest candidate in each neighborhood."""
    selected = []
    for index in np.argsort(-scores, kind='stable'):
        if scores[index] < threshold:
            break
        if all(np.linalg.norm(candidates[index] - candidates[other]) >= spacing for other in selected):
            selected.append(index)
    return selected


def matched_cuts(distances, selected):
    eligible = distances[selected] <= RADIUS
    rows, cols = linear_sum_assignment(~eligible)
    return int(eligible[rows, cols].sum())


def windows(patches):
    # One learned 5x5 convolution, ReLU, spatial average pooling and a sigmoid.
    return np.lib.stride_tricks.sliding_window_view(patches, (5, 5), axis=(1, 2))[:, ::2, ::2].reshape(len(patches), 36, 25)


def score(features, kernel, head, bias):
    activation = features @ kernel
    logits = np.maximum(activation, 0).mean(axis=1) @ head + bias
    return 1 / (1 + np.exp(-np.clip(logits, -30, 30)))


def train(examples):
    patches = np.concatenate([x for x, _, _, _, _ in examples])
    positive = np.concatenate([p for _, p, _, _, _ in examples])
    negative = np.concatenate([n for _, _, n, _, _ in examples])
    features = windows(patches[positive | negative])
    labels = positive[positive | negative].astype('f4')
    rng = np.random.default_rng(63)
    kernel = rng.normal(0, .15, (25, 8)).astype('f4')
    head = rng.normal(0, .3, 8).astype('f4')
    bias = np.float32(0)
    classes = [np.flatnonzero(labels == value) for value in (0, 1)]
    for _ in range(400):
        batch = np.concatenate([rng.choice(indices, 24) for indices in classes])
        x, y = features[batch], labels[batch]
        activation = x @ kernel
        pooled = np.maximum(activation, 0).mean(axis=1)
        error = (1 / (1 + np.exp(-np.clip(pooled @ head + bias, -30, 30))) - y) / len(y)
        gradient = (error[:, None, None] * head * (activation > 0)) / activation.shape[1]
        kernel -= .1 * (x.reshape(-1, 25).T @ gradient.reshape(-1, 8))
        head -= .1 * (pooled.T @ error)
        bias -= .1 * error.sum()
    return kernel, head, bias


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path('data'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    examples = {page: example(args.data, page, region) for page, region in EXAMPLES.items()}
    report = {}
    fold_scores = {}
    for page, (x, positive, negative, distances, candidates) in examples.items():
        weights = train([record for other, record in examples.items() if other != page])
        scores = score(windows(x), *weights)
        fold_scores[page] = scores
        predictions = scores >= .5
        report[page] = {'reviewedInterfaces': distances.shape[1],
                        'interfacesWithCandidate': int((distances.min(axis=0) <= RADIUS).sum()),
                        'interfacesWithPositivePrediction': int(((distances <= RADIUS) & predictions[:, None]).any(axis=0).sum()),
                        'clearNegativeCandidates': int(negative.sum()),
                        'clearNegativePredictedPositive': int((negative & predictions).sum()),
                        'candidatePoints': len(x)}
    references = sum(record[3].shape[1] for record in examples.values())

    def evaluate(threshold, spacing):
        matches = proposed = 0
        for page, (_, _, _, distances, candidates) in examples.items():
            selected = select_points(candidates, fold_scores[page], threshold, spacing)
            matches += matched_cuts(distances, selected)
            proposed += len(selected)
        return 2 * matches / (references + proposed), matches, proposed

    # Select the proposal settings only on training folds, before validation review.
    settings = [(threshold, spacing) for threshold in (.5, .55, .6, .65, .7, .75)
                for spacing in (6, 9, 12, 15)]
    threshold, spacing = max(settings, key=lambda pair: evaluate(*pair))
    f1, matches, proposed = evaluate(threshold, spacing)
    proposal_metrics = {'threshold': threshold, 'minimumSpacingPixels': spacing,
                        'matched': matches, 'proposed': proposed,
                        'references': references, 'f1': f1}
    for page, (_, _, _, distances, candidates) in examples.items():
        selected = select_points(candidates, fold_scores[page], threshold, spacing)
        report[page].update(shortlisted=len(selected), matchedShortlist=matched_cuts(distances, selected))
    args.output.mkdir(parents=True, exist_ok=True)
    kernel, head, bias = train(list(examples.values()))
    np.savez_compressed(args.output / 'pilot-cnn.npz', kernel=kernel, head=head, bias=bias)
    result = {'method': 'Body mask skeleton points, 15px patches, one learned convolution',
              'tolerancePixels': RADIUS, 'threshold': .5,
              'evaluation': 'Leave-one-page-out diagnostic within the six training pages',
              'shortlist': proposal_metrics,
              'pages': report}
    (args.output / 'cnn-pilot.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
