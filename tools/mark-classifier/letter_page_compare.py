"""Project frozen Mask2Former predictions onto a whole page for template review.

Every in-scope letter body is inferred, including partial template matches and
single-letter shapes. Template masks are comparison data, never model inputs.
"""
import argparse
from collections import Counter
import hashlib
from html import escape
import json
import os
from pathlib import Path
import shutil
import sqlite3
import time

import numpy as np
from PIL import Image
from scipy import ndimage as ndi
from scipy.optimize import linear_sum_assignment
import torch

from automatic.common import unpack_runs
from letter_retest import CANVAS, boundary, metric, model_for, predict, summarize
from letter_retest_data import pixels, weak_label_exclusion, weak_partition
from letter_retest_report import colored
from workbench_paths import NATIVE


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def input_fingerprint(document):
    """Only uncut ink reaches the model; label/template changes need no inference."""
    source = dict(raster=document['raster'], sourceSha256=document['sourceSha256'],
                  ink=sorted((row['id'], row['sourceRuns']) for row in document['examples']))
    return hashlib.sha256(json.dumps(source, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def source_crop(row, width):
    """Use the uncut source outline, even when the teacher could not cut it."""
    yy, xx = np.divmod(pixels(row['sourceRuns']), width)
    if not len(xx):
        raise ValueError(f"No source ink: {row['id']}")
    x, y = int(xx.min()) - 3, int(yy.min()) - 3
    w, h = int(xx.max()) - x + 4, int(yy.max()) - y + 4
    ink = np.zeros((h, w), bool)
    ink[yy-y, xx-x] = True
    return ink, [x, y, w, h]


def repair_reference_seams(ink, labels, count, region_count):
    """Repair one-pixel quantization at cut edges for comparison, never training."""
    conflict = ink & (count != 1)
    extra = (count > 0) & ~ink
    budget = max(8, round(int(ink.sum()) * .03))
    if conflict.sum() > budget or extra.sum() > budget or not labels.any():
        return None
    distances, nearest = ndi.distance_transform_edt(labels == 0, return_indices=True)
    if (distances[conflict].max(initial=0) > 1.01 or
            ndi.distance_transform_edt(~ink)[extra].max(initial=0) > 1.01):
        return None
    repaired = labels.copy()
    repaired[conflict] = labels[tuple(nearest)][conflict]
    if (set(np.unique(repaired)) != set(range(region_count + 1)) or
            min(np.bincount(repaired.ravel())[1:]) < 3):
        return None
    return repaired, int(conflict.sum()), int(extra.sum())


def reference_crop(row, width, ink, bbox):
    """Keep failed references unknown apart from bounded one-pixel raster seams."""
    complete = weak_label_exclusion(row) is None
    try:
        labels, roles, rb, repaired, extra = weak_partition(row, width)
        assert rb == bbox
        return labels, roles, dict(valid=complete, rasterError=None,
                                   repairedPixels=repaired, clippedExtraPixels=extra)
    except ValueError as error:
        labels = np.zeros(ink.shape, 'uint16')
        count = np.zeros(ink.shape, 'uint16')
        x, y, w, h = bbox
        roles = [0]
        for i, region in enumerate(row['regions'], 1):
            yy, xx = np.divmod(pixels(region['runs']), width)
            keep = (xx >= x) & (xx < x+w) & (yy >= y) & (yy < y+h)
            yy, xx = yy[keep]-y, xx[keep]-x
            labels[yy, xx] = i
            count[yy, xx] += 1
            roles.append(1 if region['role'] == 'body' else 2)
        labels[~ink | (count != 1)] = 0
        if str(error) == 'ink-conservation':
            repaired = repair_reference_seams(ink, labels, count, len(row['regions']))
            if repaired is not None:
                labels, conflicts, extra = repaired
                return labels, np.asarray(roles, 'uint8'), dict(
                    valid=complete, rasterError=None,
                    rasterRepair='one-pixel-boundary', originalRasterError=str(error),
                    repairedPixels=conflicts, clippedExtraPixels=extra)
        return labels, np.asarray(roles, 'uint8'), dict(valid=False, rasterError=str(error),
                    repairedPixels=0, unknownPixels=int((ink & (labels == 0)).sum()))


def inference_example(row, ink):
    h, w = ink.shape
    scale = min((CANVAS[0]-16)/h, (CANVAS[1]-16)/w)
    shape = (max(1, round(h*scale)), max(1, round(w*scale)))
    resized = np.array(Image.fromarray(ink.astype('uint8')).resize(shape[::-1], Image.Resampling.NEAREST))
    canvas = np.zeros(CANVAS, 'int64')
    oy, ox = (CANVAS[0]-shape[0])//2, (CANVAS[1]-shape[1])//2
    canvas[oy:oy+shape[0], ox:ox+shape[1]] = resized
    # Only this binary canvas reaches the network; neither text nor cuts do.
    return dict(record={'id': row['id'], 'group': 'whole_page'}, labels=ink.astype('uint16'),
                roles=np.array([0, 1], 'uint8'), canvas=torch.from_numpy(canvas), box=(oy, ox, *shape))


def compare_masks(reference, prediction, roles, predicted_roles, ink):
    """Align instance IDs before measuring changes; keep role changes explicit."""
    a, b = np.unique(reference[reference > 0]), np.unique(prediction[prediction > 0])
    mapping = {}
    if len(a) and len(b):
        overlap = np.array([[((reference == i) & (prediction == j)).sum() for j in b] for i in a])
        # Role agreement is preferred only when there is actual overlap.
        rows, cols = linear_sum_assignment(-overlap)
        mapping = {int(b[c]): int(a[r]) for r, c in zip(rows, cols) if overlap[r, c] > 0}
    for i in b:
        if int(i) not in mapping:
            mapping[int(i)] = int(a.max(initial=0)) + len(mapping) + 1
    aligned = np.zeros_like(prediction)
    for old, new in mapping.items():
        aligned[prediction == old] = new
    known = ink & (reference > 0) & (prediction > 0)
    role_change = known & (roles[reference] != predicted_roles[prediction])
    changed = known & ((reference != aligned) | role_change)
    unknown = ink & ((reference == 0) | (prediction == 0))
    tb, pb = boundary(reference), boundary(prediction)
    far_t = tb & (ndi.distance_transform_edt(~pb) > 2) if pb.any() else tb.copy()
    far_p = pb & (ndi.distance_transform_edt(~tb) > 2) if tb.any() else pb.copy()
    stats = dict(referenceBodies=int(sum(roles[i] == 1 for i in a)),
                 referenceKashidas=int(sum(roles[i] == 2 for i in a)),
                 modelBodies=int(sum(predicted_roles[i] == 1 for i in b)),
                 modelKashidas=int(sum(predicted_roles[i] == 2 for i in b)),
                 changedPixels=int(changed.sum()), roleChangedPixels=int(role_change.sum()),
                 unknownPixels=int(unknown.sum()), sourcePixels=int(ink.sum()),
                 changedFraction=float(changed.sum()/max(1, ink.sum())),
                 farBoundaryPixels=int(far_t.sum()+far_p.sum()),
                 rawUnassignedPixels=int((ink & (prediction == 0)).sum()))
    return stats, mapping, changed, role_change, unknown, tb, pb


def priority(stats, reference_status):
    if not reference_status['valid'] or stats['unknownPixels']:
        return 'unresolved'
    if (stats['referenceBodies'], stats['referenceKashidas']) != (stats['modelBodies'], stats['modelKashidas']):
        return 'count'
    # A role changes naturally near a moved body/kashida boundary. Do not call
    # every displaced connector pixel a whole-region classification mistake.
    if stats['farBoundaryPixels']:
        return 'boundary'
    if stats['changedPixels']:
        return 'minor'
    return 'same'


def difference_image(ink, changed, role_change, unknown):
    result = np.full((*ink.shape, 3), 255, 'uint8')
    result[ink] = (65, 75, 87)
    result[changed] = (231, 111, 32)
    result[role_change] = (206, 46, 106)
    result[unknown] = (124, 65, 190)
    return Image.fromarray(result)


def cut_image(ink, tb, pb):
    result = np.full((*ink.shape, 3), 255, 'uint8')
    result[ink] = (209, 215, 222)
    result[tb] = (0, 112, 200)
    result[pb] = (220, 40, 112)
    result[tb & pb] = (36, 48, 61)
    return Image.fromarray(result)


def paste_ink(target, image, bbox, ink):
    x, y, w, h = bbox
    target[y:y+h, x:x+w][ink] = np.asarray(image)[ink]


def load_context(data, page, document):
    """Read current corpus scope, retaining dots/marks only as gray context."""
    path = data/'automatic/corpus.sqlite'
    with sqlite3.connect(f'file:{path}?mode=ro', uri=True) as db:
        info = db.execute('SELECT width,height,source_sha FROM pages WHERE page_id=?', (page,)).fetchone()
        assert info == (document['raster']['width'], document['raster']['height'], document['sourceSha256'])
        scope = json.loads(db.execute("SELECT value FROM meta WHERE key='scope'").fetchone()[0])
        rows = db.execute('SELECT runs FROM items WHERE page_id=? AND in_scope=1', (page,)).fetchall()
    mask = np.zeros((info[1], info[0]), bool)
    for row in rows:
        mask.ravel()[pixels(unpack_runs(row[0]))] = True
    return mask, scope['version']


def main(args):
    folder, page = args.folder, args.page
    output = args.output or folder/page
    output.mkdir(parents=True, exist_ok=True)
    assets = output/'assets'
    assets.mkdir(exist_ok=True)
    vector_path = (args.vector_dir or folder/'vector')/f'{page}.json'
    document = json.loads(vector_path.read_text())
    settings = json.loads((folder/'mask2former/settings.json').read_text())
    corpus = json.loads((args.data/'corpus.json').read_text())
    page_info = next(p for p in corpus['pages'] if p['id'] == page)
    assert document['sourceSha256'] == page_info['sha256']
    assert digest(Path(corpus['sourceDir'])/page_info['filename']) == document['sourceSha256']
    # Compare with the current template config, not just a similarly named file.
    cache = json.loads((args.native/'output/segmentation-cache'/f'{page}.json').read_text())
    config = json.loads((args.native/'templates/letter-forms.json').read_text())
    if cache['templateConfig'] != config or cache['letterFormsSha256'] != document['templateSha256']:
        raise ValueError('Template snapshot is stale; export this page into a fresh comparison vector directory first')
    context, scope_version = load_context(args.data, page, document)
    records, excluded = [], []
    body_letters = set('ءاأإآٱبتثجحخدذرزسشصضطظعغفقكلمنهويىؤئة')
    for row in document['examples']:
        ink, (x, y, w, h) = source_crop(row, document['raster']['width'])
        reason = None
        if not (ink & context[y:y+h, x:x+w]).any():
            reason = 'outside-current-corpus-scope'
        elif not any(ch in body_letters for ch in row['text']):
            reason = 'annotation-sign-not-letter-body'
        if reason:
            excluded.append(dict(id=row['id'], text=row['text'], reason=reason,
                                 sourcePixels=int(ink.sum())))
        else:
            records.append(row)
    prepared = []
    for row in records:
        ink, bbox = source_crop(row, document['raster']['width'])
        labels, roles, status = reference_crop(row, document['raster']['width'], ink, bbox)
        prepared.append((row, ink, bbox, labels, roles, status))
    model_hash = digest(folder/'mask2former/best.pt')
    cache_key = dict(modelSha256=model_hash, vectorSha256=digest(vector_path),
                     threshold=settings['threshold'], canvas=settings.get('canvas',list(CANVAS)), decoder=settings.get('decoder',{}),
                     inferenceCodeSha256=digest(Path(__file__).with_name('letter_retest.py')))
    cache_path = output/'predictions.json'
    prediction_dir = output/'predictions'
    prediction_dir.mkdir(exist_ok=True)
    cached = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    start = time.monotonic()
    if args.reuse_frozen_predictions:
        original_vector = folder/'vector'/f'{page}.json'
        previous = folder/page
        old_cache = json.loads((previous/'predictions.json').read_text())
        expected = dict(cache_key, vectorSha256=digest(original_vector))
        if old_cache.get('key') != expected or input_fingerprint(json.loads(original_vector.read_text())) != input_fingerprint(document):
            raise ValueError('Cannot reuse predictions: model, inference settings, or uncut source ink changed')
        if previous.resolve() == output.resolve():
            raise ValueError('Use a separate --output to preserve the frozen comparison')
        for row in records:
            filename = row['id'].replace(':', '__')+'.npz'
            shutil.copyfile(previous/'predictions'/filename, prediction_dir/filename)
        cached = dict(old_cache, key=cache_key, reusedFrom=str(previous),
                      sourceInputSha256=input_fingerprint(document))
        cache_path.write_text(json.dumps(cached, indent=2)+'\n')
    if cached.get('key') != cache_key or not all((prediction_dir/(r['id'].replace(':', '__')+'.npz')).exists() for r in records):
        torch.set_num_threads(4)
        device = torch.device(args.device)
        model = model_for('mask2former', settings['checkpoint'], device, settings.get('initialization','pretrained'),
                          settings.get('seenBodyClass', False), geometry_input=settings.get('geometryInput', False))
        model.load_state_dict(torch.load(folder/'mask2former/best.pt', map_location='cpu', weights_only=True))
        for n, (row, ink, *_rest) in enumerate(prepared, 1):
            example=inference_example(row,ink)
            example['record']['pdfToRaster']=document['pdfToRaster']
            result = predict(model, 'mask2former', [example], device, settings['threshold'],
                             min_region_area=settings.get('decoder') or settings.get('minimumRegionAreaPixels',3),
                             canvas_shape=settings.get('canvas'))[0]
            np.savez_compressed(prediction_dir/(row['id'].replace(':', '__')+'.npz'),
                                instances=result['prediction'], roles=result['roles'], raw=result['raw'])
            if n % 25 == 0 or n == len(prepared):
                print(json.dumps({'inferred': n, 'total': len(prepared)}), flush=True)
        cached = dict(key=cache_key, device=args.device, inferenceSeconds=time.monotonic()-start, examples=len(records))
        cache_path.write_text(json.dumps(cached, indent=2)+'\n')
    shape = context.shape
    base = np.full((*shape, 3), 255, 'uint8')
    base[context] = (157, 164, 174)
    pages = {name: base.copy() for name in ('input', 'template', 'model', 'model-raw', 'difference', 'difference-raw', 'cuts', 'cuts-raw')}
    body_union = np.zeros(shape, bool)
    overlap = 0
    rows, metrics = [], []
    raw_metrics = []
    for row, ink, bbox, reference, roles, status in prepared:
        x, y, w, h = bbox
        overlap += int((body_union[y:y+h, x:x+w] & ink).sum())
        body_union[y:y+h, x:x+w] |= ink
        key = row['id'].replace(':', '__')
        with np.load(prediction_dir/(key+'.npz')) as file:
            pred, pr, raw = file['instances'], file['roles'], file['raw']
        assert not ((pred > 0) & ~ink).any() and not ((raw > 0) & ~ink).any()
        item = dict(id=row['id'], text=row['text'], line=row['line'], word=row['word'], subword=row['subword'],
                    bbox=bbox, complete=row['complete'],
                    referenceStatus=status, templates=row['templates'],
                    letters=row['letters'], trace=row.get('trace', []), results={}, images={})
        pictures = {'input': Image.fromarray(np.where(ink, 25, 255).astype('uint8')).convert('RGB'),
                    'template': colored(reference, ink, roles)}
        for suffix, labels in (('', pred), ('-raw', raw)):
            stats, mapping, changed, rc, unknown, tb, pb = compare_masks(reference, labels, roles, pr, ink)
            stats['priority'] = priority(stats, status)
            stats['metrics'] = summarize([metric(reference, labels, roles, pr)]) if status['valid'] else None
            item['results']['raw' if suffix else 'completed'] = stats
            pictures['model'+suffix] = colored(labels, ink, pr, mapping)
            pictures['difference'+suffix] = difference_image(ink, changed, rc, unknown)
            pictures['cuts'+suffix] = cut_image(ink, tb, pb)
            if status['valid']:
                (raw_metrics if suffix else metrics).append(metric(reference, labels, roles, pr))
        for name, image in pictures.items():
            filename = f'assets/{key}-{name}.png'
            image.save(output/filename)
            item['images'][name] = filename
            paste_ink(pages[name], image, bbox, ink)
        rows.append(item)
    if overlap:
        raise ValueError(f'Overlapping source subwords: {overlap} pixels; cannot silently composite them')
    for name, image in pages.items():
        Image.fromarray(image).save(assets/f'page-{name}.png')
    visible = context | body_union
    yy, xx = np.where(visible)
    view = [max(0, int(xx.min())-24), max(0, int(yy.min())-24),
            min(shape[1], int(xx.max())+25), min(shape[0], int(yy.max())+25)]
    x0, y0, x1, y1 = view
    # Export a shareable overview with all page lines at their original positions.
    overview = Image.new('RGB', ((x1-x0)*3, y1-y0), 'white')
    for i, name in enumerate(('template', 'model', 'difference')):
        overview.paste(Image.fromarray(pages[name]).crop(view), (i*(x1-x0), 0))
    overview.save(output/'page-comparison.png')
    summary = dict(page=page, examples=len(rows), exportedExamples=len(document['examples']), excludedExamples=excluded,
        templateComplete=sum(r['complete'] for r in rows),
        comparableExamples=len(metrics), templateRasterIssues=sum(bool(r['referenceStatus']['rasterError']) for r in rows),
        counts=dict(Counter(r['results']['completed']['priority'] for r in rows)),
        rawCounts=dict(Counter(r['results']['raw']['priority'] for r in rows)),
        agreement=summarize(metrics), rawAgreement=summarize(raw_metrics),
        sourceBodyPixels=int(body_union.sum()), contextPixels=int((context & ~body_union).sum()),
        scopeVersion=scope_version, sourceOverlapPixels=overlap, pageSplit=page_info['split'],
        templateSha256=document['templateSha256'], sourceSha256=document['sourceSha256'],
        modelSha256=model_hash, frozenThreshold=settings['threshold'], trainingStep=settings['step'],
        inference=cached, templateTrackerEnabled=config.get('kashidaDefaults', {}).get('trackingEnabled'),
        noTranscriptAtInference=True, sourceArtworkModified=False, humanAnnotationsModified=False,
        caveat='Inference is per subword body. Gray dots and marks show context. Agreement is scored on complete, raster-valid template references.')
    payload = dict(summary=summary, width=shape[1], height=shape[0], viewBox=[x0, y0, x1-x0, y1-y0], rows=rows)
    (output/'comparison.json').write_text(json.dumps(payload, ensure_ascii=False, indent=2)+'\n')
    (output/'comparison-data.js').write_text('window.PAGE_COMPARISON = '+json.dumps(payload, ensure_ascii=False, separators=(',', ':')).replace('<', '\\u003c')+';\n')
    for source, target in [('letter-page-review.html', 'index.html'), ('letter-page-review.css', 'review.css'), ('letter-page-review.mjs', 'review.mjs')]:
        asset = Path(__file__).parent/'app'/source
        if target == 'index.html':
            benchmark = escape(os.path.relpath(folder/'report.html', output), quote=True)
            (output/target).write_text(asset.read_text().replace('href="../report.html"', f'href="{benchmark}"'))
        else:
            shutil.copyfile(asset, output/target)
    (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, required=True)
    parser.add_argument('--page', default='page600')
    parser.add_argument('--data', type=Path, default=Path('data'))
    parser.add_argument('--native', type=Path, default=NATIVE)
    parser.add_argument('--device', default='mps')
    parser.add_argument('--vector-dir', type=Path, help='A fresh template export; leaves the training snapshot intact')
    parser.add_argument('--output', type=Path, help='Separate review output for revised templates')
    parser.add_argument('--reuse-frozen-predictions', action='store_true',
                        help='Reuse the original page predictions after verifying unchanged model inputs')
    main(parser.parse_args())
