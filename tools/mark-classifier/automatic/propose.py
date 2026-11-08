"""Regenerate improved automatic teachers without changing validation proposals."""
from workbench_paths import NATIVE
from concurrent.futures import ProcessPoolExecutor
import hashlib
import json
from pathlib import Path
import sys
import os
import subprocess


def teacher_module():
    path=NATIVE/'tools'
    if str(path) not in sys.path:sys.path.insert(0,str(path))
    from vector_review import prepare_vector_review
    return prepare_vector_review


def _prepare(task):
    page,raw_root,proposal_root,bank,bank_sha,baselines=task
    teacher=teacher_module();doc=json.loads((Path(raw_root)/f'{page}.raw.json').read_text())
    if baselines['cacheSha256']!=doc['provenance']['cacheSha256']:raise ValueError('Baseline cache does not match the vector paths')
    for word in doc['words']:
        baseline=baselines['pdfBaselines'].get(str(word['line']))
        if baseline is not None and baseline>0:word['baseline']=baseline*doc['provenance']['pdfToRaster'][3]
    from vector_review.staggered_geometry import find_staggered_groups, apply_staggered_groups
    line_baselines={line:value*doc['provenance']['pdfToRaster'][3]
                    for line,value in baselines['pdfBaselines'].items() if value>0}
    geometric_groups=find_staggered_groups(doc['paths'],doc['raster']['width'],line_baselines)
    result=teacher.prepare(doc,bank,stroke_grouping=True)
    result=apply_staggered_groups(result,geometric_groups)
    result['provenance']['weakTemplateBankSha256']=bank_sha
    result['provenance']['transcriptSha256']=hashlib.sha256('\n'.join(w['text'] for w in doc['words']).encode()).hexdigest()
    result['provenance']['automaticTeacherVersion']='detached-mark-grouping-v10'
    file=Path(proposal_root)/f'{page}.json';temp=file.with_suffix('.tmp');temp.write_text(json.dumps(result,ensure_ascii=False,separators=(',',':')));temp.replace(file)
    return {'pageId':page,**result['summary']}


def generate(raw_root,proposal_root,pages,jobs,progress):
    raw_root,proposal_root=Path(raw_root),Path(proposal_root);proposal_root.mkdir(parents=True,exist_ok=True)
    baseline_file=proposal_root.parent/'baselines.json'
    if not baseline_file.exists():
        root=NATIVE
        subprocess.run([str(root/'build/ninja-nmc/Debug/medinamushafanalyzer'),'--export-baselines',str(baseline_file.resolve()),'--pages','all'],cwd=root,
            env={**os.environ,'QT_QPA_PLATFORM':'offscreen'},check=True)
    baselines=json.loads(baseline_file.read_text())['pages']
    sources=[];teacher=teacher_module()
    bank=teacher.seed_bank((json.loads((raw_root/f'{page}.raw.json').read_text()) for page in pages),sources)
    record={'policy':'Weak train-page exemplars; no evaluation/test fitting','prototypes':dict(bank),'sources':sources}
    raw=json.dumps(record,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode();sha=hashlib.sha256(raw).hexdigest()
    (proposal_root/'teacher-bank.json').write_bytes(raw)
    summary=[]
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        tasks=((p,str(raw_root),str(proposal_root),dict(bank),sha,baselines[p]) for p in pages)
        for n,item in enumerate(pool.map(_prepare,tasks),1):summary.append(item);progress('propose',n,len(pages),item)
    return {'pages':len(summary),'teacherVersion':'detached-mark-grouping-v10','bankSha256':sha,'weakPrototypes':len(sources),'manualProposalsModified':False}
