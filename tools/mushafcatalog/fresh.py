"""Produce a reference report from the source edition or check a fresh run against one."""
import json
from pathlib import Path
import subprocess
from tempfile import NamedTemporaryFile, TemporaryDirectory

from store import REPORT, ROOT, manifest, require
from environment import validator
from input_lock import MARK_CLASSIFIER, check, file_sha, validation_models

from regenerate_native_marks_from_source import regenerate


def comparison_receipt(reference, candidate):
    baseline, reference_hash = manifest(reference)
    current, current_hash = manifest(candidate)
    run = json.loads((candidate/'fresh-run-provenance.json').read_bytes())
    return dict(referenceReport=str(reference), referenceManifestSha256=reference_hash,
                freshManifestSha256=current_hash, nativeBinarySha256=run['binarySha256'],
                transcriptSha256=file_sha(candidate/'inputs/transcript.json'), pages=len(current['pages']),
                baselineDifferingSubwords=baseline['summary']['differingSubwords'],
                freshDifferingSubwords=current['summary']['differingSubwords'], masks=current['summary']['classifiedGroups'],
                cacheNegativeControlPassed=run['cachedExportNegativeControlPassed'])


def save_verified(report, receipt):
    """Publish a small receipt only after the independent all-page comparison succeeds."""
    target = (ROOT/'output/mushafcatalog/verification.json' if report==REPORT
              else report.with_name(report.name+'.catalog-verification.json'))
    target.parent.mkdir(parents=True,exist_ok=True)
    with NamedTemporaryFile(mode='w',dir=target.parent,delete=False) as stream:
        json.dump(dict(passed=True, **receipt),stream,indent=2)
        stream.write('\n')
    Path(stream.name).replace(target)
    print(f'Comparison receipt: {target}',flush=True)


def replay_marks(inputs, report, pages, workers, model_root=None):
    inputs, report = inputs.resolve(), report.resolve()
    require((inputs/'native-assignments').is_dir() and (inputs/'transcript.json').is_file(),
            'Choose the inputs directory of a saved native mark report.')
    require(not report.exists() and not report.is_relative_to(inputs),
            'Choose a new report directory outside the archived inputs.')
    command = [str(validator()), str(MARK_CLASSIFIER/'audit_mark_assignments.py'),
               '--inputs', str(inputs), '--output', str(report), '--workers', str(workers)]
    if pages:
        command.extend(('--pages', pages))
    if model_root is not None:
        command.extend(('--model-root', str(model_root.resolve())))
    subprocess.run(command, cwd=ROOT, check=True)


def export_marks(candidate, pages, workers, python, models, pin):
    if pages == 'all':
        regenerate(candidate, None, None, workers, python, model_root=models)
    else:
        command = [str(python),str(MARK_CLASSIFIER/'audit_mark_assignments.py'),'--fresh-source',
                   '--output',str(candidate),'--pages',pages,'--model-root',str(models),'--workers',str(workers)]
        subprocess.run(command,cwd=ROOT,check=True)
    new, _ = manifest(candidate)
    sample = json.loads((candidate/(new['requestedPageIds'][0]+'.json')).read_bytes())
    embedded = sample['nativeAssignment']
    require(embedded['shapeBankSha256']==pin['nativeFiles']['templates/mark-shapes.json'] and
            embedded['waqfTemplatesSha256']==pin['nativeFiles']['tools/waqf_templates.json'] and
            sample['classifier']['modelFiles']==pin['validationModels'],
            'The compiled geometric templates or validation checkpoints differ from the input lock')
    if pages == 'all':
        require(file_sha(candidate/'inputs/transcript.json')==pin['transcriptSha256'],
                'Native QuranTextByWord export differs from the pinned transcript')
    return new


def audit_marks(report, pages, workers, model_root=None, input_lock=None):
    report = report.resolve()
    require(not report.exists(), 'Choose a new --report directory, or use replay-marks for saved inputs.')
    models = validation_models(model_root)
    pin = check(models,input_lock,full=pages=='all')
    python = validator()
    report.parent.mkdir(parents=True,exist_ok=True)
    with TemporaryDirectory(dir=report.parent,prefix='.mark-audit-') as temporary:
        candidate = Path(temporary)/'report'
        new = export_marks(candidate,pages,workers,python,models,pin)
        candidate.rename(report)
    print(f"Published {len(new['pages'])} AI/PDF mark audit pages in {report}",flush=True)


def fresh(report, pages, workers, model_root=None, input_lock=None):
    report = report.resolve()
    existed = report.exists()
    report.parent.mkdir(parents=True, exist_ok=True)
    models = validation_models(model_root)
    if existed:
        old, _ = manifest(report)
        sample = json.loads((report/(old['requestedPageIds'][0]+'.json')).read_bytes())
        require(sample['provenance'].get('sourceFormat')=='ai-pdf',
                'Choose a new --report directory for the AI/PDF export.')
    pin = check(models,input_lock,full=pages=='all')
    python = validator()
    with TemporaryDirectory(dir=report.parent, prefix='.catalog-source-') as temporary:
        candidate = Path(temporary)/'report'
        new = export_marks(candidate,pages,workers,python,models,pin)
        if existed:
            require(old['requestedPageIds']==new['requestedPageIds'], 'Existing report has different page coverage; use another --report directory.')
            if pages == 'all':
                from check_fresh_mark_regression import compare
                comparison = compare(report,candidate)
                require(comparison['passed'], 'Fresh run differs from the saved report: '+str(comparison['failures'][:10])+
                        '. Use a new --report directory to retain an intentional change.')
                save_verified(report, comparison_receipt(report,candidate))
            else:
                from check_fresh_mark_regression import normalize_waqf_links
                require(json.loads((report/'inputs/transcript.json').read_bytes()) ==
                        json.loads((candidate/'inputs/transcript.json').read_bytes()), 'Fresh transcript changed')
                for pid in new['requestedPageIds']:
                    a,b = [json.loads((d/f'{pid}.json').read_bytes()) for d in (report,candidate)]
                    normalize_waqf_links(a)
                    normalize_waqf_links(b)
                    fields = ('words','subwords','paths','groups','ignoredSignGroups','raster','classifier','validation','conservation')
                    for key in fields:
                        require(a[key]==b[key],f'{pid}: fresh {key} changed')
            print(f"Verified {len(new['pages'])} pages against {report}", flush=True)
        else:
            candidate.rename(report)
            print(f"Published {len(new['pages'])} native pages in {report}", flush=True)
