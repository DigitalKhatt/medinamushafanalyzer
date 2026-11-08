"""Compare direct Hamza classes against the published corpus classifier."""
import argparse
import copy
import csv
import json
from pathlib import Path

import numpy as np

from automatic.common import FEATURE_DIM
from automatic.labels import digest_file, load
from automatic.learn import Network, ROLES, feature_batches, predictions

CLASSES = ('standalone-hamza', 'hamza-above', 'hamza-below', 'other-ink')
EXAMPLE = 'page590:l1-w3-s1-c1'


def target(row):
    if row['role'] == 'subword-ink' and row['bases'] == 'ء':
        return 0
    if row['role'] == 'mark' and row['type'] in CLASSES[1:3]:
        return CLASSES.index(row['type'])
    return 3


def evaluate(net, features, ids, mean, std, baseline=None):
    classes, roles, scores = [], [], []
    for _, x in feature_batches(features, ids, mean, std):
        rp, tp, _ = net.forward(x)
        if baseline is None:
            kind = tp.argmax(1)
            role = rp.argmax(1)
            score = tp[np.arange(len(x)), kind]
        else:
            role, subtype, score, _ = predictions(rp, tp, baseline['typeRoles'])
            kind = np.array([
                CLASSES.index(baseline['types'][i])
                if i >= 0 and baseline['types'][i] in CLASSES[1:3] else 3
                for i in subtype
            ])
        classes.extend(kind)
        roles.extend(role)
        scores.extend(score)
    return np.array(classes), np.array(roles), np.array(scores)


def metrics(truth, predicted, roles, baseline=False):
    # Generic body is the most specific standalone result the baseline can offer.
    comparable = predicted.copy()
    if baseline:
        comparable[(truth == 0) & (roles == ROLES.index('subword-ink'))] = 0
    return {
        'correct': {name: int(((truth == i) & (comparable == i)).sum())
                    for i, name in enumerate(CLASSES)},
        'support': {name: int((truth == i).sum()) for i, name in enumerate(CLASSES)},
        'otherInkCalledHamza': int(((truth == 3) & (predicted != 3)).sum()),
        'confusion': np.bincount(4 * truth + comparable, minlength=16).reshape(4, 4).tolist(),
    }


def run(root, output, labels=None):
    manifest, rows = load(root, labels)
    features = np.memmap(root / 'features.u8', mode='r', dtype='u1',
                         shape=(len(rows), FEATURE_DIM))
    model = json.loads((root / 'models/model.json').read_text())
    weight_path = root / 'models/selected.npz'
    if digest_file(weight_path) != model['modelSha256']:
        raise ValueError('Baseline weights do not match model metadata')
    baseline_weights = np.load(weight_path)
    baseline = Network(FEATURE_DIM, baseline_weights['w1'].shape[1], len(model['types']))
    baseline.p = {key: baseline_weights[key] for key in baseline.p}
    sample = json.loads((root / 'models' / f"round-{model['selectedRound']}-samples.json").read_text())
    sampled_ids = {entry['id'] for entry in sample['examples']}
    truth = np.array([target(row) for row in rows])
    eligible = np.array([row['role'] in ROLES and row['quality'] >= .2 for row in rows])
    split = np.array([row['split'] for row in rows])
    example = next(i for i, row in enumerate(rows) if row['item_id'] == EXAMPLE)
    if split[example] != 'validation' or truth[example] != 0 or not eligible[example]:
        raise ValueError('Page 590 example must be an eligible standalone validation case')
    selected = np.array([row['item_id'] in sampled_ids for row in rows])
    train = np.flatnonzero(eligible & (split == 'train') & ((truth != 3) | selected))
    validation = np.flatnonzero(eligible & (split == 'validation'))
    x = np.asarray(features[train], dtype='f4') / 255
    mean, std = x.mean(0), np.maximum(.12, x.std(0))
    x = (x - mean) / std
    roles = np.array([ROLES.index(rows[i]['role']) for i in train])
    weights = np.array([rows[i]['quality'] for i in train], dtype='f4')
    net = Network(FEATURE_DIM, 128, len(CLASSES))
    rng = np.random.default_rng(42)
    best = -1
    for epoch in range(24):
        order = rng.permutation(len(train))
        for start in range(0, len(order), 256):
            batch = order[start:start + 256]
            net.batch(x[batch], roles[batch], truth[train[batch]], weights[batch],
                      .0015 * (1 - .8 * epoch / 23))
        predicted, _, _ = evaluate(net, features, validation, mean, std)
        matrix = np.bincount(4 * truth[validation] + predicted, minlength=16).reshape(4, 4)
        score = np.mean(2 * matrix.diagonal() / np.maximum(1, matrix.sum(0) + matrix.sum(1)))
        if score > best:
            best, chosen, best_epoch = score, copy.deepcopy(net.p), epoch + 1
    net.p = chosen
    output.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(output / 'weights.npz', **net.p, mean=mean, std=std)
    report = {
        'parentType': 'hamza',
        'classes': [{'type': 'hamza', 'class': name, 'role': role}
                    for name, role in zip(CLASSES[:3], ('subword-ink', 'mark', 'mark'))],
        'baselineSha256': model['modelSha256'],
        'trainingLabelsSha256': manifest['labelsSha256'],
        'reference': 'Prepared automatic labels with any existing human reviews',
        'baselineStandaloneComparison': 'Generic body prediction on a standalone reference',
        'trainCounts': dict(zip(CLASSES, np.bincount(truth[train], minlength=4).tolist())),
        'selectedEpoch': best_epoch,
    }
    with (output / 'held-out-hamzas.csv').open('w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['id', 'split', 'reference_class', 'baseline_class', 'baseline_role',
                         'baseline_score', 'trial_type', 'trial_class', 'trial_score'])
        for name in ('validation', 'test'):
            ids = np.flatnonzero(eligible & (split == name))
            old, old_roles, old_scores = evaluate(
                baseline, features, ids, baseline_weights['mean'], baseline_weights['std'], model)
            new, new_roles, new_scores = evaluate(net, features, ids, mean, std)
            report[name] = {'baseline': metrics(truth[ids], old, old_roles, True),
                            'trial': metrics(truth[ids], new, new_roles)}
            for j in np.flatnonzero((truth[ids] != 3) | (old != 3) | (new != 3)):
                record = [rows[ids[j]]['item_id'], name, CLASSES[truth[ids[j]]],
                          CLASSES[old[j]], ROLES[old_roles[j]], float(old_scores[j]),
                          'hamza' if new[j] != 3 else '', CLASSES[new[j]], float(new_scores[j])]
                writer.writerow(record)
                if ids[j] == example:
                    report['page590'] = record
    (output / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('data/automatic'))
    parser.add_argument('--output', type=Path, default=Path('data/automatic/experiments/hamza-trial'))
    parser.add_argument('--labels', type=Path)
    args = parser.parse_args()
    run(args.root, args.output, args.labels)
