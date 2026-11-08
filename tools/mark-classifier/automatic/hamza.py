"""Apply the validated Hamza classifier to prepared corpus regions."""
import json

from .common import FEATURE_DIM
from .labels import digest_file
from .learn import Network, ROLES, feature_batches, predictions

TYPES = ('hamza', 'hamza-above', 'hamza-below', 'other-ink')
ROLES_BY_CLASS = ('subword-ink', 'mark', 'mark')


def apply(root, metadata, features, rp, tp, roles, labels, scores, margins):
    import numpy as np

    activation = root / 'models/hamza.json'
    if not activation.exists():
        return {}, None
    config = json.loads(activation.read_text())
    path = root / config['weights']
    if (config['baselineSha256'] != metadata['modelSha256']
            or config['weightsSha256'] != digest_file(path)):
        raise ValueError('Hamza activation does not match its validated checkpoints')
    with np.load(path) as weights:
        net = Network(FEATURE_DIM, weights['w1'].shape[1], len(TYPES))
        net.p = {key: weights[key] for key in net.p}
        mean, std = weights['mean'], weights['std']
    evidence = {}
    rejected = 0
    baseline_types = [metadata['types'].index(name) for name in ('hamza-above', 'hamza-below')]
    for indices, batch in feature_batches(features, np.arange(len(features)), mean, std):
        _, probabilities, _ = net.forward(batch)
        winners = probabilities.argmax(1)
        fallback = indices[(winners == 3) & np.isin(labels[indices], baseline_types)]
        if len(fallback):
            remaining = tp[fallback].copy()
            remaining[:, baseline_types] = 0
            roles[fallback], labels[fallback], scores[fallback], margins[fallback] = predictions(
                rp[fallback], remaining, metadata['typeRoles'])
            rejected += len(fallback)
        for index, winner, votes in zip(indices, winners, probabilities):
            if winner == 3:
                continue
            roles[index] = ROLES.index(ROLES_BY_CLASS[winner])
            scores[index] = votes[winner]
            margins[index] = votes[winner] - np.partition(votes, -2)[-2]
            evidence[int(index)] = {
                'type': TYPES[winner],
                'scores': dict(zip(TYPES, map(float, votes))),
            }
    return evidence, {**config, 'accepted': len(evidence), 'rejectedBaselineHamzas': rejected}
