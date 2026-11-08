"""Read-only inference on already assigned ink, using the existing classifier.

The API accepts masks and image geometry only. It never receives expected mark
types, transcript text, letter positions, or subword membership. It never opens
a corpus database or changes labels. Model weights can be frozen with the report.
"""
import hashlib
import json
from pathlib import Path
import shutil
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def model_files(root):
    root = Path(root).resolve()
    files = {'models/model.json', 'models/selected.npz'}
    for name in ('hamza', 'damma-yeh'):
        activation = root/f'models/{name}.json'
        if activation.exists():
            config = json.loads(activation.read_text())
            weight = (root/config['weights']).resolve()
            if not weight.is_relative_to(root/'models'):
                raise ValueError('Activated checkpoint must be inside models/')
            files.add(activation.relative_to(root).as_posix())
            files.add(weight.relative_to(root).as_posix())
    return {name: digest(root/name) for name in sorted(files)}


def freeze_models(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    manifest = model_files(source)
    for name, expected in manifest.items():
        target = destination/name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and digest(target) != expected:
            raise ValueError(f'Frozen checkpoint differs: {name}; use a new inputs directory')
        if not target.exists():
            shutil.copyfile(source/name, target)
        if digest(target) != expected:
            raise ValueError('Checkpoint changed during copy')
    return manifest


def classify_regions(regions, source_paths, raster, model_root, classifier_repo):
    """No ownership data may cross this boundary, even accidentally."""
    allowed = {'id', 'runs', 'bbox', 'baseline'}
    if any(set(r) != allowed for r in regions):
        raise ValueError('Classifier input must contain only opaque ID, exact mask, bbox and baseline')
    if any(set(p) != {'runs'} for p in source_paths):
        raise ValueError('Visual context accepts source ink only')
    classifier_repo = Path(classifier_repo).resolve()
    sys.path.insert(0, str(classifier_repo))
    import numpy as np
    from automatic.common import FEATURE_DIM, feature_vector, ink_components
    from automatic.learn import ROLES, Network, feature_batches, predictions, decoder_policy
    from automatic.hamza import apply as apply_hamza
    from automatic.damma_yeh import apply as apply_pair

    root = Path(model_root).resolve()
    metadata = json.loads((root/'models/model.json').read_text())
    checkpoints = model_files(root)
    if checkpoints['models/selected.npz'] != metadata['modelSha256']:
        raise ValueError('Classifier weights do not match vocabulary metadata')
    source = np.zeros((raster['height'], raster['width']), dtype='u1')
    flat = source.ravel()
    for p in source_paths:
        for start, length in zip(p['runs'][::2], p['runs'][1::2]):
            flat[start:start+length] = 1
    features = np.empty((len(regions), FEATURE_DIM), dtype='u1')
    components = []
    for i, region in enumerate(regions):
        n = ink_components(region['runs'], region['bbox'], raster['width'])
        components.append(n)
        features[i] = feature_vector(region['runs'], region['bbox'], source, region['baseline'],
                                     components=n, scale=raster['scale'])
    with np.load(root/'models/selected.npz') as weights:
        net = Network(FEATURE_DIM, weights['w1'].shape[1], len(metadata['types']))
        net.p = {k: weights[k] for k in net.p}
        mean, std = weights['mean'], weights['std']
    rp = np.empty((len(regions), len(ROLES)), dtype='f4')
    tp = np.empty((len(regions), len(metadata['types'])), dtype='f4')
    for indices, batch in feature_batches(features, np.arange(len(regions)), mean, std):
        rp[indices], tp[indices], _ = net.forward(batch)
    roles, types, scores, margins = predictions(rp, tp, metadata['typeRoles'])
    hamzas, hamza_report = apply_hamza(root, metadata, features, rp, tp, roles, types, scores, margins)
    pair, pair_report = apply_pair(root, metadata, features, rp, tp, roles, types, scores, margins,
                                   excluded=set(hamzas))
    output = []
    for i, region in enumerate(regions):
        output.append(dict(id=region['id'], kind=ROLES[int(roles[i])],
                           type=hamzas[i]['type'] if i in hamzas else metadata['types'][int(types[i])] if types[i] >= 0 else 'unknown',
                           score=float(scores[i]), margin=float(margins[i]), components=components[i],
                           roleScores=dict(zip(ROLES, map(float, rp[i]))),
                           typeScores=dict(zip(metadata['types'], map(float, tp[i]))),
                           refinement=({'hamza': hamzas[i]} if i in hamzas else {'shapePair': pair[i]} if i in pair else None)))
    return dict(predictions=output, provenance=dict(
        modelFiles=checkpoints, selectedRound=metadata['selectedRound'], decoder=decoder_policy(),
        sourceInkSha256=hashlib.sha256(source.tobytes()).hexdigest(),
        featureSha256=hashlib.sha256(features.tobytes()).hexdigest(),
        classifierCodeHashes={f'automatic/{name}': digest(classifier_repo/'automatic'/name)
                              for name in ('common.py', 'learn.py', 'hamza.py', 'damma_yeh.py', 'labels.py')},
        context='Exact native page-ink union; features rebuilt from the frozen assigned masks.',
        inputs='Region mask, local source image and intrinsic/baseline geometry only; opaque region IDs are output keys.',
        humanOverrides=False, corpusDatabaseRead=False,
        hamzaClassifier=hamza_report, shapePairClassifier=pair_report,
        numpyVersion=np.__version__))
