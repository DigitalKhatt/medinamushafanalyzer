"""Evaluate pretrained SAM with geometry-only point prompts on the mask pilot."""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy.ndimage import distance_transform_edt
import torch
from transformers import SamModel, SamProcessor

from letter_mask_pilot import metrics


def predict(model, processor, ink):
    pad = 8
    image = np.repeat((255 * ~np.pad(ink, pad)).astype('u1')[..., None], 3, axis=-1)
    distance = distance_transform_edt(ink)
    points = []
    for y in range(0, ink.shape[0], 12):
        for x in range(0, ink.shape[1], 12):
            cell = distance[y:y + 12, x:x + 12]
            if cell.max() >= 2:
                row, col = np.unravel_index(cell.argmax(), cell.shape)
                points.append([[int(x + col + pad), int(y + row + pad)]])
    inputs = processor(image, input_points=[points], return_tensors='pt')
    with torch.inference_mode():
        output = model(**inputs)
        raw = processor.image_processor.post_process_masks(
            output.pred_masks, inputs.original_sizes, inputs.reshaped_input_sizes,
            binarize=False)[0]
    raw = raw[:, :, pad:-pad, pad:-pad].numpy().reshape(-1, *ink.shape)
    scores = output.iou_scores[0].numpy().ravel()
    kept = []
    for index in np.argsort(-scores, kind='stable'):
        if scores[index] < .8:
            break
        mask = (raw[index] > 0) & ink
        if mask.sum() < 8:
            continue
        if any((mask & other).sum() / (mask | other).sum() >= .7 for other in kept):
            continue
        kept.append(mask)
    labels = np.zeros(ink.shape, dtype='i4')
    for index, mask in enumerate(kept, 1):
        labels[mask & (labels == 0)] = index
    return labels, len(points)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--folder', type=Path, required=True)
    args = parser.parse_args()
    torch.set_num_threads(4)
    model = SamModel.from_pretrained(args.checkpoint).eval()
    processor = SamProcessor.from_pretrained(args.checkpoint, use_fast=False)
    benchmark = json.loads((args.folder / 'validation-mask-results.json').read_text())
    rows, predictions = [], {}
    with np.load(args.folder / 'validation-instance-masks.npz') as data:
        for record in benchmark['examples']:
            page = record['page']
            reference = data[f'{page}_reference']
            predicted, count = predict(model, processor, reference > 0)
            assert not np.any((predicted > 0) & (reference == 0))
            predictions[page] = predicted
            row = {'page': page, 'prompts': count, **metrics(reference, predicted)}
            rows.append(row)
            print(row, flush=True)
    totals = {key: sum(row[key] for row in rows) for key in rows[0] if key != 'page'}
    totals['instanceF1AtIoU50'] = 2 * totals['matchesAtIoU50'] / (totals['referenceRegions'] + totals['predictedRegions'])
    totals['meanIoUPerReference'] = totals['matchedIoUSum'] / totals['referenceRegions']
    result = {'checkpoint': 'facebook/sam-vit-base', 'split': 'validation',
              'prompts': 'Strongest distance-transform pixel per 12x12 ink cell',
              'minimumPredictedIoU': .8, 'duplicateMaskIoU': .7, 'minimumInkPixels': 8,
              'transcriptAtInference': False, 'totals': totals, 'examples': rows}
    np.savez_compressed(args.folder / 'validation-sam-masks.npz', **predictions)
    (args.folder / 'validation-sam-results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(totals, indent=2))


if __name__ == '__main__':
    main()
