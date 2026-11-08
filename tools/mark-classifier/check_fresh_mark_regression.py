#!/usr/bin/env python3
"""Compare a cache-free native rebuild with the verified mark-validation report."""
import argparse
from collections import Counter
import copy
import json
from pathlib import Path

from classify_assigned_marks import digest


def read(path):
    return json.loads(path.read_text())


def difference_count_errors(reference_rows, fresh_rows, reference_total, fresh_total, expected=None):
    errors = []
    if reference_rows != reference_total:
        errors.append('Baseline manifest and per-subword differences disagree')
    if fresh_rows != fresh_total or fresh_total != reference_total:
        errors.append('Fresh differences disagree with their manifest or the selected baseline')
    if expected is not None and reference_total != expected:
        errors.append(f'Expected {expected} differences in the baseline')
    return errors


def normalize_waqf_links(report):
    """Compare older reports against the explicit native link for the same sign."""
    attachments = {}
    for subword in report['subwords']:
        preceding_letter = None
        for owner in subword['owners']:
            if owner['role']=='letter':
                preceding_letter = owner['id']
            for group in owner['groups']:
                link = group['transcriptLink']
                if owner['role']=='word-sign' and preceding_letter and (link.get('expectedType') or '').startswith('waqf-'):
                    attachments[group['id']] = preceding_letter
                normalize_group_link(group,attachments)
    for group in report['groups']:
        normalize_group_link(group,attachments)


def normalize_group_link(group, attachments):
    link = group['transcriptLink']
    expected = attachments.get(group['id'])
    if link.get('attachedLetterId',expected)!=expected:
        raise ValueError(f"{group['id']}: incorrect native waqf letter link")
    if expected is not None:
        link['attachedLetterId'] = expected


def compare(baseline, candidate, expected_differences=None):
    baseline, candidate = Path(baseline), Path(candidate)
    old_manifest, new_manifest = read(baseline/'manifest.json'), read(candidate/'manifest.json')
    problems, membership_order, flag_changes, summary_metadata = [], [], [], []
    counts = Counter()

    def check(condition, message):
        if not condition:
            problems.append(message)

    pages = old_manifest['requestedPageIds']
    check(pages == new_manifest['requestedPageIds'], 'Requested page coverage changed')
    check(new_manifest['summary']['completedPages'] == 604 and new_manifest['summary']['failedPages'] == 0,
          'Fresh report does not cover all 604 pages successfully')
    check(read(baseline/'inputs/transcript.json') == read(candidate/'inputs/transcript.json'), 'Transcript changed')
    before_differences, after_differences = {}, {}
    for pid in pages:
        a, b = read(baseline/f'{pid}.json'), read(candidate/f'{pid}.json')
        normalize_waqf_links(a)
        normalize_waqf_links(b)
        provenance = b['provenance']
        check(provenance['cacheSha256'] is None and provenance['cacheOrigin'].startswith('fresh AI/PDF'),
              f'{pid}: segmentation cache appears in provenance')
        for key in ('pdfSha256', 'sourceSha256', 'templateSha256', 'pdfToRaster', 'pdfCropSize'):
            check(a['provenance'][key] == provenance[key], f'{pid}: source/template/transform changed: {key}')
        check(b['nativeAssignment']['rawInputSha256'] == digest(candidate/'inputs'/f'{pid}.raw.json'),
              f'{pid}: native assignment source hash mismatch')
        check(b['nativeAssignment']['geometrySha256'] is None, f'{pid}: geometry came from a separate saved export')
        check(b['assignmentUnchangedAfterValidation'], f'{pid}: validation mutated assignment')
        for key in ('raster', 'words', 'paths', 'groups', 'assignmentComponents', 'conservation',
                    'ignoredSignGroups', 'unownedSourcePathIds'):
            check(a[key] == b[key], f'{pid}: changed native {key}')
        old_subs, new_subs = ({s['id']: s for s in r['subwords']} for r in (a, b))
        check(old_subs.keys() == new_subs.keys(), f'{pid}: subword IDs changed')
        for sid in old_subs.keys() & new_subs.keys():
            old, new = copy.deepcopy(old_subs[sid]), copy.deepcopy(new_subs[sid])
            if old['pathIds'] != new['pathIds'] and sorted(old['pathIds']) == sorted(new['pathIds']):
                membership_order.append(dict(id=sid, cachedOrder=old['pathIds'], freshOrder=new['pathIds']))
                old['pathIds'].sort(); new['pathIds'].sort()
            # Legacy state bits describe the earlier segmentation attempt. Keep
            # them visible, while checking bodies, membership and owners exactly.
            if old['nativeLegacyFlags'] != new['nativeLegacyFlags']:
                flag_changes.append(dict(id=sid, cached=old['nativeLegacyFlags'], fresh=new['nativeLegacyFlags']))
                del old['nativeLegacyFlags']; del new['nativeLegacyFlags']
            check(old == new, f'{sid}: ownership, quotas, grouping or assignment trace changed')
        check(a['validation'] == b['validation'], f'{pid}: per-letter validation changed')
        check(a['classifier'] == b['classifier'], f'{pid}: classifier inputs or prediction provenance changed')
        check(read(baseline/f'{pid}.predictions.json') == read(candidate/f'{pid}.predictions.json'),
              f'{pid}: classifier predictions changed')
        old_summary, new_summary = dict(a['summary']), dict(b['summary'])
        # Fresh parsing may recognize a WAQF immediately instead of restoring a
        # missing flag from a legacy cache. This is a provenance counter only.
        if old_summary.get('nativeWaqfFlagsRestored') != new_summary.get('nativeWaqfFlagsRestored'):
            summary_metadata.append(dict(pageId=pid, field='nativeWaqfFlagsRestored',
                cached=old_summary.get('nativeWaqfFlagsRestored'), fresh=new_summary.get('nativeWaqfFlagsRestored')))
        old_summary.pop('nativeWaqfFlagsRestored', None);new_summary.pop('nativeWaqfFlagsRestored', None)
        check(old_summary == new_summary, f'{pid}: validation summary changed')
        before_differences.update({s['id']: s for s in a['validation'] if not s['matches']})
        after_differences.update({s['id']: s for s in b['validation'] if not s['matches']})
        counts.update(pages=1, subwords=len(b['subwords']), owners=sum(len(s['owners']) for s in b['subwords']),
                      groups=len(b['groups']), sourcePaths=len(b['paths']), sourceInkPixels=b['conservation']['inkPixels'])
        if counts['pages'] % 100 == 0:
            print(f"Compared {counts['pages']}/604 fresh pages", flush=True)
    added = sorted(after_differences.keys()-before_differences.keys())
    resolved = sorted(before_differences.keys()-after_differences.keys())
    changed = sorted(sid for sid in before_differences.keys() & after_differences.keys()
                     if before_differences[sid] != after_differences[sid])
    problems.extend(difference_count_errors(len(before_differences), len(after_differences),
        old_manifest['summary']['differingSubwords'], new_manifest['summary']['differingSubwords'],
        expected_differences))
    check(not added and not resolved and not changed, 'The exact differences changed')
    for source in (baseline/'inputs/classifier').rglob('*'):
        if source.is_file():
            target = candidate/'inputs/classifier'/source.relative_to(baseline/'inputs/classifier')
            check(target.exists() and digest(source) == digest(target), f'Classifier checkpoint changed: {source.name}')
            counts['classifierFilesCompared'] += 1
    run = read(candidate/'fresh-run-provenance.json')
    check(run['cachedExportNegativeControlPassed'] and run['segmentationCachesAvailable'] is False,
          'Native run was not isolated from segmentation caches')
    return dict(passed=not problems, failures=problems, counts=dict(counts),
                baselineDifferences=len(before_differences), freshDifferences=len(after_differences),
                addedDifferences=added, resolvedDifferences=resolved, changedDifferences=changed,
                differenceIds=sorted(after_differences), membershipOrderOnlyDifferences=membership_order,
                legacyStateMetadataChanges=flag_changes, provenanceCounterChanges=summary_metadata,
                baselineManifestSha256=digest(baseline/'manifest.json'),
                freshManifestSha256=digest(candidate/'manifest.json'),
                policy='Source pixels, words, membership, bodies, owners, exact groups, predictions and per-letter differences must match. Only native membership-list order and documented cache-restoration metadata may differ.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--expected-differences', type=int, help='Optionally pin the historical reference count')
    args = parser.parse_args()
    result = compare(args.baseline, args.candidate, args.expected_differences)
    (args.candidate/'fresh-regression.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('differenceIds', 'membershipOrderOnlyDifferences')}, indent=2))
    raise SystemExit(0 if result['passed'] else 1)
