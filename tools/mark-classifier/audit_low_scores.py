#!/usr/bin/env python3
"""Export every low-score corpus region with evidence and review hints."""
import argparse
import json
from pathlib import Path
from automatic.low_scores import audit

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parent/'data/automatic')
    parser.add_argument('--baseline',type=Path)
    args=parser.parse_args();print(json.dumps(audit(args.root,args.baseline),ensure_ascii=False))
