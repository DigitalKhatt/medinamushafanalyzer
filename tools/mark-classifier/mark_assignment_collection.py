"""Searchable cross-page index; differences are summed per owner, never canceled."""
import base64
from collections import Counter
import json
from pathlib import Path

from mark_validation import comparison_inventory


def collect_page(result, report_hash):
    by_id = {s['id']: s for s in result['subwords']}
    words = {w['id']: w for w in result['words']}
    differences = []
    for comparison in result['validation']:
        if comparison['matches']:
            continue
        sub = by_id[comparison['id']]
        owners = {o['id']: o for o in sub['owners']}
        missing, extra, expected, classified, comparable = Counter(), Counter(), Counter(), Counter(), Counter()
        rows = []
        for row in comparison['owners']:
            expected.update(row['expected']); classified.update(row['classified'])
            comparable.update(comparison_inventory(row))
            missing.update(row['missing']); extra.update(row['extra'])
            if not row['matches'] or not row['contourMatches'] or not row['groupingMatches'] or row.get('characterLinksMatch') is False:
                owner = owners[row['id']]
                rows.append(dict(id=row['id'], text=owner['text'], ordinal=owner['ordinal'], role=owner['role'],
                                 expected=row['expected'], classified=row['classified'], missing=row['missing'], extra=row['extra'],
                                 expectedContours=owner['expectedContours'], assignedContours=owner['assignedContours'],
                                 groupingIssues=owner['groupingIssues']))
                if 'characterLinksMatch' in row:
                    rows[-1]['characterIssues'] = [
                        dict(groupId=g['id'], pathIds=g['pathIds'], link=g['transcriptLink'], classified=p['inventoryType'],
                             status=p['characterMatch'])
                        for g, p in zip(owner['groups'], row['predictions'], strict=True)
                        if p['characterMatch'] not in ('match', 'accepted-form')]
                    rows[-1]['charactersWithoutInk'] = [slot for slot in owner['expectedSlots']
                                                        if slot['characterOffset'] in row['charactersWithoutInk']]
        differences.append(dict(id=sub['id'], pageId=result['pageId'], wordId=sub['wordId'], text=sub['text'],
                                wordText=words[sub['wordId']]['text'], missing=dict(missing), extra=dict(extra),
                                matchingAggregateInventory=expected == comparable,
                                ownerIdentityDifferences=sum(not r['matches'] for r in comparison['owners']),
                                characterLinkDifference=comparison.get('characterLinksMatch') is False,
                                verticalStatus=sub.get('verticalAssignment', {}).get('status'),
                                verticalReason=sub.get('verticalAssignment', {}).get('reason'),
                                bodySelectionStatus=sub.get('bodySelection', {}).get('status'),
                                bodySelectionReason=sub.get('bodySelection', {}).get('reason'),
                                contourDifferences=sum(not r['contourMatches'] for r in comparison['owners']),
                                groupingDifferences=sum(not r['groupingMatches'] for r in comparison['owners']),
                                unusedPaths=len(sub['unusedPathIds']), owners=rows))
    comparisons = {s['id']: s for s in result['validation']}
    body_corrections = []
    for sub in result['subwords']:
        selection = sub.get('bodySelection', {})
        if selection.get('status') != 'applied':
            continue
        comparison = comparisons[sub['id']]
        missing, extra = Counter(), Counter()
        for row in comparison['owners']:
            missing.update(row['missing']); extra.update(row['extra'])
        body_corrections.append(dict(id=sub['id'], pageId=result['pageId'], text=sub['text'],
                                     wordText=words[sub['wordId']]['text'], matches=comparison['matches'],
                                     missing=dict(missing), extra=dict(extra),
                                     nativeBodyPathId=sub['nativeBodyPathId'], bodyPathId=sub['bodyPathId'],
                                     nativeAspect=selection['nativeGeometry']['aspect'],
                                     selectedAspect=selection['selectedGeometry']['aspect']))
    partial_corrections = []
    for sub in result['subwords']:
        vertical = sub.get('verticalAssignment', {})
        reservation = vertical.get('ambiguousReservation', {})
        if vertical.get('status') != 'applied' or reservation.get('status') != 'reserved':
            continue
        comparison = comparisons[sub['id']]
        missing, extra = Counter(), Counter()
        for row in comparison['owners']:
            missing.update(row['missing']); extra.update(row['extra'])
        partial_corrections.append(dict(id=sub['id'], pageId=result['pageId'], text=sub['text'],
            wordText=words[sub['wordId']]['text'], matches=comparison['matches'], missing=dict(missing), extra=dict(extra),
            changedOwnerIds=vertical['changedOwnerIds'], lockedOwners=reservation['owners']))
    waqf_corrections = []
    for sub in result['subwords']:
        reservation = sub.get('waqfReservation', {})
        if reservation.get('status') != 'applied':
            continue
        comparison = comparisons[sub['id']]
        waqf_corrections.append(dict(id=sub['id'], pageId=result['pageId'], text=sub['text'],
            wordText=words[sub['wordId']]['text'], matches=comparison['matches'],
            candidatePathIds=reservation['candidatePathIds'], selectionMethod=reservation['selectionMethod'],
            changedOwnerIds=reservation['changedOwnerIds'], clearance=reservation['clearance']))
    return dict(pageId=result['pageId'], summary=result['summary'], conservation=result['conservation'],
                assignmentSha256=result['assignmentSha256'], reportSha256=report_hash,
                differences=differences, bodyCorrections=body_corrections, partialVerticalCorrections=partial_corrections,
                waqfCorrections=waqf_corrections)


def aggregate(selected, results, failures):
    results = sorted(results, key=lambda p: p['pageId'])
    failures = sorted(failures, key=lambda p: p['pageId'])
    completed = [p['pageId'] for p in results]
    failed = [p['pageId'] for p in failures]
    if len(completed)+len(failed) != len(selected) or len(set(completed+failed)) != len(selected) or set(completed+failed) != set(selected):
        raise ValueError('Batch coverage must account for every requested page exactly once')
    totals = Counter(); missing, extra = Counter(), Counter()
    for page in results:
        totals.update({k: v for k, v in page['summary'].items() if isinstance(v, int)})
        missing.update(page['summary']['missing']); extra.update(page['summary']['extra'])
    summary = dict(totals, requestedPages=len(selected), completedPages=len(results), failedPages=len(failures),
                   missing=dict(missing), extra=dict(extra),
                   sourceInkPixels=sum(p['conservation']['inkPixels'] for p in results),
                   everySourcePathAccounted=all(p['conservation']['everyPathAccounted'] for p in results))
    return dict(schemaVersion=5, summary=summary, requestedPageIds=selected, pages=results, failures=failures)


def write_collection(output, selected, results, failures, provenance=None):
    output = Path(output)
    manifest = aggregate(selected, results, failures)
    manifest['exportProvenance'] = provenance
    encoded = json.dumps(manifest, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
    (output/'manifest.json').write_text(encoded+'\n')
    template = Path(__file__).with_name('mark_assignment_collection.html').read_text()
    font = Path(__file__).with_name('mark_assignment_report.woff').read_bytes()
    template = template.replace('__REPORT_FONT_DATA__', base64.b64encode(font).decode('ascii'))
    (output/'index.html').write_text(template.replace('__COLLECTION_JSON__', encoded.replace('<', '\\u003c')))
    return manifest
