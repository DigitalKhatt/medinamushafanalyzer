"""Check the external 1441 edition and validation weights against the pinned reference."""
import hashlib
import json
from pathlib import Path

from store import ALL, HERE, ROOT, VECTOR, require
from workbench_paths import DATA

MARK_CLASSIFIER = VECTOR
from classify_assigned_marks import model_files


def file_sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def artwork_fingerprint(rows):
    h = hashlib.sha256()
    for row in rows:
        h.update((' '.join(row)+'\n').encode('ascii'))
    return h.hexdigest()


def ai_page(pid):
    return ROOT/'input/1441-AI-hafs'/(pid[4:]+'___Hafs39__DM.ai')


def validation_models(model_root=None):
    return Path(model_root or DATA/'model-bundles/dot-grouping-v9-20261104').resolve()


def check(models, lock=None, full=True):
    expected = json.loads(Path(lock or HERE/'inputs.lock.json').read_text())
    for name,digest in expected['nativeFiles'].items():
        require(file_sha(ROOT/name)==digest, f'Source file differs from the input lock: {name}')
    require(model_files(models)==expected['validationModels'],'The validation models differ from the input lock')
    if full:
        pdfs = ((pid, file_sha(ai_page(pid))) for pid in ALL)
        require(artwork_fingerprint(pdfs)==expected.get('aiPagesSha256'),
                'The AI/PDF edition differs from the input lock')
    return expected
