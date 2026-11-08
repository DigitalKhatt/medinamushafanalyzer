"""Measure local fragmentation separately from total body count."""
import numpy as np

SEEN = frozenset('سش')
TOOTH = frozenset('بتثنيئى')


def layout(letters):
    seen, triples, run = [], [], []
    for letter in letters:
        index = letter['letterIndex']
        character = letter['character']
        form = letter.get('template') or ''
        if character in SEEN:
            seen.append(letter)
        if character in TOOTH and form.startswith('beh-') and 'stacked' not in form:
            run.append(index)
            if len(run) >= 3:
                triples.append(run[-3:])
        else:
            run.clear()
    return seen, triples


def families(letters, seen):
    result = set()
    for letter in seen:
        index = letter['letterIndex']
        if letter['position'] == 'initial':
            result.add('initial-' + letter['character'])
        elif letter['position'] == 'medial' and index < len(letters) and letters[index]['character'] == 'ه':
            result.add('medial-before-heh-' + letter['character'])
        else:
            result.add(letter.get('template') or letter['position'])
    return sorted(result)


def measure(letters, reference, prediction, predicted_roles):
    seen, triples = layout(letters)
    if not (seen or triples):
        return None
    size = len(predicted_roles)
    overlap = np.bincount((reference.astype(np.int32) * size + prediction).ravel(),
                         minlength=(int(reference.max()) + 1) * size).reshape(-1, size)
    is_body = predicted_roles == 1
    totals = overlap.sum(axis=0)
    bodies, runs = [], []
    for letter in seen:
        index = letter['letterIndex']
        if index >= len(overlap):
            continue
        hits = overlap[index]
        area = max(1, hits.sum())
        purity = hits / np.maximum(1, totals)
        fraction = hits / area
        supported = is_body & (purity >= .6) & (fraction >= .08)
        iou = hits / np.maximum(1, area + totals - hits)
        bodies.append(dict(index=index, character=letter['character'], template=letter.get('template'),
                           fragments=int(supported.sum()), coverage=float(fraction[supported].sum()),
                           strayBodies=max(0, int((is_body & (purity >= .6) & (hits >= 4)).sum()) - 1),
                           iou=float(iou[is_body].max(initial=0))))
    for indices in triples:
        if max(indices) >= len(overlap):
            continue
        target = overlap[indices]
        coverage = (target * is_body) / np.maximum(1, target.sum(axis=1)[:, None])
        separate = (coverage.max(axis=1) >= .5).all() and len(set(coverage.argmax(axis=1))) == 3
        merged = ((coverage >= .5).sum(axis=0) >= 2).any()
        runs.append(dict(separate=bool(separate), merged=bool(merged)))
    return dict(seen=bodies, triples=runs,
                fragments=max((b['fragments'] for b in bodies if b['coverage'] >= .65), default=0))


def summarize(rows):
    bodies = [body for row in rows for body in row['seen']]
    triples = [run for row in rows for run in row['triples']]
    return dict(seenCrops=sum(bool(row['seen']) for row in rows),
                fragmentedSeenCrops=sum(row['fragments'] >= 2 for row in rows),
                threeOrMoreFragments=sum(row['fragments'] >= 3 for row in rows),
                straySeenCrops=sum(any(b['strayBodies'] for b in row['seen']) for row in rows),
                seenMatched=sum(body['iou'] >= .5 for body in bodies), seenBodies=len(bodies),
                separateTriples=sum(run['separate'] for run in triples),
                mergedTriples=sum(run['merged'] for run in triples), tripleRuns=len(triples))
