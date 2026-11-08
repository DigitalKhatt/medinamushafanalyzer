"""Freeze provenance-separated native letter masks for a page-held-out experiment."""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import unicodedata

import numpy as np
from scipy import ndimage as ndi

from letter_cut_pilot import EXAMPLES, reviewed_partition
from letter_bases import BODY_LETTERS, MIN_TRAINING_BASES, has_training_boundary


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def weak_label_exclusion(row):
    if (row.get('provisional') or any(letter.get('status') == 'remainder' for letter in row.get('letters', []))
            or any(region.get('labelSource') == 'remainder' for region in row.get('regions', []))):
        return 'provisional-remainder'
    if not row['complete']:
        return 'partial'
    return None


def pixels(runs):
    return np.concatenate([np.arange(s, s+n, dtype=np.int64) for s, n in zip(runs[::2], runs[1::2])]) if runs else np.array([], dtype=np.int64)


def normalize_partition(source, regions, roles):
    """Resolve only subpixel raster disagreements; reject larger geometry errors."""
    stack = np.stack(regions)
    count = stack.sum(0)
    conflict = source & (count != 1)
    extra = (count > 0) & ~source
    tolerance = max(8, round(source.sum() * .01))
    if conflict.sum() > tolerance or extra.sum() > tolerance:
        raise ValueError('ink-conservation')
    labels = np.where(source & (count == 1), stack.argmax(0) + 1, 0).astype('uint16')
    if conflict.any():
        if not labels.any(): raise ValueError('no-unambiguous-targets')
        nearest = ndi.distance_transform_edt(labels == 0, return_distances=False, return_indices=True)
        labels[conflict] = labels[tuple(nearest)][conflict]
    if set(np.unique(labels)) != set(range(len(regions)+1)):
        raise ValueError('vanished-region')
    if min(np.bincount(labels.ravel())[1:]) < 3:
        raise ValueError('tiny-region')
    return labels, np.asarray([0, *roles], dtype='uint8'), int(conflict.sum()), int(extra.sum())


def weak_partition(record, raster_width):
    px = pixels(record['sourceRuns'])
    if len(px) < 40: raise ValueError('tiny-source')
    yy, xx = np.divmod(px, raster_width)
    x, y = int(xx.min())-3, int(yy.min())-3
    w, h = int(xx.max())-x+4, int(yy.max())-y+4
    source = np.zeros((h, w), bool); source[yy-y, xx-x] = True
    regions, roles = [], []
    for region in record['regions']:
        p = pixels(region['runs']); ry, rx = np.divmod(p, raster_width)
        if len(p) == 0: raise ValueError('empty-region')
        if ((rx < x) | (rx >= x+w) | (ry < y) | (ry >= y+h)).any():
            raise ValueError('region-outside-source')
        mask = np.zeros_like(source); mask[ry-y, rx-x] = True
        regions.append(mask); roles.append(1 if region['role'] == 'body' else 2)
    if not 1 <= len(regions) <= 24: raise ValueError('region-count')
    labels, roles, conflict, extra = normalize_partition(source, regions, roles)
    return labels, roles, [x, y, w, h], conflict, extra


def human_record(data, page, part, revision, identifier, split):
    bbox, original = reviewed_partition(data, page, part, revision)
    annotation_path = data/'annotations'/f'{page}.json'
    annotation = json.loads(annotation_path.read_text())
    ids = np.unique(original[original > 0])
    labels = np.zeros_like(original, dtype='uint16')
    roles = [0]
    for i, old in enumerate(ids, 1):
        labels[original == old] = i
        roles.append(1 if annotation['parts'][old-1]['role'] == 'body' else 2)
    labels = np.pad(labels, 3)
    record = {'id': identifier, 'page': page, 'split': split, 'group': 'human_'+split,
            'provenance': 'human-reviewed', 'annotationSha256': digest(annotation_path),
            'annotationRevision': annotation['revision'], 'sourceRevision': revision,
            'sourceSha256': annotation['sourceSha256'], 'regionId': part,
            'bbox': [bbox[0]-3, bbox[1]-3, labels.shape[1], labels.shape[0]],
            'raster': annotation['raster'], 'repairedPixels': 0}
    if split == 'train':
        owners = {annotation['parts'][old-1]['owner'] for old in ids if annotation['parts'][old-1]['role'] == 'body'}
        record['baseCharacterCount'] = sum(unicodedata.normalize('NFKC', annotation['groups'][owner]['label'])
                                           in BODY_LETTERS for owner in owners)
    return record, labels, np.array(roles, dtype='uint8')


def build(data, folder):
    corpus = json.loads((data/'corpus.json').read_text())
    pages = {p['id']: p for p in corpus['pages']}
    old = data/'automatic/experiments/letter-cut-benchmark'
    reviewed_validation = json.loads((old/'validation-proposals.json').read_text())['examples']
    queue = list(csv.DictReader((old/'review-queue.csv').open()))
    human = []
    for page, part in EXAMPLES.items():
        identifier = next(r['subword_id'] for r in queue if r['subword_id'].startswith(page+':'))
        human.append(human_record(data, page, part, 1, identifier, 'train'))
    for row in reviewed_validation:
        human.append(human_record(data, row['page'], row['regionId'], row['annotationRevision'], row['subwordId'], 'validation'))
    human_ids = {r['id'] for r, _, _ in human}
    weak = []; excluded = Counter(); snapshot_hashes = {}
    validation_review_pages = {r['page'] for r in reviewed_validation}
    for path in sorted((folder/'vector').glob('page*.json')):
        document = json.loads(path.read_text()); page = document['page']; page_info = pages[page]
        if document['sourceSha256'] != page_info['sha256']:
            raise ValueError(f'{page}: SVG source hash mismatch')
        snapshot_hashes[path.name] = digest(path)
        for row in document['examples']:
            if row['id'] in human_ids: excluded['human-override'] += 1; continue
            reason = weak_label_exclusion(row)
            if reason: excluded[reason] += 1; continue
            letters = ''.join(ch for ch in row['text'] if not unicodedata.category(ch).startswith('M'))
            if not letters or any(ch not in BODY_LETTERS for ch in letters):
                excluded['non-letter'] += 1; continue
            if len(letters) != sum(r['role'] == 'body' for r in row['regions']):
                excluded['letter-count'] += 1; continue
            if page_info['split'] == 'train' and len(letters) < MIN_TRAINING_BASES:
                excluded['fewer-than-two-training-bases'] += 1; continue
            try: labels, roles, bbox, repaired, extra = weak_partition(row, document['raster']['width'])
            except ValueError as e: excluded[str(e)] += 1; continue
            split = page_info['split']
            group = 'weak_'+split
            if split == 'validation' and page in validation_review_pages: group = 'weak_validation_review_pages'
            record = {'id': row['id'], 'page': page, 'split': split, 'group': group,
                'provenance': 'machine-weak', 'textForDisplayOnly': row['text'], 'bbox': bbox,
                'sourceSha256': document['sourceSha256'], 'sourceCacheSha256': document['sourceCacheSha256'],
                'raster': document['raster'], 'pdfToRaster': document['pdfToRaster'],
                'templateSha256': document['templateSha256'], 'cacheVersion': document['cacheVersion'],
                'templates': row['templates'], 'repairedPixels': repaired, 'clippedExtraPixels': extra}
            weak.append((record, labels, roles))
    # Retain the historical cap on no-cut controls in validation and test.
    selected = []
    for group in sorted({r['group'] for r, _, _ in weak}):
        pool = [x for x in weak if x[0]['group'] == group]
        multi = [x for x in pool if len(x[2]) > 2]
        single = sorted((x for x in pool if len(x[2]) == 2),
                        key=lambda x: hashlib.sha256(x[0]['id'].encode()).hexdigest())
        chosen = single[:max(1, len(multi)//5)]
        selected.extend(multi + chosen); excluded['single-letter-cap'] += len(single)-len(chosen)
    for entry in human:
        record = entry[0]
        if record['split'] == 'train' and not has_training_boundary(record):
            excluded['fewer-than-two-training-bases'] += 1
        else:
            selected.append(entry)
    (folder/'masks').mkdir(parents=True, exist_ok=True)
    records = []
    for record, labels, roles in sorted(selected, key=lambda x: x[0]['id']):
        key = record['id'].replace(':', '__')
        filename = f'masks/{key}.npz'
        np.savez_compressed(folder/filename, instances=labels, roles=roles)
        record.update(file=filename, maskSha256=digest(folder/filename),
                      regions=len(roles)-1, bodyRegions=int((roles == 1).sum()),
                      kashidaRegions=int((roles == 2).sum()), inkPixels=int((labels > 0).sum()))
        assert record['split'] == pages[record['page']]['split']
        records.append(record)
    groups = Counter(r['group'] for r in records)
    assert groups['human_validation'] == 6
    assert groups['weak_train'] > 100 and groups['weak_validation'] > 0
    manifest = {'schemaVersion': 1, 'seed': 63, 'splitPolicy': corpus['splitPolicy'],
        'input': 'Binary isolated subword ink only; no text, character count, landmarks or template identifiers',
        'target': 'Letter body and kashida instances; letter identity is not trained',
        'roles': {'0': 'background', '1': 'body', '2': 'kashida'},
        'tuningGroup': 'weak_validation', 'reviewedEvaluationGroup': 'human_validation',
        'testUse': 'Evaluation only; template agreement is a proxy, not human measured accuracy',
        'weakRasterPolicy': 'Reject conflict/extra > max(8 pixels,1% ink); remaining ambiguous source pixels go to nearest unambiguous region',
        'minimumTrainingBaseCharacters': MIN_TRAINING_BASES,
        'groups': dict(groups), 'exclusions': dict(excluded), 'vectorSnapshotHashes': snapshot_hashes,
        'records': records}
    (folder/'dataset.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({'groups':dict(groups), 'exclusions':dict(excluded), 'regions':sum(r['regions'] for r in records)}, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path('data'))
    parser.add_argument('--folder', type=Path, required=True)
    args = parser.parse_args(); build(args.data, args.folder)
