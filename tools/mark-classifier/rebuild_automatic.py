#!/usr/bin/env python3
"""Export portable trained models, or rebuild every automatic cache from empty."""
from workbench_paths import NATIVE
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT=Path(__file__).resolve().parent


def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def empty_directory(path):
    path=Path(path).resolve()
    if path.exists() and any(path.iterdir()):raise ValueError(f'Refusing to overwrite a nonempty directory: {path}')
    path.mkdir(parents=True,exist_ok=True)
    return path


def export_models(root,destination):
    root=Path(root).resolve();destination=empty_directory(destination);models=destination/'models';models.mkdir()
    main=json.loads((root/'models/model.json').read_text());baseline=digest(root/'models/selected.npz')
    if baseline!=main['modelSha256']:raise ValueError('Base checkpoint does not match its vocabulary metadata')
    for name in ('model.json','selected.npz'):shutil.copy2(root/'models'/name,models/name)
    for name in ('hamza','damma-yeh'):
        source=root/'models'/f'{name}.json'
        if not source.exists():continue
        config=json.loads(source.read_text());weights=root/config['weights']
        if config['baselineSha256']!=baseline or config['weightsSha256']!=digest(weights):
            raise ValueError(f'{name} activation does not match its checkpoints')
        shutil.copy2(weights,models/f'{name}.npz');config['weights']=f'models/{name}.npz'
        (models/f'{name}.json').write_text(json.dumps(config,indent=2))
    manifest={'schemaVersion':1,'baseModelSha256':baseline,
        'files':{str(p.relative_to(destination)):digest(p) for p in sorted(models.iterdir())},
        'policy':'Trained checkpoints and vocabularies are immutable inference inputs. Expert paths are relative; no experiment directory is needed.'}
    (destination/'bundle.json').write_text(json.dumps(manifest,indent=2));return manifest


def validate_bundle(bundle):
    bundle=Path(bundle).resolve();manifest=json.loads((bundle/'bundle.json').read_text())
    if manifest['schemaVersion']!=1:raise ValueError('Unsupported model bundle')
    for name,sha in manifest['files'].items():
        path=(bundle/name).resolve()
        if not path.is_relative_to(bundle/'models') or digest(path)!=sha:
            raise ValueError(f'Model bundle changed: {name}')
    return manifest


def verify_rebuilds(first,second):
    """Compare independent builds, excluding report and label-export clocks."""
    import sqlite3
    first,second=Path(first).resolve(),Path(second).resolve()
    a=json.loads((first/'rebuild-manifest.json').read_text());b=json.loads((second/'rebuild-manifest.json').read_text())
    if a['status']!=b['status'] or a['status']!='complete':raise ValueError('Both rebuilds must be complete')
    if a['sourceInputs']!=b['sourceInputs'] or a['modelBundle']!=b['modelBundle']:
        raise ValueError('Rebuild input snapshots or model bundles differ')
    checks={}
    for name in ('features.u8','embeddings.f16','corpus.sqlite','scope.json','inference.json','baselines.json'):
        if digest(first/name)!=digest(second/name):raise ValueError('Rebuilt artifact differs: '+name)
        checks[name]=True
    for name in ('pages','proposals','render','scope','models'):
        left={p.relative_to(first/name):p for p in (first/name).rglob('*') if p.is_file() and not p.name.startswith('.')}
        right={p.relative_to(second/name):p for p in (second/name).rglob('*') if p.is_file() and not p.name.startswith('.')}
        if not left or left.keys()!=right.keys() or any(digest(p)!=digest(right[key]) for key,p in left.items()):
            raise ValueError('Rebuilt folder differs: '+name)
        checks[name]={'files':len(left),'allByteIdentical':True}
    with sqlite3.connect((first/'training-labels/labels.sqlite').as_uri()+'?mode=ro',uri=True) as db:
        db.execute('ATTACH DATABASE ? AS replay',((second/'training-labels/labels.sqlite').as_uri()+'?mode=ro',))
        for table in ('targets','corrections'):
            for left,right in ((table,'replay.'+table),('replay.'+table,table)):
                if db.execute(f'SELECT COUNT(*) FROM (SELECT * FROM {left} EXCEPT SELECT * FROM {right})').fetchone()[0]:
                    raise ValueError('Frozen training records differ: '+table)
    manifests=[]
    for root in (first,second):
        manifest=json.loads((root/'training-labels/manifest.json').read_text())
        manifest.pop('createdAt');manifest.pop('labelsSha256');manifests.append(manifest)
    if manifests[0]!=manifests[1]:raise ValueError('Training geometry or feature fingerprints differ')
    checks.update(frozenTargetsAndCorrectionsIdentical=True,independentEmptyBuilds=2,
        labelSnapshotDifference='Creation timestamps and resulting label SQLite hashes are excluded; targets and fingerprints must match.',
        codeFilesChanged=[name for name in a['codeSha256'].keys()|b['codeSha256'].keys() if a['codeSha256'].get(name)!=b['codeSha256'].get(name)],
        first=str(first),second=str(second))
    return checks


def snapshot_inputs(data,output,raw_root):
    data=Path(data).resolve();inputs=output/'inputs';inputs.mkdir()
    shutil.copy2(data/'corpus.json',inputs/'corpus.json')
    for name in ('annotations','proposals'):
        target=inputs/name;target.mkdir()
        for path in sorted((data/name).glob('page*.json')):shutil.copy2(path,target/path.name)
    corpus=json.loads((inputs/'corpus.json').read_text());ids=sorted(p['id'] for p in corpus['pages'])
    if ids!=sorted(p.stem for p in (inputs/'proposals').glob('page*.json')):
        raise ValueError('Every indexed page needs its original manual-review proposals')
    raw={page:digest(Path(raw_root)/f'{page}.raw.json') for page in ids}
    return {'snapshotFiles':{str(p.relative_to(inputs)):digest(p) for p in sorted(inputs.rglob('*.json'))},'rawVectors':raw}


def rebuild(args):
    bundle=Path(args.model_bundle).resolve();models=validate_bundle(bundle)
    output=empty_directory(args.output);inputs=snapshot_inputs(args.data_dir,output,args.raw_root)
    shutil.copytree(bundle/'models',output/'models')
    code=[*sorted((PROJECT/'automatic').glob('*.py')),*sorted((PROJECT/'automatic').glob('*.sql')),PROJECT/'automatic_pipeline.py',
          PROJECT/'rebuild_automatic.py',
          *sorted((NATIVE/'tools/vector_review').glob('*.py')),
          NATIVE/'tools/prepare_vector_review.py']
    manifest={'schemaVersion':1,'modelBundle':models,'sourceInputs':inputs,
        'rawRoot':str(Path(args.raw_root).resolve()),'pdfRoot':str(Path(args.pdf_root).resolve()),
        'codeSha256':{str(p.relative_to(PROJECT.parent)):digest(p) for p in code},
        'policy':'Fresh output only. No previous database, features, automatic proposals, renders or page caches are read. Saved human annotations are inputs; no approvals are inferred.',
        'commands':[],'status':'running'}
    (output/'rebuild-manifest.json').write_text(json.dumps(manifest,indent=2))
    env={**os.environ,'OMP_NUM_THREADS':'4','OPENBLAS_NUM_THREADS':'4','VECLIB_MAXIMUM_THREADS':'4'}
    def run(script,*options):
        command=[sys.executable,str(PROJECT/script),*map(str,options)]
        manifest['commands'].append(command)
        (output/'rebuild-manifest.json').write_text(json.dumps(manifest,indent=2))
        subprocess.run(command,cwd=PROJECT,env=env,check=True)
    try:
        run('automatic_pipeline.py','build','--data-dir',output/'inputs','--output',output,
            '--raw-root',Path(args.raw_root).resolve(),'--pdf-root',Path(args.pdf_root).resolve(),'--jobs',args.jobs)
        run('automatic_pipeline.py','freeze-labels','--output',output)
        run('automatic_pipeline.py','infer','--output',output)
        if args.mark_report:
            from automatic.transcript_marks import import_report
            manifest['nativeMarkIndex'] = import_report(output/'corpus.sqlite',args.mark_report)
        if args.reports:
            run('audit_low_scores.py','--root',output)
            run('audit_words.py','--root',output)
        for page,sha in inputs['rawVectors'].items():
            if digest(Path(args.raw_root)/f'{page}.raw.json')!=sha:raise ValueError('Raw analyzer input changed during rebuild')
        for path,sha in inputs['snapshotFiles'].items():
            if digest(output/'inputs'/path)!=sha:raise ValueError('Frozen input changed during rebuild')
        manifest['artifacts']={name:digest(output/name) for name in ('corpus.sqlite','features.u8','embeddings.f16','proposals/teacher-bank.json','training-labels/labels.sqlite')}
        manifest['status']='complete'
    except Exception as error:
        manifest['status']='failed';manifest['error']=str(error);raise
    finally:(output/'rebuild-manifest.json').write_text(json.dumps(manifest,indent=2))
    print('Fresh reproducible corpus:',output,flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='command',required=True)
    export=commands.add_parser('export-models');export.add_argument('--root',type=Path,default=PROJECT/'data/automatic');export.add_argument('--bundle',type=Path,required=True)
    verify=commands.add_parser('verify');verify.add_argument('first',type=Path);verify.add_argument('second',type=Path);verify.add_argument('--report',type=Path)
    build=commands.add_parser('rebuild');build.add_argument('--model-bundle',type=Path,required=True);build.add_argument('--output',type=Path,required=True)
    build.add_argument('--data-dir',type=Path,default=PROJECT/'data');build.add_argument('--raw-root',type=Path,default=NATIVE/'output/vector-review')
    build.add_argument('--pdf-root',type=Path,default=NATIVE/'input/1441-AI-hafs');build.add_argument('--jobs',type=int,choices=range(1,13),default=6)
    build.add_argument('--reports',action='store_true',help='Also regenerate the low-score and word audits.')
    build.add_argument('--mark-report',type=Path,help='Index native transcript-linked masks from a completed all-page report.')
    args=parser.parse_args()
    if args.command=='export-models':print(json.dumps(export_models(args.root,args.bundle),indent=2))
    elif args.command=='verify':
        result=verify_rebuilds(args.first,args.second)
        if args.report:args.report.write_text(json.dumps(result,indent=2))
        print(json.dumps(result,indent=2))
    else:rebuild(args)


if __name__=='__main__':main()
