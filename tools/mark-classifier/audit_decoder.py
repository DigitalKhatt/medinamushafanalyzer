#!/usr/bin/env python3
"""Evaluate saved-model decoding against frozen held-out labels without fitting."""
import argparse
import json
from pathlib import Path
from automatic.decoder_audit import audit
from segment import atomic_json


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parent/'data/automatic')
    parser.add_argument('--include-test',action='store_true',help='Only after fixing the decoder from validation results; never for tuning.')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();report=audit(args.root,args.include_test);atomic_json(args.output,report)
    for split,info in report['splits'].items():
        metric=info['eligibleQualityAtLeast02']
        print(json.dumps({'split':split,'examples':metric['after']['examples'],'corrected':info['corrected'],
            'regressed':info['regressed'],'beforeJointAccuracy':metric['before']['jointLabel']['accuracy'],
            'afterJointAccuracy':metric['after']['jointLabel']['accuracy'],'human':{k:v for k,v in info['human'].items() if k!='predictions'}}))
