"""Inspect the next run's actual training targets and freeze the user's exclusions."""
import argparse
from collections import Counter
from contextlib import contextmanager
import fcntl
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import shutil
import tempfile
from urllib.parse import urlsplit

from segment import atomic_json, read_json
from letter_bases import check_training_records


TRAIN_GROUPS = {'weak_train', 'human_train'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Selection:
    def __init__(self, folder):
        self.folder = folder.resolve()
        self.manifest = read_json(self.folder / 'dataset.json')
        if self.manifest.get('trainingReview', {}).get('status') != 'open':
            raise ValueError('Choose a candidate dataset prepared with --review-training')
        self.fingerprint = digest(self.folder / 'dataset.json')
        if read_json(self.folder / 'run.json')['datasetSha256'] != self.fingerprint:
            raise ValueError('The candidate dataset differs from the prepared run')
        self.ledger = self.folder / 'training-exclusions.json'
        records = self.manifest['records']
        splits = {p['id']: p['split'] for p in read_json(self.folder / 'inputs/corpus.json')['pages']}
        if len({r['id'] for r in records}) != len(records) or any(
                r['split'] != splits[r['page']] or (r['split'] == 'train') != (r['group'] in TRAIN_GROUPS) for r in records):
            raise ValueError('Duplicate subword or page split mismatch')
        check_training_records(records)
        self.ids = {r['id'] for r in records if r['group'] in TRAIN_GROUPS}
        with self.lock():
            if not self.ledger.exists():
                atomic_json(self.ledger, dict(datasetSha256=self.fingerprint, revision=0, excluded=[]))
            self.read()

    @contextmanager
    def lock(self):
        with (self.folder / '.training-review.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    def read(self):
        state = read_json(self.ledger)
        if state['datasetSha256'] != self.fingerprint or set(state['excluded']) - self.ids:
            raise ValueError('Exclusions do not match the frozen training candidates')
        return state

    def update(self, request):
        key, excluded = request.get('id'), request.get('excluded')
        if request.get('datasetSha256') != self.fingerprint:
            raise ValueError('The training dataset has changed. Reload the grid.')
        if not isinstance(key, str) or key not in self.ids or type(excluded) is not bool:
            raise ValueError('Choose a training subword and an exclusion state')
        with self.lock():
            state = self.read()
            if state.get('frozenOutput'):
                raise ValueError('The training selection has been frozen')
            ids = set(state['excluded'])
            if (key in ids) != excluded:
                if excluded:
                    ids.add(key)
                else:
                    ids.remove(key)
                state.update(excluded=sorted(ids), revision=state['revision'] + 1)
                atomic_json(self.ledger, state)
            return state


def write_selected(folder, stage, manifest, state):
    from letter_template_repeat import tooth_policy

    excluded = set(state['excluded'])
    original = manifest['records']
    records = [r for r in original if r['id'] not in excluded]
    training = [r for r in records if r['group'] == 'weak_train']
    if not training:
        raise ValueError('At least one automatic training example is required by this sampler')
    counts = Counter(r['group'] for r in records)
    receipt = dict(status='frozen', candidateDatasetSha256=state['datasetSha256'],
                   revision=state['revision'], excluded=state['excluded'])
    tooth = read_json(folder / 'tooth-area.json')
    retained = [r for r in tooth['measurements'] if r['id'] not in excluded]
    decoder, stats = tooth_policy(retained, records)
    atomic_json(stage / 'tooth-area.json', dict(**decoder, analyzerAreaStatistics=stats, measurements=retained))

    coverage = read_json(folder / 'template-coverage.json')
    cases = Counter(key for r in training for key in r['templateCases'])
    for row in coverage['cases']:
        key = '|'.join(row[k] for k in ('template', 'position', 'character'))
        if row['selected']['train'] and not cases[key]:
            row['status'] = 'excluded-in-training-review'
        row['selected']['train'] = cases[key]
    absent = sorted({r['template'] for r in coverage['cases']} -
                    {r['template'] for r in coverage['cases'] if r['selected']['train']})
    coverage['rulesWithoutCompleteTrainingExample'] = absent
    atomic_json(stage / 'template-coverage.json', coverage)
    prep = read_json(folder / 'preparation.json')
    prep.update(groups=counts, rulesWithTraining=prep['rules'] - len(absent),
                rulesWithoutCompleteTrainingExample=absent,
                selectedCases=sum(r['status'] == 'covered' for r in coverage['cases']), decoder=decoder)
    prep['exclusions']['manual-training-exclusion'] = len(excluded)
    atomic_json(stage / 'preparation.json', prep)

    (stage / 'masks').mkdir()
    for record in records:
        src = folder / record['file']
        if digest(src) != record['maskSha256']:
            raise ValueError(f"Changed training or evaluation mask: {record['id']}")
        shutil.copyfile(src, stage / record['file'])
    (stage / 'vector').mkdir()
    hashes = {}
    changed_pages = {key.split(':')[0] for key in excluded}
    for name, expected in manifest['vectorSnapshotHashes'].items():
        src, dest = folder / 'vector' / name, stage / 'vector' / name
        if digest(src) != expected:
            raise ValueError(f'Changed vector snapshot: {name}')
        if src.stem in changed_pages:
            doc = read_json(src)
            doc['examples'] = [r for r in doc['examples'] if r['id'] not in excluded]
            atomic_json(dest, doc)
        else:
            shutil.copyfile(src, dest)
        hashes[name] = digest(dest)
    frozen = dict(manifest, records=records, groups=counts, decoder=decoder,
                  trainingReview=receipt, vectorSnapshotHashes=hashes)
    atomic_json(stage / 'dataset.json', frozen)
    for name in ('inputs', 'pretrained', 'code'):
        shutil.copytree(folder / name, stage / name)
    for name in ('unfinished-inventory.json', 'runtime-versions.json'):
        shutil.copyfile(folder / name, stage / name)
    (stage / 'logs').mkdir()
    plan = read_json(folder / 'run.json')
    atomic_json(stage / 'run.json', dict(plan, datasetSha256=digest(stage / 'dataset.json'), trainingReview=receipt))


def freeze(selection, output):
    output = output.resolve()
    if output == selection.folder or output.is_relative_to(selection.folder):
        raise ValueError('Use a separate output directory for the training run')
    with selection.lock():
        state = selection.read()
        if state.get('frozenOutput') and state['frozenOutput'] != str(output):
            raise ValueError(f"Selection already frozen in {state['frozenOutput']}")
        if output.exists():
            manifest = read_json(output / 'dataset.json')
            receipt = manifest.get('trainingReview', {})
            if (receipt.get('status'), receipt.get('candidateDatasetSha256'), receipt.get('revision'), receipt.get('excluded')) != (
                    'frozen', selection.fingerprint, state['revision'], state['excluded']):
                raise ValueError('Output already exists and belongs to another selection')
            excluded = set(state['excluded'])
            expected = [r for r in selection.manifest['records'] if r['id'] not in excluded]
            if manifest['records'] != expected or read_json(output / 'run.json')['datasetSha256'] != digest(output / 'dataset.json'):
                raise ValueError('The frozen output differs from the selected records')
        else:
            if digest(selection.folder / 'dataset.json') != selection.fingerprint:
                raise ValueError('Candidate dataset changed during review')
            output.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=output.parent) as temp:
                stage = Path(temp) / 'run'
                stage.mkdir()
                write_selected(selection.folder, stage, selection.manifest, state)
                stage.rename(output)
        state['frozenOutput'] = str(output)
        atomic_json(selection.ledger, state)


def handler_for(selection):
    assets = selection.folder / 'review'
    index = read_json(assets / 'gallery.json')
    if index['datasetSha256'] != selection.fingerprint or {r['id'] for r in index['rows']} != selection.ids:
        raise ValueError('Gallery and candidate manifest disagree')
    allowed = {'/', '/index.html', '/review.mjs', '/review.css', '/gallery.json', '/report-quran.woff'}
    allowed.update(f"/{view}/{r['file']}.png" for r in index['rows'] for view in ('base', 'native'))
    token = secrets.token_urlsafe(32)

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=assets, **kwargs)

        def json_response(self, status, obj):
            body = json.dumps(obj, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def send_head(self):
            if urlsplit(self.path).path not in allowed:
                self.send_error(404)
                return None
            return super().send_head()

        def do_GET(self):
            if urlsplit(self.path).path == '/api/selection':
                return self.json_response(200, dict(selection.read(), token=token))
            return super().do_GET()

        def do_POST(self):
            if self.path != '/api/selection':
                return self.send_error(404)
            if self.headers.get('X-Review-Token') != token:
                return self.json_response(403, {'error': 'Reload the grid to save a change.'})
            try:
                size = int(self.headers.get('Content-Length', 0))
                if not 0 < size <= 4096:
                    raise ValueError('Invalid request size')
                payload = json.loads(self.rfile.read(size))
                if not isinstance(payload, dict):
                    raise ValueError('Invalid review change')
                self.json_response(200, selection.update(payload))
            except (ValueError, TypeError) as error:
                self.json_response(400, {'error': str(error)})

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    for action in ('build', 'serve', 'freeze'):
        sub = actions.add_parser(action)
        sub.add_argument('--folder', type=Path, required=True)
        if action == 'build':
            sub.add_argument('--previous-gallery', type=Path, help='Compare against the previous run’s original masks by subword ID')
        if action == 'serve':
            sub.add_argument('--port', type=int, default=8789)
        if action == 'freeze':
            sub.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    selection = Selection(args.folder)
    if args.action == 'build':
        from letter_training_gallery import build_candidates
        import torch
        torch.set_num_threads(2)
        dest = selection.folder / 'review'
        if dest.exists():
            raise ValueError('Gallery already exists')
        with tempfile.TemporaryDirectory(dir=selection.folder) as temp:
            stage = Path(temp) / 'review'
            stage.mkdir()
            build_candidates(selection.folder, stage, args.previous_gallery)
            stage.rename(dest)
        print(f'{len(selection.ids)} training subwords prepared in {dest}')
    elif args.action == 'serve':
        with ThreadingHTTPServer(('127.0.0.1', args.port), handler_for(selection)) as server:
            print(f'Training review at http://127.0.0.1:{server.server_port}/', flush=True)
            server.serve_forever()
    else:
        freeze(selection, args.output)
        print(f'Included training examples frozen in {args.output}')


if __name__ == '__main__':
    main()
