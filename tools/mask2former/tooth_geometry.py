"""Measure the visible spaces between three tooth peaks on frozen page splits."""
import argparse
from collections import Counter
from pathlib import Path

from workspace import read, run_path

import numpy as np
from scipy.ndimage import distance_transform_edt, gaussian_filter1d
from scipy.signal import find_peaks
from scipy.stats import mannwhitneyu
from skimage.morphology import skeletonize

from letter_corpus_audit import write_json
from seen import layout
from train import examples, focus_validation


def measure_teeth(ink):
    """Find the three strongest upper-contour peaks and measure their intervening flats."""
    occupied = ink.any(axis=0)
    if not ink.any() or not occupied.all():
        return None
    skeleton = skeletonize(np.ascontiguousarray(ink))
    width = max(1., float(2 * np.median(distance_transform_edt(ink)[skeleton])))
    top = gaussian_filter1d(ink.argmax(axis=0).astype(float), sigma=max(.65, .3 * width))
    pad = float(max(top.max(), ink.shape[0] - 1))
    peaks, properties = find_peaks(-np.r_[pad, top, pad], prominence=.65 * width,
                                   distance=max(2, round(width)))
    if len(peaks) < 3:
        return None
    peaks = np.sort(peaks[np.argsort(properties['prominences'], kind='stable')[-3:]] - 1)
    valleys = [a + int(np.argmax(top[a:b + 1])) for a, b in zip(peaks[:-1], peaks[1:], strict=True)]
    heights = (np.interp(peaks, valleys, top[valleys]) - top[peaks]) / width
    if heights.min() < .65:
        return None
    gaps = []
    for start, valley, stop in zip(peaks[:-1], valleys, peaks[1:], strict=True):
        level = top[valley] - .5 * width
        a, b = valley, valley
        while a > start and top[a - 1] >= level:
            a -= 1
        while b < stop and top[b + 1] >= level:
            b += 1
        gaps.append((b - a + 1) / width)
    return dict(penWidth=width, heights=heights.tolist(), gaps=gaps,
                highTooth=float(heights.max() - np.median(heights)),
                longestGap=max(gaps), gapDifference=max(gaps) - min(gaps))


def observations(sample, letters, coverage):
    seen, triples = layout(letters)
    groups = [('seen', [l['letterIndex']]) for l in seen] + [('senna', ids) for ids in triples]
    labels = sample['labels']
    source = labels > 0
    for kind, ids in groups:
        coverage[kind + 'Eligible'] += 1
        y, x = np.where(np.isin(labels, ids))
        if not len(x):
            continue
        # Instance labels supply a search window. Heights and lengths come from
        # the original ink in that window, including any connector ink.
        roi = source[y.min():y.max() + 1, x.min():x.max() + 1]
        features = measure_teeth(roi)
        if features is not None:
            coverage[kind + 'Measured'] += 1
            yield dict(id=sample['record']['id'], kind=kind, letterIndices=ids, **features)


def summarize_geometry(rows):
    result = {}
    for name, pool in [('allMeasured', rows), ('lowTooth', [r for r in rows if r['highTooth'] <= 1])]:
        seen = [r for r in pool if r['kind'] == 'seen']
        senna = [r for r in pool if r['kind'] == 'senna']
        measures = {}
        for key in ('highTooth', 'longestGap', 'gapDifference'):
            a, b = [r[key] for r in seen], [r[key] for r in senna]
            if a and b:
                u = mannwhitneyu(b, a).statistic
                measures[key] = dict(seenMedian=float(np.median(a)), sennaMedian=float(np.median(b)),
                                     aucSennaHigher=float(u / (len(a) * len(b))))
        result[name] = dict(seen=len(seen), senna=len(senna), measures=measures)
    return result


def audit(baseline, output):
    manifest = read(baseline / 'dataset.json')
    run = read(baseline / 'run.json')
    excluded = set(manifest['excludedReviewedIds']) | set(manifest['excludedCropIds'])
    groups = {}
    for split in ('train', 'validation'):
        records = [r for r in manifest['records'] if r['split'] == split]
        samples = examples(baseline, [r for r in records if any(layout(r['letters']))] if split == 'train' else records)
        if split == 'train':
            items = ((sample, sample['record']['letters']) for sample in samples)
        else:
            items = ((item['example'], item['letters']) for item in
                     focus_validation(baseline, run, samples, excluded) if item['letters'])
        coverage = Counter()
        rows = [r for sample, letters in items for r in observations(sample, letters, coverage)]
        groups[split] = dict(coverage=dict(coverage), summary=summarize_geometry(rows), rows=rows)
        print(split, groups[split]['coverage'], groups[split]['summary'], flush=True)
    write_json(output, dict(groups=groups, baseline=str(baseline), datasetSha256=run['datasetSha256'],
                            lowToothMaxHeightDifferencePenWidths=1,
                            length='Horizontal span within half a pen width of the valley floor'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    audit(run_path(args.baseline), args.output)
