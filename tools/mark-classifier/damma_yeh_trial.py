#!/usr/bin/env python3
"""Fit a mask-only damma/yeh expert against a frozen, source-cleaned dataset."""
import argparse
from collections import Counter
import json
from pathlib import Path
import sqlite3

import numpy as np
from scipy.optimize import minimize
from automatic.common import FEATURE_DIM
from automatic.damma_yeh import COLUMNS,TYPES,VERSION,probability
from automatic.labels import load,digest_file


def run(root):
    manifest,rows=load(root)
    features=np.memmap(root/'features.u8',mode='r',dtype='u1',shape=(len(rows),FEATURE_DIM))
    eligible=np.asarray([r['role']=='mark' and r['type'] in TYPES and r['quality']>=.2 for r in rows])
    train=np.flatnonzero(eligible&np.asarray([r['split']=='train' for r in rows]))
    y=np.asarray([TYPES.index(rows[i]['type']) for i in train],dtype='f8')
    quality=np.asarray([rows[i]['quality'] for i in train],dtype='f8')
    raw=np.asarray(features[train][:,COLUMNS],dtype='f8')/255
    mean=raw.mean(0);std=np.maximum(.12,raw.std(0));x=(raw-mean)/std
    weight=np.zeros(len(y))
    for kind in range(2):
        mask=y==kind
        if not mask.any():raise ValueError('Both classes need train-page examples')
        weight[mask]=.5*quality[mask]/quality[mask].sum()
    regularization=.0001
    def objective(theta):
        z=x@theta[:-1]+theta[-1];p=1/(1+np.exp(-np.clip(z,-40,40)))
        loss=float(weight@(np.logaddexp(0,z)-y*z)+regularization*.5*(theta[:-1]@theta[:-1]))
        delta=weight*(p-y)
        return loss,np.r_[x.T@delta+regularization*theta[:-1],delta.sum()]
    fit=minimize(objective,np.zeros(len(COLUMNS)+1),jac=True,method='L-BFGS-B',options={'maxiter':200,'gtol':1e-7,'ftol':1e-12})
    if not fit.success:raise ValueError(f'Shape training did not converge: {fit.message}')
    path=root/'models/damma-yeh.npz';np.savez_compressed(path,w=fit.x[:-1],b=fit.x[-1],mean=mean,std=std)
    config={'version':VERSION,'weights':'models/damma-yeh.npz','weightsSha256':digest_file(path),
        'baselineSha256':digest_file(root/'models/selected.npz'),'trainingLabelsSha256':manifest['labelsSha256'],
        'threshold':.99,'classes':list(TYPES),'input':'Region mask and intrinsic geometry; no neighboring ink, baseline placement, transcript or owner.',
        'policy':'Only confident pair disagreements are reconsidered; conserve total pair probability, all other type scores and role scores.'}
    weights=dict(w=fit.x[:-1],b=fit.x[-1],mean=mean,std=std)
    with sqlite3.connect(root/'corpus.sqlite') as db:
        baseline={i:(k,t) for i,k,t in db.execute('select id,raw_prediction_kind,raw_prediction_type from items')}
    def measure(split):
        ids=np.flatnonzero(eligible&np.asarray([r['split']==split for r in rows]));p=probability(np.asarray(features[ids]),weights)
        before=after=regressed=corrected=0;changes=[]
        for i,vote in zip(ids,p):
            r=rows[i];old=baseline[r['item_id']];new=old
            winner=int(vote>=.5);certainty=max(vote,1-vote)
            if old[0]=='mark' and old[1] in TYPES and certainty>=config['threshold']:new=('mark',TYPES[winner])
            a=old==('mark',r['type']);b=new==('mark',r['type']);before+=a;after+=b;regressed+=a and not b;corrected+=b and not a
            if old!=new:changes.append(dict(id=r['item_id'],reference=r['type'],before=old[1],after=new[1],shapeProbability=float(certainty)))
        return dict(examples=len(ids),counts=dict(Counter(rows[i]['type'] for i in ids)),beforeCorrect=int(before),afterCorrect=int(after),regressed=int(regressed),corrected=int(corrected),changes=changes)
    report={'config':config,'training':measure('train'),'trainingPages':len({rows[i]['page_id'] for i in train}),
        'trainingIds':[rows[i]['item_id'] for i in train],'optimizer':dict(iterations=fit.nit,loss=float(fit.fun)),
        'referenceWarning':'Source-cleaned automatic targets plus explicit existing reviews; teacher agreement is not measured human accuracy.',
        'validation':measure('validation')}
    # Freeze the entire fitting procedure and threshold before touching test.
    if report['validation']['regressed']:raise ValueError('Held-out validation labels regressed; do not activate')
    (root/'shape-validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
    report['testAfterSelection']=measure('test')
    (root/'shape-trial.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
    if report['testAfterSelection']['regressed']:raise ValueError('Frozen candidate regressed on test; retain experiment without activation')
    (root/'models/damma-yeh.json').write_text(json.dumps(config,indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='trainingIds'},ensure_ascii=False,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--root',type=Path,required=True)
    run(parser.parse_args().root.resolve())
