#!/usr/bin/env python3
"""Record a human's explicit review manifest; never infer approvals from scores."""
import argparse
import json
import os
from pathlib import Path
import sqlite3

import segment
from automatic.reviews import preflight,record


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',required=True,type=Path)
    parser.add_argument('--data-dir',default=Path(__file__).resolve().parent/'data',type=Path)
    parser.add_argument('--check-only',action='store_true')
    args=parser.parse_args();root=args.data_dir/'automatic'
    pipeline_lock=(root/'.pipeline.lock').open('a+b')
    if os.name=='posix':
        import fcntl
        try:fcntl.flock(pipeline_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:parser.error('An automatic pipeline run is using this corpus. Finish that run before synchronizing reviews.')
    manifest=json.loads(args.manifest.read_text())
    corpus=json.loads((args.data_dir/'corpus.json').read_text())
    store=segment.Store({'source_dir':corpus['sourceDir']},args.data_dir)
    if args.check_only:
        db=sqlite3.connect(root/'corpus.sqlite');db.row_factory=sqlite3.Row
        try:plans=preflight(store,root,manifest,db)
        finally:db.close()
        print(json.dumps({'pages':len(plans),'items':len(manifest['items']),'checked':True}))
    else:
        result=record(store,root,manifest,lambda page,n,total:print(f'{n}/{total}: {page}',flush=True))
        print(json.dumps(result))


if __name__=='__main__':main()
