"""Compare frozen CNN cut proposals and Mask2Former on reviewed body ink."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy import ndimage as ndi
from scipy.optimize import linear_sum_assignment

from letter_cut_pilot import EXAMPLES, reviewed_partition

ADJACENCY = np.ones((3, 3), dtype=bool)


def split_at_points(ink, points):
    """Use the shortest local chord that separates two viable ink regions."""
    remaining = ink.copy()
    minimum_area = max(8, round(ink.sum() * .025))
    for y, x in points:
        if not remaining[y, x]:
            continue
        labels, count = ndi.label(remaining, structure=ADJACENCY)
        component = labels == labels[y, x]
        if component.sum() < 2 * minimum_area:
            continue
        best = None
        for angle in np.arange(0, np.pi, np.pi / 16):
            dy, dx = np.sin(angle), np.cos(angle)
            chord = {(y, x)}
            for direction in (-1, 1):
                for step in range(1, 14):
                    r = round(y + direction * step * dy)
                    c = round(x + direction * step * dx)
                    if not (0 <= r < ink.shape[0] and 0 <= c < ink.shape[1] and component[r, c]):
                        break
                    chord.add((r, c))
                else:
                    break
            else:
                if best is not None and len(chord) >= len(best):
                    continue
                trial = remaining.copy()
                trial[tuple(np.array(sorted(chord)).T)] = False
                trial_labels, new_count = ndi.label(trial, structure=ADJACENCY)
                if new_count != count + 1:
                    continue
                children = np.bincount(trial_labels[component & trial])
                if len(children[children > 0]) == 2 and min(children[children > 0]) >= minimum_area:
                    best = chord
        if best:
            remaining[tuple(np.array(sorted(best)).T)] = False

    labels, _ = ndi.label(remaining, structure=ADJACENCY)
    nearest = ndi.distance_transform_edt(~remaining, return_distances=False, return_indices=True)
    return np.where(ink, labels[tuple(nearest)], 0)


def metrics(truth, prediction):
    a, b = np.unique(truth[truth > 0]), np.unique(prediction[prediction > 0])
    if len(b):
        intersections = np.array([[( (truth == i) & (prediction == j)).sum() for j in b] for i in a])
        truth_sizes = np.array([(truth == i).sum() for i in a])
        predicted_sizes = np.array([(prediction == j).sum() for j in b])
        iou = intersections / (truth_sizes[:, None] + predicted_sizes - intersections)
        rows, cols = linear_sum_assignment(-iou)
        matched = iou[rows, cols]
        hits = int((matched >= .5).sum())
    else:
        matched, hits = np.array([]), 0
    return {'referenceRegions': len(a), 'predictedRegions': len(b), 'matchesAtIoU50': hits,
            'matchedIoUSum': float(matched.sum()),
            'coveredInkPixels': int(((truth > 0) & (prediction > 0)).sum()),
            'sourceInkPixels': int((truth > 0).sum())}


def mask2former(checkpoint, training, validation, steps):
    import torch
    from torch.nn.functional import interpolate
    from transformers import Mask2FormerForUniversalSegmentation

    torch.manual_seed(63)
    torch.set_num_threads(4)
    model = Mask2FormerForUniversalSegmentation.from_pretrained(
        checkpoint, num_labels=1, ignore_mismatched_sizes=True, train_num_points=1024,
        use_auxiliary_loss=False, output_hidden_states=True)
    model.model.pixel_level_module.requires_grad_(False)
    model.model.pixel_level_module.eval()
    optimizer = torch.optim.AdamW((p for p in model.parameters() if p.requires_grad), lr=.0002)
    mean = torch.tensor([.485, .456, .406])[None, :, None, None]
    std = torch.tensor([.229, .224, .225])[None, :, None, None]

    def image(ink):
        h, w = ink.shape
        # Preserve aspect ratio and keep native ink pixels for final scoring.
        scale = min(120 / h, 184 / w)
        shape = (max(1, round(h * scale)), max(1, round(w * scale)))
        resized = interpolate(torch.tensor(ink, dtype=torch.float32)[None, None], shape, mode='nearest')
        canvas = torch.ones((1, 3, 128, 192))
        canvas[:, :, :shape[0], :shape[1]] = 1 - resized
        return (canvas - mean) / std, shape

    train_inputs = []
    for partition in training:
        inputs, shape = image(partition > 0)
        regions = torch.tensor(np.stack([partition == i for i in np.unique(partition) if i]), dtype=torch.float32)
        target = torch.zeros((len(regions), 128, 192))
        target[:, :shape[0], :shape[1]] = interpolate(regions[:, None], shape, mode='nearest')[:, 0]
        train_inputs.append((inputs, target))

    model.train()
    model.model.pixel_level_module.eval()
    for step in range(steps):
        inputs, masks = train_inputs[step % len(train_inputs)]
        optimizer.zero_grad()
        loss = model(pixel_values=inputs, mask_labels=[masks],
                     class_labels=[torch.zeros(len(masks), dtype=torch.long)]).loss
        loss.backward()
        optimizer.step()
        if (step + 1) % 6 == 0:
            print(f'Mask2Former step {step + 1}/{steps}, loss {loss.item():.3f}', flush=True)

    model.eval()
    predictions = []
    with torch.inference_mode():
        for ink in validation:
            inputs, shape = image(ink)
            output = model(pixel_values=inputs)
            confidence = output.class_queries_logits[0].softmax(-1)[:, 0]
            masks = interpolate(output.masks_queries_logits, (128, 192), mode='bilinear', align_corners=False)
            masks = interpolate(masks[:, :, :shape[0], :shape[1]], ink.shape,
                                mode='bilinear', align_corners=False)[0].sigmoid()
            strength = masks * confidence[:, None, None]
            eligible = (confidence[:, None, None] >= .5) & (masks >= .5)
            best, ids = strength.masked_fill(~eligible, 0).max(0)
            predictions.append(np.where(ink & (best.numpy() > 0), ids.numpy() + 1, 0))
    return predictions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path('data'))
    parser.add_argument('--checkpoint', type=Path)
    parser.add_argument('--steps', type=int, default=48)
    args = parser.parse_args()
    folder = args.data / 'automatic/experiments/letter-cut-benchmark'
    frozen = json.loads((folder / 'validation-proposals.json').read_text())
    assert frozen['split'] == 'validation'
    from automatic.reviews import mask_digest
    rows, references, masks = [], [], {}
    for record in frozen['examples']:
        page = record['page']
        original = json.loads((args.data / 'history' / page /
                               f'revision-{record["annotationRevision"]:06}.json').read_text())
        body = next(part for part in original['parts'] if part['id'] == record['regionId'])
        assert mask_digest(body['runs']) == record['bodyMaskSha256']
        (x, y, _, _), reference = reviewed_partition(
            args.data, page, record['regionId'], record['annotationRevision'])
        points = [(point['y'] - y, point['x'] - x) for point in record['proposals']]
        predicted = split_at_points(reference > 0, points)
        assert np.array_equal(predicted > 0, reference > 0)
        masks[f'{page}_cnn'] = predicted
        masks[f'{page}_reference'] = reference
        references.append(reference)
        rows.append({'page': page, 'subwordId': record['subwordId'], 'originXY': [x, y],
                     'cnn': metrics(reference, predicted)})

    def save():
        totals = {}
        for name in rows[0].keys() & {'cnn', 'mask2former'}:
            values = {key: sum(row[name][key] for row in rows) for key in rows[0][name]}
            values['instanceF1AtIoU50'] = 2 * values['matchesAtIoU50'] / (values['referenceRegions'] + values['predictedRegions'])
            values['meanIoUPerReference'] = values['matchedIoUSum'] / values['referenceRegions']
            totals[name] = values
        result = {'split': 'validation', 'transcriptAtInference': False,
                  'target': 'Instance masks for reviewed body and join regions, ignoring role and letter identity',
                  'cnnCheckpointSha256': frozen['checkpointSha256'], 'totals': totals, 'examples': rows}
        if 'mask2former' in totals:
            result.update(mask2formerCheckpoint='facebook/mask2former-swin-tiny-coco-instance',
                          mask2formerSteps=args.steps)
        np.savez_compressed(folder / 'validation-instance-masks.npz', **masks)
        (folder / 'validation-mask-results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(totals, indent=2), flush=True)

    save()
    if args.checkpoint:
        training = [reviewed_partition(args.data, page, part)[1] for page, part in EXAMPLES.items()]
        for row, reference, prediction in zip(
                rows, references, mask2former(args.checkpoint, training, [r > 0 for r in references], args.steps)):
            row['mask2former'] = metrics(reference, prediction)
            masks[f'{row["page"]}_mask2former'] = prediction
        save()


if __name__ == '__main__':
    main()
