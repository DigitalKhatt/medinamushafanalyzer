"""Compare transcript-free skeleton cut candidates with the frozen CNN."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from scipy.ndimage import gaussian_filter1d
from scipy.spatial import QhullError
from scipy.spatial.distance import cdist
from workbench_paths import NATIVE

from letter_cut_pilot import (
    EXAMPLES, PATCH, matched_cuts, example, reviewed_partition, score,
    select_points, train, windows,
)
from letter_mask_pilot import metrics, split_at_points

sys.path.insert(0, str(NATIVE.parent /
                       'mushafanalyzer/tools/paw_segmentation'))
import segment_paw_letters as paw


def geometry(ink):
    mask = np.pad(ink.astype('uint8') * 255, 8)
    bootstrap = paw.estimate_stroke_width(mask)
    if bootstrap <= 0:
        return [], 'stroke width unavailable'
    # The analyzer normally receives the word baseline. Estimate it from ink.
    baseline = float(gaussian_filter1d(mask.sum(axis=1).astype(float),
                                       max(1, bootstrap / 2)).argmax())
    contours = paw.paw_contours(mask, bootstrap)
    try:
        medial = paw.build_medial(paw.medial_topology_mask(mask, contours),
                                  paw.contour_generators(contours, bootstrap),
                                  bootstrap, baseline)
    except QhullError:
        return [], 'degenerate Voronoi input'
    if medial is None:
        return [], 'no medial axis'
    width = 2 * float(np.median(medial.radii[medial.spine]))
    vertex_s = paw.vertex_spine_coordinates(medial)
    pixel_s = paw.pixel_spine_coordinates(mask, medial, vertex_s, np.nonzero(mask))
    profile = paw.cut_profile(mask, medial, width, pixel_s)
    if profile is None:
        return [], 'spine too short'
    return paw.cut_candidates(profile, baseline, width), None


def candidates(ink, weights):
    rules, reason = geometry(ink)
    points = np.array([(round(c.xy[1]) - 8, round(c.xy[0]) - 8)
                       for c in rules], dtype=int).reshape(-1, 2)
    valid = (points >= 0).all(axis=1) & (points < ink.shape).all(axis=1)
    valid[valid] = ink[tuple(points[valid].T)]
    points = points[valid]
    maximum = sum((paw.W_THINNESS, paw.W_NARROWNESS, paw.W_PURITY,
                   paw.W_ADJACENCY, paw.W_BASELINE, paw.W_HORIZONTAL))
    rule_scores = np.array([c.score for c in rules])[valid] / maximum
    if len(points):
        patches = np.lib.stride_tricks.sliding_window_view(
            np.pad(ink, PATCH // 2), (PATCH, PATCH))
        cnn = score(windows(patches[tuple(points.T)].astype('f4')), *weights)
    else:
        cnn = rule_scores
    return points, {'skeleton': rule_scores, 'hybrid': (rule_scores + cnn) / 2}, reason


def interfaces(reference):
    edges = {}
    for dy, dx in ((0, 1), (1, 0), (1, 1), (1, -1)):
        for y, x in np.argwhere(reference):
            r, c = y + dy, x + dx
            if (0 <= r < reference.shape[0] and 0 <= c < reference.shape[1]
                    and reference[r, c] and reference[y, x] != reference[r, c]):
                key = tuple(sorted((reference[y, x], reference[r, c])))
                edges.setdefault(key, []).append(((y + r) / 2, (x + c) / 2))
    return list(edges.values())


def evaluate(reference, points, edges):
    prediction = split_at_points(reference > 0, points)
    assert np.array_equal(prediction > 0, reference > 0)
    distances = np.stack([cdist(points, edge).min(axis=1) for edge in edges], axis=1)
    hits = matched_cuts(distances, list(range(len(points))))
    return (metrics(reference, prediction) |
            {'matchedPoints': hits, 'extraPoints': len(points) - hits,
             'missedInterfaces': len(edges) - hits}, prediction)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path('data'))
    args = parser.parse_args()
    folder = args.data / 'automatic/experiments/letter-cut-benchmark'
    frozen = json.loads((folder / 'validation-proposals.json').read_text())
    checkpoint = folder / 'pilot-cnn.npz'
    assert hashlib.sha256(checkpoint.read_bytes()).hexdigest() == frozen['checkpointSha256']
    with np.load(checkpoint) as weights_file:
        weights = tuple(weights_file[name] for name in ('kernel', 'head', 'bias'))
    assert frozen['split'] == 'validation'
    assert not EXAMPLES.keys() & {row['page'] for row in frozen['examples']}
    training = {page: example(args.data, page, region) for page, region in EXAMPLES.items()}
    folds = []
    for page, region in EXAMPLES.items():
        _, ref = reviewed_partition(args.data, page, region)
        fold_weights = train([record for other, record in training.items() if other != page])
        points, scores, _ = candidates(ref > 0, fold_weights)
        folds.append((ref, interfaces(ref), points, scores))

    settings = {}
    for method in ('skeleton', 'hybrid'):
        best = -1
        for threshold in np.arange(.30, .81, .05):
            for spacing in (6, 9, 12, 15):
                hits = count = 0
                for ref, edges, points, scores in folds:
                    selected = select_points(points, scores[method], threshold, spacing)
                    result, _ = evaluate(ref, points[selected], edges)
                    hits += result['matchedPoints']
                    count += len(selected) + len(edges)
                if 2 * hits / count > best:
                    best = 2 * hits / count
                    settings[method] = {'threshold': round(float(threshold), 2),
                                        'spacing': spacing, 'trainingPointF1': best}

    rows, masks = [], {}
    old_masks = np.load(folder / 'validation-instance-masks.npz')
    for record in frozen['examples']:
        page = record['page']
        (x, y, _, _), ref = reviewed_partition(args.data, page, record['regionId'],
                                                record['annotationRevision'])
        revision = json.loads((args.data / 'annotations' / f'{page}.json').read_text())['revision']
        edges = interfaces(ref)
        points, scores, reason = candidates(ref > 0, weights)
        selections = {'cnn': np.array([(p['y'] - y, p['x'] - x)
                                        for p in record['proposals']], dtype=int).reshape(-1, 2)}
        for method, config in settings.items():
            selections[method] = points[select_points(points, scores[method],
                                                       config['threshold'], config['spacing'])]
        row = {'page': page, 'subword': record['subwordId'], 'reviewRevision': revision,
               'geometryCandidates': len(points), 'geometryFailure': reason,
               'workbench': record['workbench'], 'pointsYX': {}}
        masks[f'{page}_reference'] = ref
        for method, selected in selections.items():
            row[method], prediction = evaluate(ref, selected, edges)
            if method == 'cnn':
                assert np.array_equal(prediction, old_masks[f'{page}_cnn'])
            masks[f'{page}_{method}'] = prediction
            row['pointsYX'][method] = selected.tolist()
        rows.append(row)

    totals = {}
    for method in ('cnn', *settings):
        total = {key: sum(row[method][key] for row in rows) for key in rows[0][method]}
        total['meanIoUPerReference'] = total['matchedIoUSum'] / total['referenceRegions']
        total['instanceF1AtIoU50'] = 2 * total['matchesAtIoU50'] / (
            total['referenceRegions'] + total['predictedRegions'])
        totals[method] = total
    report = {'split': 'validation', 'transcriptAtInference': False,
              'cnnCheckpointSha256': frozen['checkpointSha256'], 'settings': settings,
              'totals': totals, 'examples': rows}
    (folder / 'validation-skeleton-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(totals, indent=2))
    render(folder, rows, masks)


def render(folder, rows, masks):
    import matplotlib.pyplot as plt
    from matplotlib.colors import ListedColormap
    columns = {'reference': 'Reviewed', 'cnn': 'Frozen CNN',
               'skeleton': 'Skeleton rules', 'hybrid': 'Skeleton + CNN'}
    fig, axes = plt.subplots(len(rows), len(columns), figsize=(11, 12), layout='constrained')
    cmap = ListedColormap(['white', '#0072b2', '#e69f00', '#009e73',
                           '#d55e00', '#cc79a7', '#56b4e9', '#7067a8'])
    for i, row in enumerate(rows):
        for j, (method, name) in enumerate(columns.items()):
            ax = axes[i, j]
            mask = masks[f'{row["page"]}_{method}']
            _, dense = np.unique(mask, return_inverse=True)
            ax.imshow(dense.reshape(mask.shape), cmap=cmap, vmin=0, vmax=cmap.N - 1,
                      interpolation='nearest')
            ax.set_xticks([])
            ax.set_yticks([])
            ax.set_frame_on(False)
            if i == 0:
                ax.set_title(name, fontweight='bold')
            if j == 0:
                ax.set_ylabel(row['page'], rotation=0, ha='right', va='center')
            else:
                ax.set_xlabel(f'{row[method]["matchesAtIoU50"]}/{row[method]["referenceRegions"]} masks matched')
    fig.suptitle('Held-out subword segmentation without transcript', fontweight='bold')
    fig.savefig(folder / 'validation-skeleton-comparison.png', dpi=150)
    plt.close(fig)


if __name__ == '__main__':
    main()
