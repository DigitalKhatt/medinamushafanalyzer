#!/usr/bin/env python3
"""Audit each vector word/subword against transcript dots and recognized marks."""
from workbench_paths import NATIVE
import argparse
import json
from pathlib import Path
from automatic.word_audit import audit

if __name__ == '__main__':
    project = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=project/'data/automatic')
    parser.add_argument('--native', type=Path, default=NATIVE)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--pages', help='Comma-separated page numbers; default all prepared pages')
    args = parser.parse_args()
    pages = {f'page{int(p):03}' for p in args.pages.split(',')} if args.pages else None
    result = audit(args.root, args.native, args.output, pages, progress=lambda s: print(s, flush=True))
    print(json.dumps({k:v for k,v in result.items() if k not in ('pageSummary','inference')}, ensure_ascii=False, indent=2))
