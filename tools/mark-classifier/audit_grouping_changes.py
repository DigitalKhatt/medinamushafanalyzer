#!/usr/bin/env python3
"""Compare all proposed mark groups, ignoring generated candidate numbering."""
import argparse
import hashlib
import json
from pathlib import Path

from automatic.common import merge_runs


SEMANTICS = ('kind', 'markType', 'wordId', 'subwordId', 'line')


def candidates(document):
    result = {}
    for candidate in document['candidates']:
        key = tuple(sorted(candidate['pathIds']))
        if key in result:
            raise ValueError('Duplicate source group: ' + repr(key))
        result[key] = candidate
    return result


def compare(before, after):
    before, after = Path(before), Path(after)
    pages = sorted(p.name for p in before.glob('page*.json'))
    if not pages or pages != sorted(p.name for p in after.glob('page*.json')):
        raise ValueError('Proposal page sets differ')
    report = {'pages': len(pages), 'unchangedGroups': 0, 'changes': [],
              'allSourcePathsIdentical': True, 'allUnchangedGroupLabelsAndOwnersIdentical': True}
    for filename in pages:
        old = json.loads((before/filename).read_text())
        new = json.loads((after/filename).read_text())
        for field in ('pageId', 'sourceSha256', 'raster'):
            if old[field] != new[field]:
                raise ValueError(filename + ': changed source metadata: ' + field)
        a = {p['id']: p for p in old['paths']}
        b = {p['id']: p for p in new['paths']}
        if a.keys() != b.keys():
            raise ValueError(filename + ': changed source path set')
        for key in a:
            for field in ('runs', 'bbox', 'contourCount'):
                if a[key][field] != b[key][field]:
                    raise ValueError(filename + ': changed source path geometry: ' + key)
        left, right = candidates(old), candidates(new)
        for key in left.keys() & right.keys():
            if any(left[key].get(f) != right[key].get(f) for f in SEMANTICS):
                raise ValueError(filename + ': relabelled or reassigned unchanged group: ' + repr(key))
        report['unchangedGroups'] += len(left.keys() & right.keys())
        removed = [left[k] for k in sorted(left.keys() - right.keys())]
        added = [right[k] for k in sorted(right.keys() - left.keys())]
        if not removed and not added:
            continue
        old_paths = sorted(p for c in removed for p in c['pathIds'])
        new_paths = sorted(p for c in added for p in c['pathIds'])
        if old_paths != new_paths:
            raise ValueError(filename + ': changed groups lose or duplicate source paths')
        # Exact mask conservation as well as path-ID conservation.
        old_runs = merge_runs([a[p] for p in old_paths])
        new_runs = merge_runs([b[p] for p in new_paths])
        if old_runs != new_runs:
            raise ValueError(filename + ': changed groups alter source pixels')
        def details(candidate):
            return {key: candidate.get(key) for key in ('id', 'pathIds', *SEMANTICS)}
        report['changes'].append({'page': old['pageId'],
            'removed': [details(c) for c in removed], 'added': [details(c) for c in added],
            'conservedMaskSha256': hashlib.sha256(json.dumps(old_runs).encode()).hexdigest()})
    report['changedPages'] = len(report['changes'])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = compare(args.before, args.after)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({key: value for key, value in report.items() if key != 'changes'}))


if __name__ == '__main__':
    main()
