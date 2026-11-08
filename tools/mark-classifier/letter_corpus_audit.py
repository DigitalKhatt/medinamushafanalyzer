"""Infer every native subword and serve the largest cutter/model disagreements."""
from workbench_paths import NATIVE
import argparse
from collections import Counter
from contextlib import closing
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import sqlite3
import time
from urllib.parse import parse_qs, urlsplit

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi

from letter_bases import base_character_count
from letter_page_compare import compare_masks, difference_image, inference_example, reference_crop, source_crop
from letter_retest import CANVAS, decoder_options, inputs, mask_decode, model_for, native, resize_example
from letter_retest_report import colored
from letter_template_repeat import vector_row


HERE = Path(__file__).resolve().parent
SCHEMA = """
CREATE TABLE IF NOT EXISTS pages (page INTEGER PRIMARY KEY, receipt TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS examples (
    id TEXT PRIMARY KEY, page INTEGER NOT NULL, text TEXT NOT NULL, word_text TEXT NOT NULL,
    split TEXT NOT NULL, training INTEGER NOT NULL, reference TEXT NOT NULL,
    difference REAL, raw_difference REAL, max_error REAL, boundary_distance REAL,
    count_error INTEGER NOT NULL, templates TEXT NOT NULL, detail TEXT NOT NULL, masks BLOB NOT NULL
);
CREATE INDEX IF NOT EXISTS example_difference ON examples(difference DESC,id);
CREATE INDEX IF NOT EXISTS example_raw_difference ON examples(raw_difference DESC,id);
CREATE INDEX IF NOT EXISTS example_max_error ON examples(max_error DESC,id);
CREATE INDEX IF NOT EXISTS example_page ON examples(page,id);
CREATE INDEX IF NOT EXISTS example_cut_difference ON examples(difference DESC,id)
    WHERE json_extract(detail,'$.expectedBodies')>1;
CREATE TABLE IF NOT EXISTS failures (id TEXT PRIMARY KEY, page INTEGER, reason TEXT NOT NULL);
"""


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    temporary.replace(path)


def disagreement(reference, prediction, roles, predicted_roles, ink, pen_width):
    stats, mapping, changed, rc, unknown, tb, pb = compare_masks(
        reference, prediction, roles, predicted_roles, ink)
    stats['differenceFraction'] = float((changed | unknown).sum() / max(1, ink.sum()))
    body_ious = []
    for label in np.unique(reference[reference > 0]):
        if roles[label] != 1:
            continue
        matches = [other for other, target in mapping.items()
                   if target == label and predicted_roles[other] == 1]
        actual = np.isin(prediction, matches) if matches else np.zeros_like(ink)
        expected = reference == label
        body_ious.append(float((actual & expected).sum() / max(1, (actual | expected).sum())))
    stats['worstBodyDifference'] = 1 - min(body_ious, default=0)
    if tb.any() and pb.any():
        distances = np.concatenate((ndi.distance_transform_edt(~pb)[tb],
                                    ndi.distance_transform_edt(~tb)[pb]))
        stats['boundaryP95PenWidths'] = float(np.quantile(distances, .95) / pen_width)
    else:
        stats['boundaryP95PenWidths'] = None
    return stats


def source_records(cache):
    lines = {row['line']: row for row in cache['sourceLines']}
    words = {(row['line'], row['word']) for row in cache['sourceWords'] if row.get('inTranscript', True)}
    return [row for row in cache['subwords']
            if lines.get(row['line'], {}).get('transcriptWords', 0)
            and (row['line'], row['word']) in words
            and base_character_count(row['text'])]


def prepare_example(row, document):
    # Source ink is also exported for partial matches with missing body masks.
    vector = vector_row(row, document)
    ink, bbox = source_crop(vector, document['raster']['width'])
    reference, roles, status = reference_crop(vector, document['raster']['width'], ink, bbox)
    status['valid'] = bool((row['complete'] or row.get('provisional')) and not status['rasterError'])
    example = inference_example(vector, ink)
    example['record']['pdfToRaster'] = document['pdfToRaster']
    return dict(row=row, vector=vector, ink=ink, bbox=bbox, reference=reference,
                roles=roles, status=status, example=example)


def document_for(cache, raw):
    if raw['provenance']['cacheSha256'] != cache['sourceFileSha256']:
        raise ValueError('The source raster metadata belongs to a different native page')
    return dict(page=f"page{cache['page']:03}", raster=raw['raster'],
                pdfToRaster=raw['provenance']['pdfToRaster'],
                sourceSha256=raw['sourceSha256'], sourceCacheSha256=cache['sourceFileSha256'],
                templateSha256=cache['letterFormsSha256'])


def encode_masks(item, prediction, predicted_roles, raw):
    buffer = io.BytesIO()
    np.savez_compressed(buffer, ink=item['ink'], reference=item['reference'], roles=item['roles'],
                        prediction=prediction, predicted_roles=predicted_roles, raw=raw)
    return buffer.getvalue()


def batch_probabilities(model, batch, device, threshold, canvas_shape=None):
    """Queue mask resizing before transferring all selected queries together."""
    import torch
    examples = [resize_example(item['example'], canvas_shape) for item in batch]
    canvases = torch.stack([example['canvas'] for example in examples])
    result = model(pixel_values=inputs(canvases, device))
    classes = result.class_queries_logits.softmax(-1).cpu().numpy()
    resized, shapes, selected_classes = [], [], []
    for index, item in enumerate(batch):
        query_classes = classes[index]
        keep = ((query_classes[:, :-1].max(1) >= threshold) &
                (query_classes.argmax(1) != query_classes.shape[1] - 1))
        selected_classes.append(classes[index][keep])
        if not keep.any():
            shapes.append((0, *item['ink'].shape))
            continue
        indices = torch.as_tensor(np.flatnonzero(keep), dtype=torch.long, device=device)
        logits = result.masks_queries_logits[index].index_select(0, indices)
        probabilities = native(logits, examples[index]).sigmoid()
        shapes.append(tuple(probabilities.shape))
        resized.append(probabilities.reshape(-1))
    packed = torch.cat(resized).cpu().numpy() if resized else np.empty(0, 'float32')
    offset, outputs = 0, []
    for shape, query_classes in zip(shapes, selected_classes):
        size = int(np.prod(shape))
        outputs.append((packed[offset:offset + size].reshape(shape), query_classes))
        offset += size
    return outputs


def save_example(db, item, prediction, predicted_roles, raw, document, split, training_ids):
    vector, row = item['vector'], item['row']
    ink, reference, roles = item['ink'], item['reference'], item['roles']
    if ((prediction > 0) & ~ink).any() or ((raw > 0) & ~ink).any():
        raise ValueError('Prediction contains pixels outside the source ink')
    if prediction.any() and not np.array_equal(prediction > 0, ink):
        raise ValueError('Completed predictions must account for all source ink')
    transform = document['pdfToRaster']
    pen_width = .5 * np.sqrt(abs(transform[0] * transform[3] - transform[1] * transform[2]))
    completed = disagreement(reference, prediction, roles, predicted_roles, ink, pen_width)
    original = disagreement(reference, raw, roles, predicted_roles, ink, pen_width)
    status = item['status']
    state = ('provisional' if row.get('provisional') else 'complete') if status['valid'] else 'partial'
    score = completed['differenceFraction'] if status['valid'] else None
    raw_score = original['differenceFraction'] if status['valid'] else None
    worst = completed['worstBodyDifference'] if status['valid'] else None
    distance = completed['boundaryP95PenWidths'] if status['valid'] else None
    expected = base_character_count(row['text'])
    detail = dict(id=vector['id'], text=row['text'], wordText=row['wordText'],
                  page=int(document['page'][4:]), line=row['line'], word=row['word'], subword=row['subword'],
                  expectedBodies=expected, split=split, usedForTraining=vector['id'] in training_ids,
                  reference=state, referenceStatus=status, templates=row['templates'], letters=row['letters'],
                  bbox=item['bbox'], completed=completed, raw=original)
    db.execute('INSERT INTO examples VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
               (vector['id'], detail['page'], row['text'], row['wordText'], split,
                int(detail['usedForTraining']), state, score, raw_score, worst, distance,
                abs(completed['modelBodies'] - expected), json.dumps(row['templates']),
                json.dumps(detail, ensure_ascii=False), encode_masks(item, prediction, predicted_roles, raw)))
    return detail


def corpus_summary(db):
    return dict(
        examples=db.execute('SELECT COUNT(*) FROM examples').fetchone()[0],
        pages=db.execute('SELECT COUNT(*) FROM pages').fetchone()[0],
        failures=db.execute('SELECT COUNT(*) FROM failures').fetchone()[0],
        byReference=dict(db.execute('SELECT reference,COUNT(*) FROM examples GROUP BY reference')),
        bySplit=dict(db.execute('SELECT split,COUNT(*) FROM examples GROUP BY split')),
        usedForTraining=db.execute('SELECT COUNT(*) FROM examples WHERE training=1').fetchone()[0],
        bodyCountDifferences=db.execute('SELECT COUNT(*) FROM examples WHERE count_error>0').fetchone()[0],
        words=db.execute("SELECT COUNT(DISTINCT substr(id,1,instr(id,'-s')-1)) FROM examples").fetchone()[0],
    )


def infer(args):
    import torch
    torch.set_num_threads(args.threads)
    started = time.perf_counter()
    folder, snapshot = args.folder.resolve(), args.snapshot.resolve()
    output = folder / ('corpus-audit-smoke' if args.smoke else 'corpus-audit')
    output.mkdir(exist_ok=True)
    generation = json.loads((snapshot / 'generation.json').read_text())
    if generation['pagesCompleted'] != 604 or generation['failures']:
        raise ValueError('The frozen native snapshot must contain every page')
    manifest = json.loads((folder / 'dataset.json').read_text())
    settings = json.loads((folder / 'mask2former/settings.json').read_text())
    if settings['datasetSha256'] != digest(folder / 'dataset.json'):
        raise ValueError('The trained dataset changed')
    signature = dict(model=digest(folder / 'mask2former/best.pt'), snapshot=digest(snapshot / 'generation.json'),
                     code=digest(__file__), decoder=digest(HERE / 'letter_retest.py'),
                     comparison=digest(HERE / 'letter_page_compare.py'),
                     rasterization=digest(HERE / 'letter_template_repeat.py'),
                     reference=digest(HERE / 'letter_retest_data.py'),
                     settings=digest(folder / 'mask2former/settings.json'),
                     threshold=settings['threshold'], canvas=settings.get('canvas', list(CANVAS)))
    signature_file = output / 'signature.json'
    if signature_file.exists() and json.loads(signature_file.read_text()) != signature:
        raise ValueError('Frozen audit inputs changed; use a separate output directory')
    write_json(signature_file, signature)
    clock_file = output / 'clock.json'
    if not clock_file.exists():
        write_json(clock_file, dict(startedAtUnix=time.time()))
    wall_started = json.loads(clock_file.read_text())['startedAtUnix']
    splits = {row['id']: row['split'] for row in json.loads((folder / 'inputs/corpus.json').read_text())['pages']}
    training_ids = {row['id'] for row in manifest['records'] if row['split'] == 'train'}
    inventory_file = folder / 'scope-inventory.json'
    inventory = json.loads(inventory_file.read_text()) if inventory_file.exists() and not args.smoke else {}
    scope = dict(expectedSubwords=inventory.get('totalSubwords'), expectedWords=inventory.get('totalWords'))
    device = torch.device(args.device)
    model = model_for('mask2former', str(folder / 'pretrained'), device,
                      seen_body_class=settings.get('seenBodyClass', False),
                      geometry_input=settings.get('geometryInput', False))
    model.load_state_dict(torch.load(folder / 'mask2former/best.pt', map_location='cpu', weights_only=True))
    model.eval()
    timings = Counter(preparationSeconds=0., modelSeconds=0., decodingAndComparisonSeconds=0.)
    expected_total = 0
    with closing(sqlite3.connect(output / 'comparison.sqlite')) as db:
        db.executescript(SCHEMA)
        progress = corpus_summary(db)
        for receipt, in db.execute('SELECT receipt FROM pages'):
            timings.update(json.loads(receipt).get('timing', {}))
        page_numbers = [3, 4, 600] if args.smoke else list(range(1, 605))
        for page in page_numbers:
            page_started = time.perf_counter()
            path = snapshot / 'pages' / f'page{page:03}.json'
            if generation['pageHashes'][str(page)]['export'] != digest(path):
                raise ValueError(f'Changed frozen page {page}')
            cache = json.loads(path.read_bytes())
            rows = source_records(cache)
            expected_total += len(rows)
            existing = db.execute('SELECT receipt FROM pages WHERE page=?', (page,)).fetchone()
            if existing:
                expected_total += json.loads(existing[0]).get('missingWords', 0)
                continue
            raw_path = NATIVE / 'output/vector-review' / f'page{page:03}.raw.json'
            document = document_for(cache, json.loads(raw_path.read_bytes()))
            prepared, failures = [], []
            word_keys = {(row['line'], row['word']) for row in rows}
            in_scope_lines = {row['line'] for row in cache['sourceLines'] if row['transcriptWords']}
            missing_words = [row for row in cache['sourceWords']
                             if row.get('inTranscript', True) and row['line'] in in_scope_lines
                             and base_character_count(row['text'])
                             and (row['line'], row['word']) not in word_keys]
            failures.extend((document['page'] + f":l{row['line']}-w{row['word']}-s0", page,
                             'The native page has no subword source ink for this word') for row in missing_words)
            expected_total += len(missing_words)
            previous_timing = timings.copy()
            reference_counts = Counter()
            training_count = 0
            count_differences = 0
            t = time.perf_counter()
            for row in rows:
                sid = document['page'] + f":l{row['line']}-w{row['word']}-s{row['subword']}"
                try:
                    prepared.append(prepare_example(row, document))
                except (ValueError, KeyError, AssertionError) as error:
                    failures.append((sid, page, str(error)))
            timings['preparationSeconds'] += time.perf_counter() - t
            with torch.inference_mode(), db:
                for offset in range(0, len(prepared), args.batch):
                    batch = prepared[offset:offset + args.batch]
                    t = time.perf_counter()
                    probabilities = batch_probabilities(model, batch, device, settings['threshold'],
                                                        settings.get('canvas'))
                    timings['modelSeconds'] += time.perf_counter() - t
                    t = time.perf_counter()
                    for item, (mask_probs, query_classes) in zip(batch, probabilities):
                        prediction, roles, raw = mask_decode(
                            mask_probs, query_classes, item['ink'], settings['threshold'],
                            **decoder_options(settings.get('decoder') or 3, item['example']['record']))
                        detail = save_example(db, item, prediction, roles, raw, document, splits[document['page']], training_ids)
                        reference_counts[detail['reference']] += 1
                        training_count += detail['usedForTraining']
                        count_differences += detail['completed']['modelBodies'] != detail['expectedBodies']
                    timings['decodingAndComparisonSeconds'] += time.perf_counter() - t
                db.executemany('INSERT INTO failures VALUES (?,?,?)', failures)
                receipt = dict(examples=len(prepared), expected=len(rows) + len(missing_words),
                               missingWords=len(missing_words), failures=len(failures),
                               elapsedSeconds=time.perf_counter() - page_started,
                               timing={key: value - previous_timing[key] for key, value in timings.items()})
                db.execute('INSERT INTO pages VALUES (?,?)', (page, json.dumps(receipt)))
            progress['examples'] += len(prepared)
            progress['pages'] += 1
            progress['words'] += len(word_keys)
            progress['failures'] += len(failures)
            progress['usedForTraining'] += training_count
            progress['bodyCountDifferences'] += count_differences
            for state, count in reference_counts.items():
                progress['byReference'][state] = progress['byReference'].get(state, 0) + count
            split = splits[document['page']]
            progress['bySplit'][split] = progress['bySplit'].get(split, 0) + len(prepared)
            summary = dict(progress, **scope, expectedExamples=expected_total, elapsedSeconds=time.time() - wall_started,
                           timing=dict(timings), device=str(device), batch=args.batch,
                           sourceInkOnlyAtInference=True, checkpointStep=settings['step'])
            write_json(output / 'progress.json', summary)
            if page % 10 == 0 or failures:
                print(json.dumps(summary), flush=True)
        summary = dict(corpus_summary(db), **scope, expectedExamples=expected_total,
                       elapsedSeconds=time.time() - wall_started, timing=dict(timings),
                       invocationSeconds=time.perf_counter() - started,
                       activePageSeconds=sum(json.loads(row[0])['elapsedSeconds']
                                             for row in db.execute('SELECT receipt FROM pages')),
                       device=str(device), batch=args.batch, checkpointStep=settings['step'],
                       sourceInkOnlyAtInference=True,
                       interpretation='Differences compare model predictions with automatic cutter masks.',
                       penWidthPageUnits=.5)
        if summary['examples'] + summary['failures'] != expected_total:
            raise ValueError('Some corpus examples are unaccounted for')
        if inventory and expected_total != inventory['totalSubwords'] + len(inventory['missingWords']):
            raise ValueError('The inferred corpus differs from its frozen word inventory')
        summary['examplesPerSecond'] = summary['examples'] / max(1, summary['elapsedSeconds'])
        write_json(output / 'summary.json', summary)
        print(json.dumps(summary, indent=2), flush=True)


SORTS = dict(difference='difference', raw='raw_difference', letter='max_error',
             boundary='boundary_distance', count='count_error',
             original="COALESCE(json_extract(detail,'$.originalCompleted.differenceFraction'),difference)")


def search(db, params, review_ids=None):
    clauses, values = [], []
    if review_ids is not None:
        if review_ids:
            clauses.append('id IN (' + ','.join('?' for _ in review_ids) + ')')
            values.extend(review_ids)
        else:
            clauses.append('0')
    for key, column in [('split', 'split'), ('reference', 'reference'), ('page', 'page')]:
        value = params.get(key, ['all'])[0]
        if value != 'all':
            clauses.append(column + '=?')
            values.append(value)
    training = params.get('training', ['all'])[0]
    if training in ('used', 'unused'):
        clauses.append('training=?')
        values.append(int(training == 'used'))
    bases = params.get('bases', ['all'])[0]
    if bases == 'multi':
        clauses.append("json_extract(detail,'$.expectedBodies')>1")
    elif bases == 'single':
        clauses.append("json_extract(detail,'$.expectedBodies')=1")
    query = params.get('q', [''])[0].strip()
    if query:
        clauses.append('(id LIKE ? OR text LIKE ? OR word_text LIKE ? OR templates LIKE ?)')
        values.extend(['%' + query + '%'] * 4)
    where = (' WHERE ' + ' AND '.join(clauses)) if clauses else ''
    limit = min(1000, max(1, int(params.get('limit', [100])[0])))
    offset = max(0, int(params.get('offset', [0])[0]))
    order = SORTS.get(params.get('sort', ['difference'])[0], 'difference')
    total = db.execute('SELECT COUNT(*) FROM examples' + where, values).fetchone()[0]
    rows = db.execute('SELECT detail FROM examples' + where + f' ORDER BY {order} DESC,difference DESC,id LIMIT ? OFFSET ?',
                      [*values, limit, offset]).fetchall()
    return dict(total=total, offset=offset, rows=[json.loads(row[0]) for row in rows])


def comparison_image(db, sid, raw=False):
    row = db.execute('SELECT masks FROM examples WHERE id=?', (sid,)).fetchone()
    if row is None:
        raise KeyError(sid)
    with np.load(io.BytesIO(row[0])) as data:
        ink, reference, roles = data['ink'], data['reference'], data['roles']
        prediction = data['raw' if raw else 'prediction']
        pr = data['predicted_roles']
        stats, mapping, changed, role_change, unknown, tb, pb = compare_masks(reference, prediction, roles, pr, ink)
        pictures = [colored(reference, ink, roles), colored(prediction, ink, pr, mapping),
                    difference_image(ink, changed, role_change, unknown)]
    width, height = pictures[0].size
    scale = min(3., 280 / max(1, width), 145 / max(1, height))
    size = (max(1, round(width * scale)), max(1, round(height * scale)))
    panel = Image.new('RGB', (3 * (size[0] + 12), size[1] + 27), 'white')
    draw = ImageDraw.Draw(panel)
    for index, (picture, title) in enumerate(zip(pictures, ['Cutter', 'Mask2Former', 'Difference'])):
        x = index * (size[0] + 12) + 6
        panel.paste(picture.resize(size, Image.Resampling.NEAREST), (x, 22))
        draw.text((x, 4), title, fill='#364350')
    buffer = io.BytesIO()
    panel.save(buffer, 'PNG')
    return buffer.getvalue()


def serve(args):
    folder = args.folder.resolve() / 'corpus-audit'
    assets = dict(html=HERE / 'app/letter-corpus-audit.html',
                  css=HERE / 'app/letter-corpus-audit.css',
                  js=HERE / 'app/letter-corpus-audit.mjs')
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                route = urlsplit(self.path)
                params = parse_qs(route.query)
                if route.path in ('/', '/index.html'):
                    content, mime = assets['html'].read_bytes(), 'text/html'
                elif route.path in ('/audit.css', '/audit.mjs'):
                    key = 'css' if route.path.endswith('css') else 'js'
                    content, mime = assets[key].read_bytes(), ('text/css' if key == 'css' else 'text/javascript')
                elif route.path == '/api/summary':
                    path = folder / 'summary.json'
                    content, mime = (path if path.exists() else folder / 'progress.json').read_bytes(), 'application/json'
                elif route.path == '/api/timings':
                    content, mime = (args.folder.resolve() / 'timings.json').read_bytes(), 'application/json'
                elif route.path == '/api/review-batches':
                    path = folder / 'review-batches.json'
                    batches = json.loads(path.read_bytes()) if path.exists() else {}
                    content, mime = json.dumps(dict(batches=batches,
                        beforeAvailable=(folder / 'baseline-comparison.sqlite').exists())).encode(), 'application/json'
                else:
                    before = params.get('before', ['0'])[0] == '1'
                    filename = 'baseline-comparison.sqlite' if before else 'comparison.sqlite'
                    with closing(sqlite3.connect((folder / filename).as_uri() + '?mode=ro', uri=True)) as db:
                        if route.path == '/api/search':
                            review_ids = None
                            batch = params.get('reviewBatch', ['all'])[0]
                            if batch != 'all':
                                path = folder / 'review-batches.json'
                                batches = json.loads(path.read_bytes()) if path.exists() else {}
                                if batch not in batches:
                                    raise ValueError('Unknown review batch')
                                review_ids = batches[batch]['ids']
                            result = search(db, params, review_ids)
                            if review_ids is not None:
                                for row in result['rows']:
                                    note = batches[batch].get('notes', {}).get(row['id'])
                                    if note:
                                        row['reviewNote'] = note
                            content, mime = json.dumps(result, ensure_ascii=False).encode(), 'application/json'
                        elif route.path == '/api/image':
                            content = comparison_image(db, params.get('id', [''])[0], params.get('raw', ['0'])[0] == '1')
                            mime = 'image/png'
                        elif route.path == '/api/failures':
                            content = json.dumps([dict(id=sid, page=page, reason=reason)
                                                  for sid, page, reason in db.execute('SELECT * FROM failures')],
                                                 ensure_ascii=False).encode()
                            mime = 'application/json'
                        else:
                            raise KeyError(route.path)
                self.send_response(200)
                self.send_header('Content-Type', mime)
                self.send_header('Content-Length', str(len(content)))
                self.send_header('Cache-Control', 'no-cache')
                self.end_headers()
                self.wfile.write(content)
            except (BrokenPipeError, ConnectionResetError):
                pass
            except KeyError as error:
                self.send_error(404, str(error))
            except (ValueError, sqlite3.Error) as error:
                self.send_error(400, str(error))

        def log_message(self, *_args):
            pass

    with ThreadingHTTPServer(('127.0.0.1', args.port), Handler) as server:
        print(f'Corpus cut comparison at http://127.0.0.1:{args.port}/', flush=True)
        server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['infer', 'serve'])
    parser.add_argument('--folder', type=Path, required=True)
    parser.add_argument('--snapshot', type=Path)
    parser.add_argument('--device', default='mps')
    parser.add_argument('--batch', type=int, default=8)
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--port', type=int, default=8790)
    parser.add_argument('--smoke', action='store_true', help='Benchmark three pages in a separate output')
    args = parser.parse_args()
    if args.action == 'infer':
        infer(args)
    else:
        serve(args)


if __name__ == '__main__':
    main()
