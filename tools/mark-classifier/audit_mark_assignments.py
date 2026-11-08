#!/usr/bin/env python3
"""Validate native C++ mark assignments with the classifier and render reports.

This script never chooses bodies, quotas, owners, or groups. Native assignments
must already exist before the classifier is loaded. No corpus database or
letter-body segmentation participates. Frozen exports/checkpoints are replayable.
"""
import argparse
import base64
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from mark_validation import compare_predictions, comparison_inventory, digest_json, painted_source_paths
from classify_assigned_marks import classify_regions, digest, freeze_models, model_files
from mark_assignment_collection import collect_page, write_collection
from workbench_paths import DATA, PROJECT

ROOT = Path(__file__).resolve().parents[2]
CLASSIFIER = PROJECT
VERSION = 5


def validate_transcript(snapshot, source=None):
    if snapshot.get('producer') != 'medinamushafanalyzer-QuranTextByWord-v1':
        raise ValueError('Use the native QuranTextByWord export')
    pages = {p['pageId']: p for p in snapshot['pages']}
    if len(pages) != len(snapshot['pages']):
        raise ValueError('Duplicate transcript page')
    if source is not None:
        raw = {f'page{int(n)+1:03d}': text for n, text in
               re.findall(r'R"page(\d+)\((.*?)\)page\1"', source, re.S)}
        for pid, page in pages.items():
            if page['rawText'] != raw.get(pid):
                raise ValueError(f'{pid}: binary transcript differs from quran.cpp; rebuild native binary')
    return pages


def write_json(path, data):
    Path(path).write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, separators=(',', ':'))+'\n')


def summarize(assignment, comparisons):
    owners = [o for s in assignment['subwords'] for o in s['owners']]
    rows = [o for s in comparisons for o in s['owners']]
    missing, extra = Counter(), Counter()
    for row in rows:
        missing.update(row['missing']); extra.update(row['extra'])
    by_id = {r['id']: r for r in rows}
    aggregate_match = 0
    for sub in comparisons:
        expected, observed = Counter(), Counter()
        for row in sub['owners']:
            expected.update(row['expected']); observed.update(comparison_inventory(row))
        aggregate_match += any(not r['matches'] for r in sub['owners']) and expected == observed
    link_stats = {}
    if assignment.get('schemaVersion', 0) >= 5:
        statuses = Counter(p['characterMatch'] for row in rows for p in row['predictions'])
        link_stats = dict(linkedInkGroups=statuses['match'] + statuses['accepted-form'] + statuses['different'],
                          characterLinkMismatches=statuses['different'], ambiguousInkGroups=statuses['ambiguous'],
                          unmatchedInkGroups=statuses['unmatched'],
                          charactersWithoutInk=sum(len(row['charactersWithoutInk']) for row in rows),
                          characterLinkDifferingSubwords=sum(not s['characterLinksMatch'] for s in comparisons),
                          inventoryAndStructureDifferingSubwords=sum(not s['inventoryAndStructureMatch'] for s in comparisons))
    return dict(**link_stats, subwords=len(comparisons), matchingSubwords=sum(s['matches'] for s in comparisons),
                differingSubwords=sum(not s['matches'] for s in comparisons),
                textWords=len({s['wordId'] for s in assignment['subwords'] if any(o['role'] == 'letter' for o in s['owners'])}),
                signOnlyGroups=sum(not any(o['role'] == 'letter' for o in s['owners']) for s in assignment['subwords']),
                letters=sum(o['role'] == 'letter' for o in owners),
                differingLetters=sum(o['role'] == 'letter' and (not by_id[o['id']]['matches'] or
                                     by_id[o['id']].get('characterLinksMatch') is False) for o in owners),
                wordSigns=sum(o['role'] == 'word-sign' for o in owners),
                differingWordSigns=sum(o['role'] == 'word-sign' and (not by_id[o['id']]['matches'] or
                                       by_id[o['id']].get('characterLinksMatch') is False) for o in owners),
                expectedMarks=sum(s['type'] != 'small-kaf-body' for o in owners for s in o['expectedSlots']),
                auxiliaryBodyGroups=sum(s['type'] == 'small-kaf-body' for o in owners for s in o['expectedSlots']),
                classifiedGroups=len(assignment['groups']), sourcePaths=len(assignment['paths']),
                decomposedNativePaths=len({p['sourcePathId'] for p in assignment.get('assignmentComponents', [])}),
                decomposedSubwords=sum(bool(s.get('markDecomposition')) for s in assignment['subwords']),
                missing=dict(missing), extra=dict(extra),
                contextualHamzaOwners=sum(bool(r.get('acceptedHamzaForms')) for r in rows),
                contextualHamzaEquivalences=sum(e['count'] for r in rows for e in r.get('contextualEquivalences', [])),
                waqfReservationAppliedSubwords=sum(s.get('waqfReservation', {}).get('status') == 'applied' for s in assignment['subwords']),
                waqfReservationFallbackSubwords=sum(s.get('waqfReservation', {}).get('status') == 'fallback' for s in assignment['subwords']),
                waqfReservationUnchangedSubwords=sum(s.get('waqfReservation', {}).get('status') == 'unchanged' for s in assignment['subwords']),
                contourDifferences=sum(not r['contourMatches'] for r in rows),
                groupingDifferences=sum(not r['groupingMatches'] for r in rows),
                geometricBoundaryFallbackOwners=sum(o.get('geometricGrouping', {}).get('method') ==
                                                    'existing-owner-boundary-conflict' for o in owners),
                unusedPaths=sum(len(s['unusedPathIds']) for s in assignment['subwords']),
                subwordsWithMatchingAggregateButLetterDifferences=aggregate_match,
                identityDifferingSubwords=sum(any(not r['matches'] for r in s['owners']) for s in comparisons),
                structureOnlyDifferingSubwords=sum(not s.get('inventoryAndStructureMatch', s['matches']) and
                                                   all(r['matches'] for r in s['owners']) for s in comparisons),
                structuralDifferingSubwords=sum(bool(s['unusedPathIds']) or any(o['issues'] or o['groupingIssues'] for o in s['owners']) for s in assignment['subwords']),
                ignoredSignPaths=sum(len(s['pathIds']) for s in assignment['ignoredSignGroups']),
                carrierPositions=sum(o.get('carrier', False) for o in owners),
                detachedGroupsNormalized=sum(len(s.get('nativeSubwordIds', [s['id']]))-1 for s in assignment['subwords']),
                verticalAdjustedSubwords=sum(s.get('verticalAssignment', {}).get('status') == 'applied' for s in assignment['subwords']),
                verticalPartialAdjustedSubwords=sum(s.get('verticalAssignment', {}).get('status') == 'applied' and
                    s['verticalAssignment'].get('ambiguousReservation', {}).get('status') == 'reserved' for s in assignment['subwords']),
                verticalFallbackSubwords=sum(s.get('verticalAssignment', {}).get('status') == 'fallback' for s in assignment['subwords']),
                verticalChangedOwners=sum(len(s.get('verticalAssignment', {}).get('changedOwnerIds', [])) for s in assignment['subwords']),
                nativeWaqfFlagsRestored=len(assignment['provenance'].get('waqfTypeRepairs', [])),
                bodySelectionEligibleSubwords=sum(s.get('bodySelection', {}).get('status') in ('applied', 'unchanged', 'fallback') for s in assignment['subwords']),
                bodySelectionAdjustedSubwords=sum(s.get('bodySelection', {}).get('status') == 'applied' for s in assignment['subwords']),
                bodySelectionFallbackSubwords=sum(s.get('bodySelection', {}).get('status') == 'fallback' for s in assignment['subwords']),
                unownedPaths=len(assignment['unownedSourcePathIds']))


def read_native_assignment(file, raw_hash, transcript, geometry_hash):
    assignment = json.loads(file.read_text())
    provenance = assignment.get('nativeAssignment', {})
    if provenance.get('producer') != 'medinamushafanalyzer-native-marks-v1':
        raise ValueError('Missing native C++ assignment provenance; run --assign-marks first')
    if (provenance.get('rawInputSha256') != raw_hash or
            provenance.get('transcriptSha256') != digest_json(transcript) or
            provenance.get('geometrySha256') != geometry_hash):
        raise ValueError('Native assignment belongs to different source, transcript or geometry')
    if assignment['pageId'] != transcript['pageId']:
        raise ValueError('Native assignment page differs from transcript')
    return assignment


def write_page(file, transcript, inputs, output, classifier_repo, support, collection, resume=False,
               assignments=None):
    stem = file.name.split('.')[0]
    raw_hash = digest(file)
    geometry_file = inputs/'geometry'/f'{stem}.json'
    bank_file = inputs/'geometry/shape-bank.json'
    geometry_hash = digest(geometry_file) if geometry_file.exists() else None
    bank_hash = digest(bank_file) if bank_file.exists() else None
    native_file = (assignments or inputs/'native-assignments')/f'{stem}.assignment.json'
    if not native_file.exists():
        raise ValueError(f'Missing {native_file}; generate native assignments with --assign-marks')
    native_hash = digest(native_file)
    result_path = output/f'{stem}.json'
    if resume and result_path.exists():
        previous = json.loads(result_path.read_text())
        provenance = previous['provenance']
        required = [output/f'{stem}{suffix}' for suffix in ('.assignment.json', '.predictions.json', '.differences.json', '.html')]
        if (provenance.get('rawInputSha256') == raw_hash and
                provenance.get('transcriptSnapshotSha256') == support['transcriptSha256'] and
                provenance.get('groupingGeometrySha256') == geometry_hash and
                provenance.get('shapeBankSha256') == bank_hash and
                provenance.get('nativeAssignmentFileSha256') == native_hash and
                provenance.get('reportCodeHashes') == support['files'] and
                previous['classifier']['modelFiles'] == support['models'] and
                previous.get('collection', False) == collection and all(p.exists() for p in required)):
            return collect_page(previous, digest(result_path))
    assignment = read_native_assignment(native_file, raw_hash, transcript, geometry_hash)
    assignment_hash = digest_json(assignment)
    # Ownership exists on disk before learned classification can run.
    write_json(output/f'{stem}.assignment.json', assignment)
    group_ids = {f'region{i}': g['id'] for i, g in enumerate(assignment['groups'])}
    regions = [dict(id=f'region{i}', **{k: g[k] for k in ('runs', 'bbox', 'baseline')})
               for i, g in enumerate(assignment['groups'])]
    classified = classify_regions(regions, painted_source_paths(assignment),
                                  assignment['raster'], support['modelRoot'], classifier_repo)
    for prediction in classified['predictions']:
        prediction['id'] = group_ids[prediction['id']]
    if digest_json(assignment) != assignment_hash:
        raise ValueError('Classifier changed the frozen assignment')
    comparisons = compare_predictions(assignment, classified['predictions'])
    if digest_json(assignment) != assignment_hash:
        raise ValueError('Validation changed the frozen assignment')
    if classified['provenance']['sourceInkSha256'] != assignment['conservation']['inkSha256']:
        raise ValueError('Classifier context differs from frozen source ink')
    summary = summarize(assignment, comparisons)
    result = {**assignment, 'summary': summary, 'validation': comparisons,
              'classifier': classified['provenance'], 'assignmentSha256': assignment_hash,
              'validationPolicy': dict(contextualHamza='For ئِ, accept hamza-above or hamza-below for one expected hamza. Preserve raw classifier labels; apply equivalence only within the assigned letter after inference.'),
              'assignmentUnchangedAfterValidation': True, 'collection': collection,
              'provenance': {**assignment['provenance'], 'rawInputSha256': raw_hash,
                             'transcriptSnapshotSha256': support['transcriptSha256'],
                             'groupingGeometrySha256': geometry_hash,
                             'shapeBankSha256': bank_hash,
                             'nativeAssignmentFileSha256': native_hash,
                             'reportCodeHashes': support['files']}}
    write_json(output/f'{stem}.predictions.json', classified)
    write_json(result_path, result)
    write_json(output/f'{stem}.differences.json', dict(pageId=stem, summary=summary,
               assignmentSha256=assignment_hash, subwords=[s for s in comparisons if not s['matches']]))
    template = Path(__file__).with_name('mark_assignment_report.html').read_text()
    font = Path(__file__).with_name('mark_assignment_report.woff')
    template = template.replace('__REPORT_FONT_DATA__', base64.b64encode(font.read_bytes()).decode('ascii'))
    encoded = json.dumps(result, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
    (output/f'{stem}.html').write_text(template.replace('__REPORT_JSON__', encoded.replace('<', '\\u003c')))
    return collect_page(result, digest(result_path))


def page_selection(value):
    selected = set()
    for part in value.split(','):
        limits = ('1-604' if part == 'all' else part).split('-')
        first, last = int(limits[0]), int(limits[-1])
        if len(limits) > 2 or not 1 <= first <= last <= 604:
            raise ValueError('Pages must be 1-604, ranges, or all')
        selected.update(f'page{n:03d}' for n in range(first, last+1))
    return sorted(selected)


def write_report(inputs, output, classifier_repo=CLASSIFIER, *, pages=None, workers=1, resume=False,
                 assignments=None, model_root=None):
    inputs, output = Path(inputs).resolve(), Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    transcript = validate_transcript(json.loads((inputs/'transcript.json').read_text()))
    selected = page_selection(pages) if pages else sorted(transcript)
    files = {p.name.split('.')[0]: p for p in inputs.glob('page*.raw.json')}
    model_root = model_root if model_root is not None else inputs/'classifier'
    support = dict(transcriptSha256=digest(inputs/'transcript.json'), models=model_files(model_root),
                   modelRoot=model_root,
                   files={name: digest(Path(__file__).with_name(name)) for name in (
                       'audit_mark_assignments.py', 'mark_validation.py', 'classify_assigned_marks.py',
                       'mark_assignment_report.html', 'mark_assignment_report.woff',
                       'mark_assignment_collection.py', 'mark_assignment_collection.html')})
    results, failures, jobs = [], [], []
    for pid in selected:
        if pid not in transcript or pid not in files:
            failures.append(dict(pageId=pid, error='Missing native export or transcript'))
        else:
            jobs.append((files[pid], transcript[pid], inputs, output, classifier_repo, support, len(selected)>1, resume,
                         Path(assignments).resolve() if assignments else None))
    def record(pid, result=None, error=None):
        if error is not None:
            failure = dict(pageId=pid, error=f'{type(error).__name__}: {error}')
            failures.append(failure)
            write_json(output/f'{pid}.failure.json', failure)
            print(f'{pid}: FAILED: {failure["error"]}', flush=True)
        else:
            results.append(result)
            failure_file = output/f'{pid}.failure.json'
            if failure_file.exists():
                failure_file.unlink()
            count = result['summary']['differingSubwords']
            print(f'{pid}: {count} differing subwords; {len(results)+len(failures)}/{len(selected)} pages', flush=True)
    if workers > 1 and len(jobs) > 1:
        with ProcessPoolExecutor(max_workers=min(workers, len(jobs))) as pool:
            futures = {pool.submit(write_page, *job): job[1]['pageId'] for job in jobs}
            for future in as_completed(futures):
                pid = futures[future]
                try:
                    result = future.result()
                except Exception as error:
                    record(pid, error=error)
                else:
                    record(pid, result=result)
    else:
        for job in jobs:
            pid = job[1]['pageId']
            try:
                result = write_page(*job)
            except Exception as error:
                record(pid, error=error)
            else:
                record(pid, result=result)
    export = inputs/'export-provenance.json'
    manifest = write_collection(output, selected, results, failures,
                                json.loads(export.read_text()) if export.exists() else None)
    if len(selected) == 1 and results:
        (output/'index.html').write_bytes((output/f'{selected[0]}.html').read_bytes())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'output/mark-assignment-audit')
    parser.add_argument('--inputs', type=Path, help='Frozen native exports and classifier/models; validation reads no database')
    parser.add_argument('--assignments', type=Path, help='Native assignment output; defaults to inputs/native-assignments. No Python fallback.')
    parser.add_argument('--model-root', type=Path, help='Freeze these classifier checkpoints in the report')
    parser.add_argument('--classifier-repo', type=Path, default=CLASSIFIER)
    parser.add_argument('--svg-dir', type=Path, help='Optional artwork binding for catalog exports')
    parser.add_argument('--fresh-source', action='store_true', help='Native export parses AI/PDF instead of reading page caches')
    parser.add_argument('--pages', help='Fresh export defaults to 3; replay defaults to every transcript page')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--resume', action='store_true', help='Reuse reports only when inputs, checkpoints and code hashes match')
    parser.add_argument('--binary', type=Path, default=ROOT/'build/ninja-nmc/Release/medinamushafanalyzer')
    args = parser.parse_args()
    inputs = args.inputs
    fresh_export = inputs is None
    output_inputs = (args.output/'inputs').resolve()
    if inputs is None:
        if args.svg_dir is None and not args.fresh_source:
            parser.error('Use --fresh-source for AI/PDF-only export, or supply --svg-dir for cached workbench export')
        args.pages = args.pages or '3'
        inputs = output_inputs
        inputs.mkdir(parents=True, exist_ok=True)
        if list(inputs.glob('page*.raw.json')):
            parser.error('Fresh export needs an empty inputs directory; use --inputs to replay, or choose a new output')
        binary = args.binary.resolve()
        for source in ('quran.cpp', 'pageanalysisresult.cpp', 'pageanalysisresult.h', 'pathmask.h', 'proposalexport.cpp', 'afont.cpp', 'main.cpp',
                       'markassignment.cpp', 'markgeometry.cpp', 'markcharacterlinks.cpp', 'waqfrecognition.cpp',
                       'templates/mark-shapes.json', 'tools/waqf_templates.json'):
            if (ROOT/source).stat().st_mtime > binary.stat().st_mtime:
                parser.error(f'Native binary predates {source}; rebuild first')
        env = {**os.environ, 'QT_QPA_PLATFORM': 'offscreen'}
        args.assignments = args.assignments or inputs/'native-assignments'
        command = [str(binary), '--export-proposals', str(inputs),
                   '--export-mark-assignments', str(args.assignments), '--pages', args.pages]
        if args.svg_dir is not None:
            command.extend(('--svg-dir', str(args.svg_dir.resolve())))
        if args.fresh_source:
            command.append('--fresh-source')
        subprocess.run(command, cwd=ROOT, env=env, check=True)
        subprocess.run([str(binary), '--export-transcript', str(inputs/'transcript.json'), '--pages', args.pages], cwd=ROOT, env=env, check=True)
        validate_transcript(json.loads((inputs/'transcript.json').read_text()), (ROOT/'quran.cpp').read_text())
        write_json(inputs/'export-provenance.json', dict(binarySha256=digest(binary),
                   quranSourceSha256=digest(ROOT/'quran.cpp'), segmentationSourceSha256=digest(ROOT/'pageanalysisresult.cpp'),
                   exportSourceSha256=digest(ROOT/'proposalexport.cpp'), vectorMaskSourceSha256=digest(ROOT/'pathmask.h'),
                   fontMatchingSourceSha256=digest(ROOT/'afont.cpp'),
                   perPathTemplateScoresIncluded=True))
        if args.model_root is None:
            args.model_root = (DATA if args.classifier_repo == CLASSIFIER else args.classifier_repo/'data')/'automatic'
    inputs = inputs.resolve()
    if not fresh_export and (args.output.resolve().is_relative_to(inputs) or
                             (output_inputs != inputs and output_inputs.is_relative_to(inputs))):
        parser.error('Choose a report directory outside the archived inputs')
    model_root = inputs/'classifier'
    if args.model_root is not None:
        if not fresh_export and output_inputs.is_relative_to(inputs):
            parser.error('Use a new report directory for replacement checkpoints')
        model_root = output_inputs/'classifier'
        freeze_models(args.model_root, model_root)
    if not (model_root/'models/model.json').exists():
        parser.error('Frozen classifier is missing; supply --model-root to freeze the existing checkpoints')
    if output_inputs != inputs:
        output_inputs.mkdir(parents=True, exist_ok=True)
        transcript = inputs/'transcript.json'
        report_transcript = output_inputs/'transcript.json'
        if not report_transcript.exists():
            shutil.copyfile(transcript, report_transcript)
        elif digest(report_transcript) != digest(transcript):
            parser.error('Report transcript differs from the archive; choose a new report directory')
    manifest = write_report(inputs, args.output, args.classifier_repo, pages=args.pages, workers=max(1, args.workers),
                            resume=args.resume, assignments=args.assignments, model_root=model_root)
    print(json.dumps(manifest['summary'], ensure_ascii=False, indent=2))
    if manifest['summary']['failedPages']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
