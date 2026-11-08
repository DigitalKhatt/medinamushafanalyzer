#!/usr/bin/env python3
"""Regenerate all native letter cuts into an isolated, resumable audit snapshot."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[2]

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run(output, executable, jobs=4, templates=None, page_timeout=180):
    if page_timeout <= 0:raise ValueError('Page timeout must be positive')
    output=output.resolve();output.mkdir(parents=True,exist_ok=True)
    executable=executable.resolve()
    engine_hash=digest(ROOT/'segmentsubword.cpp');executable_hash=digest(executable)
    previous=output/'generation.json'
    generation={}
    if previous.exists():
        generation=json.loads(previous.read_text())
        if generation.get('segmenterSha256')!=engine_hash or generation.get('exporterSha256')!=executable_hash:
            raise ValueError('Cutter or executable changed: use a new audit directory')
    for name in ['pages','logs','runtime','runtime/output']:(output/name).mkdir(exist_ok=True)
    config=output/'letter-forms.json';original=Path(templates).resolve() if templates else ROOT/'templates/letter-forms.json'
    if not config.exists():shutil.copyfile(original,config)
    config_hash=digest(config)
    if config_hash!=digest(original) or generation.get('templateFileSha256',config_hash)!=config_hash:
        raise ValueError('Templates changed: use a new audit directory')
    template=json.loads(config.read_bytes())
    if template['kashidaDefaults'].get('trackingEnabled'):raise ValueError('Tracker must remain disabled')
    cache=output/'runtime/output/segmentation-cache'
    if not cache.exists():cache.symlink_to(output/'pages',target_is_directory=True)
    if cache.resolve()!=(output/'pages').resolve():raise ValueError('Unsafe cache location')
    font_path=ROOT/'input/font.dat'
    font_hash=digest(font_path)
    hashes={n:digest(ROOT/f'pages/page{n}.dat') for n in range(1,605)}
    receipts=generation.get('pageHashes',{}) if generation.get('fontFileSha256')==font_hash else {}
    verified={}
    def validate(raw,page):
        value=json.loads(raw)
        return (value.get('page')==page and value.get('sourceFileSha256')==hashes[page] and
                value.get('fontSha256')==font_hash and value.get('templateConfig')==template and
                isinstance(value.get('subwords'),list) and isinstance(value.get('sourceWords'),list))
    pending=[];completed=[];failures=[];started=time.monotonic()
    for page in range(1,605):
        path=output/'pages'/f'page{page:03}.json'
        if path.is_file():
            raw=path.read_bytes()
            signature={'source':hashes[page],'export':hashlib.sha256(raw).hexdigest()}
            prior=receipts.get(str(page))
            valid=prior==signature
            # Unrecorded files include legacy snapshots and pages saved just before an interruption.
            if prior is None:
                try:valid=validate(raw,page)
                except (ValueError, UnicodeError, AttributeError):valid=False
            if valid:
                verified[str(page)]=signature
                completed.append(page);continue
        pending.append(page)
    env={**os.environ,'QT_QPA_PLATFORM':'offscreen','MEDINA_LETTER_FORMS':str(config)}
    def one(page):
        try:
            with (output/'logs'/f'page{page:03}.log').open('w') as log:
                result=subprocess.run([str(executable),str(ROOT),str(output/'pages'),str(page)],cwd=output/'runtime',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=page_timeout)
            if result.returncode:raise RuntimeError(f'exporter exited {result.returncode}')
            raw=(output/'pages'/f'page{page:03}.json').read_bytes()
            if not validate(raw,page):raise ValueError('Incomplete export or changed inputs')
            return page,{'source':hashes[page],'export':hashlib.sha256(raw).hexdigest()},None
        except Exception as error:return page,None,str(error)
    def save():
        status={'schemaVersion':1,'pagesExpected':604,'pagesCompleted':len(completed),'completedPages':sorted(completed),'failures':failures,'elapsedSeconds':time.monotonic()-started,'templateFileSha256':config_hash,'segmenterSha256':engine_hash,'exporterSha256':executable_hash,'trackerEnabled':False,'provenance':'machine-template-audit','humanReviewed':False,'fontFileSha256':font_hash,'pageHashes':verified}
        temporary=output/'generation.json.tmp'
        temporary.write_text(json.dumps(status,indent=2)+'\n')
        temporary.replace(output/'generation.json')
    if pending:save()
    print(f'{len(completed)}/604 pages available; regenerating {len(pending)} with {jobs} workers',flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for future in as_completed([pool.submit(one,p) for p in pending]):
            page,signature,error=future.result()
            if error:failures.append({'page':page,'error':error});print(f'page{page:03}: {error}',flush=True)
            else:
                completed.append(page)
                verified[str(page)]=signature
            save()
            if (len(completed)+len(failures))%20==0:print(f'{len(completed)}/604 pages; {len(failures)} failures; {time.monotonic()-started:.0f}s',flush=True)
    if digest(config)!=config_hash or digest(original)!=config_hash:raise ValueError('Live templates changed during audit')
    if digest(font_path)!=font_hash:raise ValueError('Source font changed during audit')
    if digest(ROOT/'segmentsubword.cpp')!=engine_hash or digest(executable)!=executable_hash:raise ValueError('Cutter changed during audit')
    if any(digest(ROOT/f'pages/page{n}.dat')!=h for n,h in hashes.items()):raise ValueError('Source page changed during audit')
    # A verified no-op must preserve timing and the manifest hash embedded in downstream reports.
    if (pending or generation.get('completedPages') != sorted(completed) or generation.get('failures')
            or generation.get('pageHashes') != verified or generation.get('fontFileSha256') != font_hash):
        save()
    print(f'Finished: {len(completed)}/604 pages, {len(failures)} failures',flush=True)
    return bool(failures)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--executable',type=Path,default=Path('/private/tmp/medina-incomplete-corpus'))
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--page-timeout',type=int,default=180,help='Maximum seconds per page, including on a resumed audit')
    parser.add_argument('--templates',type=Path,help='Explicit frozen template file; leaves concurrently edited live templates alone')
    args=parser.parse_args();raise SystemExit(run(args.output,args.executable,args.jobs,args.templates,args.page_timeout))
