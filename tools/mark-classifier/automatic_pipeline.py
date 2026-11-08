#!/usr/bin/env python3
"""Separate offline label preparation, model training and corpus inference."""
from __future__ import annotations
from workbench_paths import NATIVE
import argparse
from datetime import datetime,timezone
import json
import os
import shutil
from pathlib import Path
import time

ROOT=Path(__file__).resolve().parent

def atomic(path,value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True);temp=path.with_suffix('.tmp')
    temp.write_text(json.dumps(value,ensure_ascii=False,indent=2,allow_nan=False));temp.replace(path)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=['run','build','prepare-labels','freeze-labels','train','infer','resolve','scope','check'])
    parser.add_argument('--data-dir',type=Path,default=ROOT/'data')
    parser.add_argument('--output',type=Path)
    parser.add_argument('--pdf-root',type=Path,default=NATIVE/'input/1441-AI-hafs')
    parser.add_argument('--raw-root',type=Path,default=NATIVE/'output/vector-review')
    parser.add_argument('--proposal-root',type=Path,help='Reuse a specified automatic proposal snapshot instead of generating a new one.')
    parser.add_argument('--labels',type=Path,help='Independent frozen training-label dataset (default: OUTPUT/training-labels).')
    parser.add_argument('--structural-filter',action='store_true',help='Withhold structurally suspicious model predictions; default is diagnostic flags only.')
    parser.add_argument('--pages',default='all')
    parser.add_argument('--rebuild-pages',help='Regenerate only these comma-separated page numbers; reuse all other source-mask page caches.')
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--rounds',type=int,default=3)
    parser.add_argument('--epochs',type=int,default=18)
    parser.add_argument('--hard-example-model',type=Path,help='Frozen checkpoint folder used to select difficult train-page examples; labels never come from its predictions.')
    parser.add_argument('--allow-weak-labels',action='store_true',help='Explicitly train on machine-labelled examples without claiming human validation.')
    args=parser.parse_args()
    if args.command in ('run','train','resolve') and not args.allow_weak_labels:parser.error('Automatic supervision requires --allow-weak-labels; human exports remain separate.')
    if not 1<=args.jobs<=12 or not 1<=args.rounds<=6 or not 1<=args.epochs<=100:parser.error('Invalid jobs/rounds/epochs')
    output=(args.output or args.data_dir/'automatic').resolve();output.mkdir(parents=True,exist_ok=True)
    pipeline_lock=(output/'.pipeline.lock').open('a+b')  # Held until this run exits.
    if os.name=='posix':
        import fcntl
        try:fcntl.flock(pipeline_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:parser.error('Another automatic run is already using this output directory.')
    if args.command=='run' and (output/'report.json').exists():
        previous=json.loads((output/'report.json').read_text())
        tag=previous.get('startedAt','previous').replace(':','-').replace('+','_')
        archive=output/'experiments'/tag;archive.mkdir(parents=True,exist_ok=True)
        for name in ['report.json','models/model.json','models/selected.npz']:
            source=output/name
            if source.exists():shutil.copyfile(source,archive/Path(name).name)
    available=sorted(p.stem for p in (args.data_dir/'proposals').glob('page*.json'))
    if args.pages=='all':pages=available
    else:
        selected=set()
        for piece in args.pages.split(','):
            ends=piece.split('-');a=int(ends[0]);b=int(ends[-1]);selected.update(f'page{n:03d}' for n in range(a,b+1))
        pages=[p for p in available if p in selected]
        if set(pages)!=selected:parser.error('Some requested pages have no vector proposals')
    if not pages and args.command in ('run','build','prepare-labels'):parser.error('No proposal pages')
    state={'schemaVersion':1,'startedAt':datetime.now(timezone.utc).isoformat(),'status':'running',
        'stage':'starting','weakSupervision':True,'humanLabelsAreSeparate':True,'args':{k:str(v) if isinstance(v,Path) else v for k,v in vars(args).items()}}
    if args.command=='train':
        for prior_path in (output/'status.json',output/'report.json'):
            if prior_path.exists():
                prior=json.loads(prior_path.read_text())
                if prior.get('proposals'):
                    state['proposals']=prior['proposals'];break
    start=time.monotonic()
    def progress(stage,current,total,details):
        state.update(stage=stage,current=current,total=total,details=details,elapsedSeconds=round(time.monotonic()-start,1))
        atomic(output/'status.json',state);print(json.dumps({'stage':stage,'current':current,'total':total,**details},ensure_ascii=False),flush=True)
    try:
        if args.command in ('resolve','scope','check','infer','freeze-labels'):
            import sqlite3
            with sqlite3.connect(output/'corpus.sqlite') as db:
                for key in ('build','learning','scope','resolution','inference'):
                    row=db.execute('SELECT value FROM meta WHERE key=?',(key,)).fetchone()
                    if row:state[key]=json.loads(row[0])
            if (output/'report.json').exists():state['proposals']=json.loads((output/'report.json').read_text()).get('proposals')
        if args.command in ('run','build','prepare-labels'):
            if not args.proposal_root:
                from automatic.propose import generate
                args.proposal_root=output/'proposals'
                state['proposals']=generate(args.raw_root.resolve(),args.proposal_root,pages,args.jobs,progress)
            from automatic.build import build
            rebuild_pages={f'page{int(p):03d}' for p in args.rebuild_pages.split(',')} if args.rebuild_pages else None
            state['build']=build(args.data_dir.resolve(),output,args.pdf_root.resolve(),pages,args.jobs,progress,args.proposal_root,rebuild_pages)
        if args.command in ('run','build','prepare-labels','scope'):
            from automatic.scope import apply_scopes
            state['scope']=apply_scopes(args.data_dir.resolve(),output,args.jobs,progress)
        if args.command in ('run','prepare-labels','freeze-labels'):
            from automatic.labels import prepare
            state['labels']=prepare(output,args.labels)
            progress('labels-prepared',state['labels']['items'],state['labels']['items'],{'labelsSha256':state['labels']['labelsSha256']})
        if args.command in ('run','train'):
            if 'build' not in state:
                import sqlite3
                with sqlite3.connect(output/'corpus.sqlite') as db:
                    row=db.execute("SELECT value FROM meta WHERE key='build'").fetchone()
                    if row:state['build']=json.loads(row[0])
            from automatic.learn import train_loop
            state['learning']=train_loop(output,args.rounds,args.epochs,progress,args.labels,args.hard_example_model)
        if args.command in ('run','infer'):
            from automatic.inference import infer_corpus
            state['inference']=infer_corpus(output,progress,args.structural_filter)
            state.pop('resolution',None)
        if args.command=='resolve':
            from automatic.resolve import resolve_corpus
            state['resolution']=resolve_corpus(output,args.jobs,progress)
            # This explicitly requested historical heuristic pass no longer has
            # the model-plus-human-only final-label contract.
            import sqlite3
            with sqlite3.connect(output/'corpus.sqlite') as db:db.execute("DELETE FROM meta WHERE key='inference'")
            state.pop('inference',None)
        if args.command=='check':
            from automatic.structure import recheck
            state['learning']=recheck(output,progress)
        state.update(status='complete',stage='complete',elapsedSeconds=round(time.monotonic()-start,1),finishedAt=datetime.now(timezone.utc).isoformat())
        atomic(output/'status.json',state);atomic(output/'report.json',state)
        print(f'Corpus and report: {output}',flush=True)
    except BaseException as error:
        state.update(status='interrupted' if isinstance(error,KeyboardInterrupt) else 'failed',error=str(error),elapsedSeconds=round(time.monotonic()-start,1))
        atomic(output/'status.json',state)
        raise

if __name__=='__main__':main()
