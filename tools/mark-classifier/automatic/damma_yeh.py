"""A learned shape-only distinction between damma and small high yeh.

Only confident disagreements inside this existing pair are reconsidered. The
pair's total probability is conserved; other types and role votes are fixed.
No text, ownership, teacher labels or human flags are read at inference.
"""
import json
from pathlib import Path

VERSION='mask-only-damma-high-yeh-v1'
TYPES=('small-high-yeh','damma')
# Region mask and intrinsic geometry. Omit the context image and all baseline
# placement features, which can encode another letter's neighboring marks.
COLUMNS=tuple(range(256))+(512,513,514,515,516,518,519,520,524,525,526,527)


def probability(features,weights):
    import numpy as np
    x=(np.asarray(features[:,COLUMNS],dtype='f4')/255-weights['mean'])/weights['std']
    logits=np.clip(x@weights['w']+weights['b'],-30,30)
    return 1/(1+np.exp(-logits))


def refine(rp,tp,roles,labels,features,metadata,weights,excluded=(),threshold=.99):
    import numpy as np
    from .learn import ROLES,predictions
    pair=[metadata['types'].index(t) for t in TYPES]
    ids=np.flatnonzero((roles==ROLES.index('mark'))&np.isin(labels,pair))
    if excluded:ids=ids[~np.isin(ids,list(excluded))]
    evidence={}
    for start in range(0,len(ids),2048):
        block=ids[start:start+2048];damma=probability(np.asarray(features[block]),weights)
        for i,p in zip(block,damma):
            votes=np.asarray([1-p,p],dtype='f4');winner=int(votes.argmax())
            if votes[winner]<threshold or labels[i]==pair[winner]:continue
            before=tp[i,pair].copy();mass=float(before.sum())
            if mass<=0:continue
            tp[i,pair]=votes*mass
            proposed_role,proposed_type,_,_=predictions(rp[i:i+1],tp[i:i+1],metadata['typeRoles'])
            if proposed_role[0]!=ROLES.index('mark') or proposed_type[0]!=pair[winner]:
                tp[i,pair]=before
                continue
            evidence[int(i)]={'version':VERSION,'scores':dict(zip(TYPES,map(float,votes))),
                'beforePairScores':dict(zip(TYPES,map(float,before))),
                'preservedPairProbability':mass,'policy':'Shape-only learned pair probabilities; role and other type probabilities unchanged.'}
    if evidence:
        selected=np.asarray(list(evidence));r,t,s,m=predictions(rp[selected],tp[selected],metadata['typeRoles'])
        roles[selected]=r;labels[selected]=t
        return evidence,selected,s,m,len(ids)
    return evidence,np.asarray([],dtype='i8'),np.asarray([]),np.asarray([]),len(ids)


def apply(root,metadata,features,rp,tp,roles,labels,scores,margins,excluded=()):
    import numpy as np
    from .labels import digest_file
    root=Path(root);activation=root/'models/damma-yeh.json'
    if not activation.exists():return {},None
    config=json.loads(activation.read_text());path=root/config['weights']
    if config['baselineSha256']!=metadata['modelSha256'] or digest_file(path)!=config['weightsSha256']:
        raise ValueError('Damma/yeh shape classifier does not match its checkpoints')
    with np.load(path) as weights:
        evidence,ids,s,m,eligible=refine(rp,tp,roles,labels,features,metadata,weights,excluded,config['threshold'])
    scores[ids]=s;margins[ids]=m
    return evidence,{**config,'eligible':eligible,'changed':len(evidence),'version':VERSION}
