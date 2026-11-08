"""Geometry-only staggered stroke groups, independent of word/subword text."""
from collections import Counter, defaultdict
import hashlib
import itertools
import json

from .stroke_geometry import stroke_pair, stroke_paths


def mask_runs(paths):
    result = []
    for start, count in sorted((s, n) for path in paths
                              for s, n in zip(path['runs'][::2], path['runs'][1::2])):
        if result and result[-2]+result[-1] == start:
            result[-1] += count
        else:
            result.extend((start, count))
    return result


def find_staggered_groups(paths, width, line_baselines):
    """Find unambiguous extended pairs using only source paths and line geometry.

    No words, subwords, transcript characters, teacher classes or predictions are
    accepted as inputs. The existing compact-pair rule remains unchanged.
    """
    lines = defaultdict(list)
    splits = {}
    for path in paths:
        baseline = line_baselines.get(str(path['line']))
        box = path['bbox']
        if baseline is None or box[1]+box[3]/2 <= baseline or box[2] < 1.1*box[3]:
            continue
        parts = stroke_paths(path, width, split=True)
        if len(parts) > 1:
            splits[path['id']] = parts
        for part in parts:
            if part.get('stroke') is not None and part['stroke'][1] > baseline:
                lines[path['line']].append(part)
    pairs = []
    for line, strokes in lines.items():
        for first, second in itertools.combinations(strokes, 2):
            pair = [first, second]
            if not stroke_pair(pair) and stroke_pair(pair, staggered=True):
                pairs.append((line, pair))
    uses = Counter(path['id'] for _, pair in pairs for path in pair)
    groups = []
    for line, pair in pairs:
        if any(uses[path['id']] != 1 for path in pair):
            continue
        parents = {path.get('sourcePathId') for path in pair}.intersection(splits)
        groups.append({'id': 'geometry-staggered-'+hashlib.sha256(
            json.dumps(mask_runs(pair), separators=(',', ':')).encode()).hexdigest()[:20],
            'line': line, 'paths': pair,
            'splitPaths': {parent: splits[parent] for parent in parents}})
    return groups


def apply_staggered_groups(document, groups):
    """Apply independent mask groups; inherit ownership only when hints agree.

    Teacher labels and subword boundaries cannot veto or create a group. A
    cross-boundary group keeps null ownership for later assignment.
    """
    paths = {path['id']: path for path in document['paths']}
    for group in groups:
        for parent, parts in group['splitPaths'].items():
            for part in parts:
                paths.setdefault(part['id'], part)
            for candidate in document['candidates']:
                candidate['pathIds'] = [child for pid in candidate['pathIds']
                    for child in ([part['id'] for part in parts] if pid == parent else [pid])]
        members = {path['id'] for path in group['paths']}
        for path in group['paths']:
            paths.setdefault(path['id'], path)
        affected = [c for c in document['candidates'] if members.intersection(c['pathIds'])]
        if len(affected) == 1 and set(affected[0]['pathIds']) == members:
            continue
        linked = {}
        for field in ('wordId', 'subwordId'):
            values = {c.get(field) for c in affected}
            linked[field] = values.pop() if len(values) == 1 else None
        kept, removed = [], set()
        for candidate in document['candidates']:
            remaining = [pid for pid in candidate['pathIds'] if pid not in members]
            if remaining:
                kept.append({**candidate, 'pathIds': remaining})
            else:
                removed.add(candidate['id'])
        kept.append({'id': group['id'], 'kind': 'mark', 'markType': 'tanwin-kasr',
            'pathIds': sorted(members), 'line': group['line'], **linked,
            'issues': ['Geometry-only staggered stroke group; ownership is an inherited hint'],
            'groupingSource': 'line-geometry-v1'})
        document['candidates'] = kept
        for subword in document.get('subwords', []):
            for field in ('candidateIds', 'bodyCandidateIds'):
                if field in subword:
                    subword[field] = [cid for cid in subword[field] if cid not in removed]
            if subword['id'] == linked['subwordId']:
                subword.setdefault('candidateIds', []).append(group['id'])
    document['paths'] = list(paths.values())
    document['summary'] = dict(Counter(c['kind'] for c in document['candidates']))
    document['provenance']['staggeredGrouping'] = {
        'version': 1, 'source': 'source outlines and geometric line baselines only',
        'groups': [{'id': g['id'], 'pathIds': sorted(p['id'] for p in g['paths'])} for g in groups]}
    return document
