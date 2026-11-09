"""Predict connected subwords and serve the Mask2Former · Cutter comparison."""
from collections import defaultdict
from contextlib import closing, nullcontext
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import sqlite3
import time
from urllib.parse import parse_qs, urlsplit

from workspace import HERE, WORKBENCH, read, run_path

import numpy as np

from data import base_count, connected_rows, document, generation_for, identifier, load_page, matching_audit, open_audit
from letter_corpus_audit import (
    SCHEMA, batch_probabilities, comparison_image, corpus_summary, prepare_example, save_example, search,
    source_records, write_json,
)
from letter_page_compare import inference_example
from letter_retest import decoder_options, mask_decode, model_for
from letter_template_repeat import sha
from seen import layout, measure, summarize

CONNECTED = "json_extract(detail,'$.expectedBodies') >= 2"


def checkpoint(folder):
    model = folder / 'mask2former'
    selected = model / 'selected.json'
    if selected.is_file():
        result = read(selected)
        return model / result['weights'], result['settings']
    if (folder / 'run.json').is_file() and 'scope' in read(folder / 'run.json'):
        raise ValueError('The run has no selected checkpoint. Finish the training task first.')
    return model / 'best.pt', read(model / 'settings.json')


def cached_example(db, row, source):
    sid = identifier(source['page'], row)
    saved = db.execute('SELECT detail,masks FROM examples WHERE id=?', (sid,)).fetchone()
    if saved is None:
        return None
    detail = json.loads(saved[0])
    if any(detail[key] != row[key] for key in ('text', 'templates', 'letters')):
        return None
    with np.load(io.BytesIO(saved[1])) as masks:
        ink, reference, roles = (masks[name] for name in ('ink', 'reference', 'roles'))
    vector = dict(id=sid)
    example = inference_example(vector, ink)
    example['record']['pdfToRaster'] = source['pdfToRaster']
    return dict(row=row, vector=vector, ink=ink, bbox=detail['bbox'], reference=reference, roles=roles,
                status=detail['referenceStatus'], example=example)


def previous_reference(run):
    previous = run_path(run['previous']) if run['previous'] else None
    return matching_audit(previous, run['generationSha256'], run.get('rasterMetadataSha256'))


def predict(folder, device_name, batch_size, page_numbers=None):
    import torch

    if batch_size <= 0:
        raise ValueError('Batch size must be positive')
    started = time.monotonic()
    run = read(folder / 'run.json')
    dataset_path = folder / 'dataset.json'
    weights, settings = checkpoint(folder)
    if sha(dataset_path) != run['datasetSha256'] or sha(dataset_path) != settings['datasetSha256']:
        raise ValueError('The training dataset changed')
    if sha(folder / 'inputs' / 'corpus.json') != run['corpusSha256']:
        raise ValueError('The frozen split changed')
    snapshot = Path(run['snapshot'])
    generation = generation_for(snapshot)
    if sha(snapshot / 'generation.json') != run['generationSha256']:
        raise ValueError('The native snapshot changed')
    pages = {p['id']: p for p in read(folder / 'inputs' / 'corpus.json')['pages']}
    training = {r['id'] for r in read(dataset_path)['records'] if r['split'] == 'train'}
    numbers = sorted(set(page_numbers)) if page_numbers is not None else list(range(1, 605))
    if not numbers or numbers[0] < 1 or numbers[-1] > 604:
        raise ValueError('Choose page numbers from 1 to 604')
    output = folder / ('corpus-audit-smoke' if page_numbers is not None else 'corpus-audit')
    output.mkdir(exist_ok=True)
    reference = previous_reference(run)
    before = output / 'baseline-comparison.sqlite'
    if reference and not before.exists():
        os.link(reference, before)
    signature = dict(weights=sha(weights), settings=settings, snapshot=run['generationSha256'], pages=numbers,
                     scope='connected-only', device=device_name, batch=batch_size,
                     code={p.name: sha(p) for p in (
                         Path(__file__), HERE / 'data.py', WORKBENCH / 'letter_corpus_audit.py',
                         WORKBENCH / 'letter_page_compare.py', WORKBENCH / 'letter_retest.py',
                         WORKBENCH / 'letter_template_repeat.py', WORKBENCH / 'letter_retest_data.py')})
    stamp = output / 'signature.json'
    if stamp.exists() and read(stamp) != signature:
        raise ValueError('Inference inputs changed. Use a new run to preserve the existing predictions.')
    write_json(stamp, signature)

    device = torch.device(device_name)
    torch.set_num_threads(4)
    model = model_for('mask2former', str(folder / 'pretrained'), device,
                      seen_body_class=settings.get('seenBodyClass', False),
                      geometry_input=settings.get('geometryInput', False))
    model.load_state_dict(torch.load(weights, map_location='cpu', weights_only=True))
    model.eval()
    old = closing(open_audit(before)) if before.exists() else nullcontext()
    with old as previous, closing(sqlite3.connect(output / 'comparison.sqlite')) as db, torch.inference_mode():
        db.executescript(SCHEMA)
        processed = {n: json.loads(receipt) for n, receipt in db.execute('SELECT page,receipt FROM pages')}
        for number in numbers:
            if number in processed and not processed[number]['failures']:
                continue
            page = f'page{number:03}'
            cache = load_page(snapshot, generation, number)
            source = document(page, cache, pages[page])
            rows = connected_rows(cache)
            skipped = sum(base_count(row) == 1 for row in source_records(cache))
            started_page = time.monotonic()
            missing = []
            prepared = []
            for index, row in enumerate(rows):
                sid = identifier(page, row)
                try:
                    item = cached_example(previous, row, source) if previous is not None else None
                    if item is None:
                        item = prepare_example(row, source)
                    elif index == 0:
                        fresh = prepare_example(row, source)
                        if item['bbox'] != fresh['bbox'] or any(
                                not np.array_equal(item[k], fresh[k]) for k in ('ink', 'reference', 'roles')):
                            raise ValueError(f'Cached reference differs for {sid}')
                    prepared.append(item)
                except (ValueError, KeyError, AssertionError) as error:
                    missing.append((sid, number, str(error)))
            with db:
                if number in processed:
                    db.execute('DELETE FROM examples WHERE page=?', (number,))
                    db.execute('DELETE FROM failures WHERE page=?', (number,))
                    db.execute('DELETE FROM pages WHERE page=?', (number,))
                for offset in range(0, len(prepared), batch_size):
                    batch = prepared[offset:offset + batch_size]
                    probabilities = batch_probabilities(model, batch, device, settings['threshold'],
                                                        settings.get('canvas'))
                    for item, (mask, classes) in zip(batch, probabilities, strict=True):
                        pred, roles, raw = mask_decode(mask, classes, item['ink'], settings['threshold'],
                            **decoder_options(settings.get('decoder') or 3, item['example']['record']))
                        save_example(db, item, pred, roles, raw, source, pages[page]['split'], training)
                db.executemany('INSERT INTO failures VALUES (?,?,?)', missing)
                receipt = dict(expected=len(rows), examples=len(prepared), failures=len(missing),
                               singleLetterPassthrough=skipped, seconds=time.monotonic() - started_page)
                db.execute('INSERT INTO pages VALUES (?,?)', (number, json.dumps(receipt)))
            processed[number] = receipt
            if len(processed) % 10 == 0 or missing:
                progress = dict(pages=len(processed), examples=sum(p['examples'] for p in processed.values()),
                                failures=sum(p['failures'] for p in processed.values()),
                                elapsedSeconds=sum(p['seconds'] for p in processed.values()),
                                singleLetterPassthrough=sum(p['singleLetterPassthrough'] for p in processed.values()))
                write_json(output / 'progress.json', progress)
                print(f"{progress['pages']}/{len(numbers)} pages, {progress['examples']} connected predictions", flush=True)
        summary = dict(corpus_summary(db), connectedOnly=True,
                       singleLetterPassthrough=sum(p['singleLetterPassthrough'] for p in processed.values()),
                       expectedExamples=sum(p['expected'] for p in processed.values()),
                       elapsedSeconds=sum(p['seconds'] for p in processed.values()),
                       invocationSeconds=time.monotonic() - started, device=device_name, batch=batch_size,
                       checkpointStep=settings['step'], sourceInkOnlyAtInference=True)
        if summary['pages'] != len(numbers) or summary['examples'] + summary['failures'] != summary['expectedExamples']:
            raise ValueError('The audit inventory does not account for every connected crop')
        if page_numbers is None and (summary['expectedExamples'] != run['scope']['connected'] or
                summary['singleLetterPassthrough'] != run['scope']['singleLetterPassthrough']):
            raise ValueError('The audit inventory differs from the prepared scope')
        write_json(output / 'summary.json', summary)
        if summary['failures']:
            raise ValueError(f"{summary['failures']} crops failed preparation. See the audit failures.")
    compare(output)
    return summary


def compare(folder):
    """Record local seen/teeth errors and pair prior results on the same cuts."""
    cohorts = {'current': defaultdict(list), 'previous': defaultdict(list)}
    fragments, teeth = {}, {}
    regressions = {}
    with closing(open_audit(folder / 'comparison.sqlite')) as db:
        prior = (folder / 'baseline-comparison.sqlite').is_file()
        if prior:
            db.execute('ATTACH DATABASE ? AS previous', ((folder / 'baseline-comparison.sqlite').resolve().as_uri() + '?mode=ro',))
            regressions = {sid: f'Previous {a:.1%}, current {b:.1%} ink differs'
                           for sid, a, b in db.execute("""SELECT n.id, o.difference, n.difference
                FROM examples n JOIN previous.examples o USING(id)
                WHERE n.split!='train' AND n.reference='complete' AND n.difference>o.difference
                ORDER BY n.difference-o.difference DESC,n.id LIMIT 100""")}
        for sid, blob in db.execute("SELECT id,detail FROM examples WHERE reference='complete' ORDER BY id"):
            detail = json.loads(blob)
            letters = detail['letters']
            if not any(layout(letters)):
                continue
            def measure_row(schema):
                saved = db.execute(f'SELECT masks FROM {schema}.examples WHERE id=?', (sid,)).fetchone()
                if saved:
                    with np.load(io.BytesIO(saved[0])) as masks:
                        return measure(letters, masks['reference'], masks['prediction'], masks['predicted_roles'])
            stats = measure_row('main')
            split = detail['split']
            cohorts['current'][split].append(stats)
            if prior:
                old = measure_row('previous')
                if old:
                    cohorts['previous'][split].append(old)
            if split != 'train' and any(b['strayBodies'] for b in stats['seen']):
                fragments[sid] = f"{stats['fragments']} substantial fragments. Inspect any small stray bodies on the same letter."
            if split != 'train' and any(t['merged'] for t in stats['triples']):
                teeth[sid] = 'A predicted body covers two or more of the three genuine teeth.'
    write_json(folder / 'seen-and-teeth.json',
               {mode: {split: summarize(rows) for split, rows in splits.items()} for mode, splits in cohorts.items()})
    def batch(title, notes):
        return dict(title=title, ids=list(notes), notes=notes)
    write_json(folder / 'review-batches.json', {
        'seen-fragments': batch('Held-out sīn and shīn fragments', fragments),
        'merged-teeth': batch('Held-out merged teeth', teeth),
        'largest-new-disagreements': batch('Largest new held-out disagreements', regressions),
    })


def connected_summary(folder, db):
    path = folder / 'summary.json'
    original = read(path if path.exists() else folder / 'progress.json')
    if original.get('connectedOnly') or 'singleLetterPassthrough' in original:
        return original
    examples, words, errors, training = db.execute(f"""SELECT COUNT(*),
        COUNT(DISTINCT substr(id,1,instr(id,'-s')-1)), COALESCE(SUM(count_error>0),0),
        COALESCE(SUM(training),0) FROM examples WHERE {CONNECTED}""").fetchone()
    return dict(original, connectedOnly=True, examples=examples, expectedSubwords=examples, words=words,
                bodyCountDifferences=errors, usedForTraining=training,
                singleLetterPassthrough=original['examples'] - examples, filteredArchive=True,
                bySplit=dict(db.execute(f'SELECT split,COUNT(*) FROM examples WHERE {CONNECTED} GROUP BY split')),
                byReference=dict(db.execute(f'SELECT reference,COUNT(*) FROM examples WHERE {CONNECTED} GROUP BY reference')))


def serve(run, port):
    folder = run / 'corpus-audit'
    database = folder / 'comparison.sqlite'
    if not database.is_file():
        raise ValueError(f'Run connected prediction first. Missing {database}')
    with closing(open_audit(database)) as db:
        summary = connected_summary(folder, db)
    before = folder / 'baseline-comparison.sqlite'
    assets = {'/': ('index.html', 'text/html'), '/index.html': ('index.html', 'text/html'),
              '/audit.css': ('audit.css', 'text/css'), '/audit.mjs': ('audit.mjs', 'text/javascript')}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            route = urlsplit(self.path)
            params = parse_qs(route.query)
            try:
                if route.path in assets:
                    file, mime = assets[route.path]
                    content = (HERE / 'web' / file).read_bytes()
                elif route.path == '/changed-cuts.html' and (folder / 'changed-cuts.html').is_file():
                    content = (folder / 'changed-cuts.html').read_bytes()
                    mime = 'text/html; charset=utf-8'
                else:
                    mime = 'application/json'
                    if route.path == '/api/summary':
                        result = summary
                    elif route.path == '/api/timings':
                        result = {k: v for k, v in summary.items() if k in
                                  ('timing', 'elapsedSeconds', 'invocationSeconds', 'examplesPerSecond', 'device', 'batch')}
                    else:
                        batch_path = folder / 'review-batches.json'
                        batches = read(batch_path) if batch_path.exists() else {}
                        use_before = params.get('before') == ['1'] and before.exists()
                        with closing(open_audit(before if use_before else database)) as db:
                            if route.path == '/api/review-batches':
                                # Older reports may have single letters in their review batches.
                                for batch in batches.values():
                                    batch['ids'] = [sid for sid in batch['ids'] if db.execute(
                                        f'SELECT 1 FROM examples WHERE id=? AND {CONNECTED}', (sid,)).fetchone()]
                                result = dict(batches=batches, beforeAvailable=before.exists())
                            elif route.path == '/api/search':
                                params['bases'] = ['multi']
                                key = params.get('reviewBatch', ['all'])[0]
                                batch = batches[key] if key != 'all' else {}
                                result = search(db, params, batch.get('ids'))
                                for row in result['rows']:
                                    if note := batch.get('notes', {}).get(row['id']):
                                        row['reviewNote'] = note
                            elif route.path == '/api/image':
                                sid = params.get('id', [''])[0]
                                if not db.execute(f'SELECT 1 FROM examples WHERE id=? AND {CONNECTED}', (sid,)).fetchone():
                                    raise KeyError(sid)
                                content = comparison_image(db, sid, params.get('raw') == ['1'])
                                mime = 'image/png'
                            elif route.path == '/api/failures':
                                result = [dict(id=sid, page=page, reason=reason)
                                          for sid, page, reason in db.execute('SELECT * FROM failures')]
                            else:
                                raise KeyError(route.path)
                    if mime == 'application/json':
                        content = json.dumps(result, ensure_ascii=False).encode()
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

        def log_message(self, *_):
            pass

    with ThreadingHTTPServer(('127.0.0.1', port), Handler) as server:
        print(f'Mask2Former · Cutter comparison at http://127.0.0.1:{server.server_port}/', flush=True)
        server.serve_forever()
