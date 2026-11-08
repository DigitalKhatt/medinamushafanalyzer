#!/usr/bin/env python3
"""Orchestrate cache-free C++ segmentation, then run the Python validator.

The native working directory contains only links to the source AI/PDF directory
and the fixed geometric font templates. No segmentation caches or prior exports
are supplied to the native processes. Python never assigns or groups marks.
"""
import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n')


def regenerate(output, baseline, svg_directory, workers, python, resume_interrupted=False, model_root=None):
    output = output.resolve()
    svg_directory = svg_directory.resolve() if svg_directory is not None else None
    svg_args = ['--svg-dir', str(svg_directory)] if svg_directory is not None else []
    if baseline is None and model_root is None:
        raise ValueError('Supply validation checkpoints or a baseline report')
    baseline = baseline.resolve() if baseline else None
    from classify_assigned_marks import freeze_models, model_files
    models = model_root.resolve() if model_root else baseline/'inputs/classifier'
    if output.exists() and not resume_interrupted:
        raise ValueError('Use a new output directory for a genuinely fresh run')
    output.mkdir(parents=True, exist_ok=resume_interrupted)
    built = ROOT/'build/ninja-nmc/Release/medinamushafanalyzer'
    binary = output/'native/medinamushafanalyzer'
    binary.parent.mkdir(exist_ok=True)
    if not binary.exists():
        shutil.copy2(built, binary)
    runtime = output/'native-run'; (runtime/'input').mkdir(parents=True, exist_ok=resume_interrupted)
    for name in ('font.dat', '1441-AI-hafs'):
        link = runtime/'input'/name
        target = (ROOT/'input'/name).resolve()
        if resume_interrupted:
            if not link.is_symlink() or link.resolve() != target:
                raise ValueError('Interrupted run has different native source links')
        else:
            link.symlink_to(target, target_is_directory=target.is_dir())
    inputs = output/'inputs'; inputs.mkdir(exist_ok=resume_interrupted)
    (inputs/'native-assignments').mkdir(exist_ok=resume_interrupted)
    logs = output/'verification-logs'; logs.mkdir(exist_ok=resume_interrupted)
    env = {**os.environ, 'QT_QPA_PLATFORM': 'offscreen'}
    started = time.time()
    provenance = dict(binarySha256=digest(binary), baselineManifestSha256=digest(baseline/'manifest.json') if baseline else None,
                      nativeWorkingDirectory=str(runtime), nativeWorkingDirectoryEntries=['input'],
                      nativeInputs=['AI/PDF source pages', *(['source SVG viewBoxes/hashes'] if svg_args else []),
                                    'fixed geometric font and embedded mark templates', 'compiled QuranTextByWord'],
                      segmentationCachesAvailable=False, classifierUsedForSegmentation=False,
                      modelSource='Frozen validation checkpoints', modelFiles=model_files(models), workers=workers,
                      commandMode='--fresh-source --export-proposals --export-mark-assignments')
    if resume_interrupted:
        previous = json.loads((output/'fresh-run-provenance.json').read_text())
        for key in ('binarySha256', 'baselineManifestSha256', 'workers', 'nativeWorkingDirectory', 'nativeInputs'):
            if previous[key] != provenance[key]:
                raise ValueError(f'Interrupted run changed {key}; start a new run instead')
        if previous.get('modelFiles', provenance['modelFiles']) != provenance['modelFiles']:
            raise ValueError('The validation checkpoints changed since the interrupted run')
        provenance['resumedInterruptedFreshRun'] = True
        provenance['completedFreshPagesRetained'] = 0
    write(output/'fresh-run-provenance.json', provenance)

    # A negative control proves the normal cached export cannot run in this cwd.
    control = [str(binary), '--export-proposals', str(output/'cache-negative-control'), *svg_args, '--pages', '3']
    result = subprocess.run(control, cwd=runtime, env=env, capture_output=True, text=True)
    (logs/'cache-negative-control.log').write_text(result.stdout+result.stderr)
    if result.returncode == 0 or 'Cannot read pages/page3.dat' not in result.stderr:
        raise RuntimeError('The cache-free working-directory negative control did not fail as expected')
    assert not (runtime/'pages').exists()
    provenance['cachedExportNegativeControlPassed'] = True
    write(output/'fresh-run-provenance.json', provenance)

    with (logs/'transcript.log').open('w') as log:
        subprocess.run([str(binary), '--export-transcript', str(inputs/'transcript.json'), '--pages', 'all'],
                       cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    from audit_mark_assignments import validate_transcript
    transcript = json.loads((inputs/'transcript.json').read_bytes())
    validate_transcript(transcript, (ROOT/'quran.cpp').read_text())
    if baseline and transcript != json.loads((baseline/'inputs/transcript.json').read_bytes()):
        raise RuntimeError('The native transcript changed since the reference report')
    del transcript

    def batch(index):
        numbers = list(range(index+1, 605, workers))
        target = output/'native-batches'/f'batch{index+1:02d}';target.mkdir(parents=True, exist_ok=resume_interrupted)
        retained, pending = [], []
        for number in numbers:
            pid = f'page{number:03d}'
            raw, assignment = target/f'{pid}.raw.json', target/'assignments'/f'{pid}.assignment.json'
            if resume_interrupted and raw.exists() and assignment.exists():
                doc, result = json.loads(raw.read_text()), json.loads(assignment.read_text())
                native = result['nativeAssignment']
                source = runtime/'input/1441-AI-hafs'/f'{number:03d}___Hafs39__DM.ai'
                pdf_hash = digest(source)
                source_hash = digest(svg_directory/f'{pid}.svg') if svg_directory is not None else pdf_hash
                if (doc['provenance']['cacheSha256'] is not None or
                        not doc['provenance']['cacheOrigin'].startswith('fresh AI/PDF') or
                        doc['provenance']['pdfSha256'] != pdf_hash or
                        doc['provenance']['templateSha256'] != digest(runtime/'input/font.dat') or
                        doc['sourceSha256'] != source_hash or
                        native['binarySha256'] != provenance['binarySha256'] or
                        native['rawInputSha256'] != digest(raw)):
                    raise RuntimeError(f'{pid}: previously completed fresh output no longer matches its sources')
                retained.append(dict(pageId=pid, words=len(doc['words']), paths=len(doc['paths'])))
            else:
                pending.append(number)
        command = [str(binary), '--export-proposals', str(target), '--export-mark-assignments',
                   str(target/'assignments'), *svg_args,
                   '--pages', ','.join(map(str, pending)), '--fresh-source']
        suffix = '-resumed' if resume_interrupted else ''
        write(target/f'command{suffix}.json', dict(command=command, cwd=str(runtime), pages=pending,
              retainedFreshPageIds=[p['pageId'] for p in retained]))
        if pending:
            with (logs/f'batch{index+1:02d}{suffix}.log').open('w') as log:
                completed = subprocess.run(command, cwd=runtime, env=env, stdout=log, stderr=subprocess.STDOUT)
            manifest = json.loads((target/'raw-manifest.json').read_text())
            code = completed.returncode
        else:
            manifest = dict(pages=[], failures=[]);code = 0
        manifest['pages'] = sorted(retained+manifest['pages'], key=lambda p: p['pageId'])
        write(target/'complete-manifest.json', manifest)
        return dict(batch=index+1, returncode=code, retainedFreshPages=len(retained), **manifest)

    results = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        pending = {pool.submit(batch, i) for i in range(workers)}
        while pending:
            completed, pending = wait(pending, timeout=60, return_when=FIRST_COMPLETED)
            for future in completed:
                result = future.result();results.append(result)
                write(output/'native-batch-status.json', dict(completedBatches=results, expectedBatches=workers))
            count = sum(1 for _ in (output/'native-batches').glob('batch*/assignments/page*.assignment.json'))
            print(f'Native assignments exported for {count}/604 pages', flush=True)
    provenance['nativeSeconds'] = round(time.time()-started, 3)
    provenance['completedFreshPagesRetained'] = sum(r['retainedFreshPages'] for r in results)
    failures = [f for result in results for f in result['failures']]
    page_records = sorted([p for result in results for p in result['pages']], key=lambda p: p['pageId'])
    write(inputs/'raw-manifest.json', dict(pages=page_records, failures=failures))
    expected = [f'page{number:03d}' for number in range(1, 605)]
    if failures or any(r['returncode'] for r in results) or [p['pageId'] for p in page_records] != expected:
        write(output/'fresh-run-provenance.json', provenance)
        raise RuntimeError(f'Fresh native export produced {len(page_records)}/604 pages. First failures: {failures[:3]}')
    for target in sorted((output/'native-batches').glob('batch*')):
        for source in target.glob('page*.raw.json'):
            doc = json.loads(source.read_text())
            if doc['provenance']['cacheSha256'] is not None or not doc['provenance']['cacheOrigin'].startswith('fresh AI/PDF'):
                raise RuntimeError(f'{source.name}: native provenance reports cache usage')
            source.rename(inputs/source.name)
        for source in (target/'assignments').glob('page*.assignment.json'):
            source.rename(inputs/'native-assignments'/source.name)
    write(inputs/'native-assignments/native-assignment-manifest.json', dict(
        producer='medinamushafanalyzer-native-marks-v1', pages=expected, failures=[],
        pythonAssignmentUsed=False, freshSource=True, segmentationCachesAvailable=False))
    assert not (runtime/'pages').exists()
    provenance['completedNativePages'] = len(page_records)
    provenance['perPathTemplateScoresIncluded'] = True
    write(output/'fresh-run-provenance.json', provenance)
    write(inputs/'export-provenance.json', provenance)

    # Checkpoints are validation inputs only, copied after native ownership is complete.
    freeze_models(models, inputs/'classifier')
    command = [str(python), str(Path(__file__).with_name('audit_mark_assignments.py')), '--inputs', str(inputs),
               '--output', str(output), '--pages', 'all', '--workers', '4']
    print('Native export complete; validating all 604 pages', flush=True)
    with (logs/'validation.log').open('w') as log:
        subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    provenance['totalSeconds'] = round(time.time()-started, 3)
    write(output/'fresh-run-provenance.json', provenance)
    print('Fresh source report complete', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    reference = parser.add_mutually_exclusive_group(required=True)
    reference.add_argument('--baseline', type=Path, help='Reference transcript and validation models')
    reference.add_argument('--model-root', type=Path, help='Portable validation bundle for a build from zero')
    parser.add_argument('--svg-dir', type=Path, help='Optional artwork binding for catalog exports')
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--python', type=Path, default=ROOT.parent/'mushafanalyzer/.venv/bin/python')
    parser.add_argument('--resume-interrupted', action='store_true',
                        help='Keep only completed fresh outputs after verifying their exact source and binary hashes; never read them as native segmentation inputs')
    args = parser.parse_args()
    if not 1 <= args.workers <= 16:
        parser.error('--workers must be between 1 and 16')
    regenerate(args.output, args.baseline, args.svg_dir, args.workers, args.python, args.resume_interrupted,
               model_root=args.model_root)
