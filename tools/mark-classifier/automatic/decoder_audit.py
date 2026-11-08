"""Compare decoding rules on held-out pages with fixed weights and labels.

Default evaluation uses validation pages only. Test evaluation is an explicit,
subsequent regression check after the decoder has been fixed, never a tuning set.
"""
from datetime import datetime,timezone
import json
from pathlib import Path

from .common import FEATURE_DIM
from .labels import load,digest_file
from .learn import Network,ROLES,decoder_policy,feature_batches,f1_score,predictions


def legacy_predictions(rp,tp,type_roles):
    """Frozen historical decoder for comparison only; never used by inference."""
    import numpy as np
    r=rp.argmax(1);conditional=tp.copy()
    for k,role in enumerate(type_roles):conditional[:,k]*=(r==role)
    sums=conditional.sum(1,keepdims=True);conditional/=np.maximum(1e-12,sums)
    t=conditional.argmax(1);has_type=np.isin(r,[1,2,3])&(sums[:,0]>0);t[~has_type]=-1
    score=rp.max(1);score[has_type]*=conditional[np.flatnonzero(has_type),t[has_type]]
    ordered=np.sort(rp,axis=1)
    return r,t,score,ordered[:,-1]-ordered[:,-2]


def audit(root,include_test=False):
    import numpy as np
    root=Path(root);model=json.loads((root/'models/model.json').read_text())
    manifest,targets=load(root)
    if manifest['labelsSha256']!=model['trainingLabelsSha256']:raise ValueError('Audit requires the labels used to fit the saved model')
    weights=root/'models/selected.npz'
    if digest_file(weights)!=model['modelSha256']:raise ValueError('Saved model weights changed')
    with np.load(weights) as w:
        net=Network(FEATURE_DIM,w['w1'].shape[1],len(model['types']));net.p={k:w[k] for k in net.p};mean,std=w['mean'],w['std']
    features=np.memmap(root/'features.u8',mode='r',dtype='u1',shape=(len(targets),FEATURE_DIM))
    type_index={name:i for i,name in enumerate(model['types'])}
    class_names=[name for i,name in enumerate(ROLES) if i not in (1,2,3)]+[
        ROLES[role]+':'+name for role,name in zip(model['typeRoles'],model['types'])]
    class_index={name:i for i,name in enumerate(class_names)}
    def classes(r,t):
        return np.array([class_index.get(ROLES[a]+':'+model['types'][b],-1) if a in (1,2,3) and b>=0 else
            class_index.get(ROLES[a],-1) for a,b in zip(r,t)],dtype='i4')
    output={'createdAt':datetime.now(timezone.utc).isoformat(),'modelSha256':model['modelSha256'],
        'trainingLabelsSha256':manifest['labelsSha256'],'decoder':decoder_policy(),'baseline':'role-first-filter-renormalize',
        'policy':'Fixed weights, features and page splits. Validation checked before deployment; test pages used only after the decoder was fixed. No labels or approvals generated.',
        'referenceWarning':'Automatic references measure teacher agreement. Human-reviewed results are reported separately. Existing held-out pages are reused for regression checks.',
        'splits':{}}
    for split in ['validation']+(['test'] if include_test else []):
        indices=np.array([i for i,t in enumerate(targets) if t['split']==split and t['role'] in ROLES and t['quality']>=.2],dtype='i8')
        rtrue=np.array([ROLES.index(targets[i]['role']) for i in indices],dtype='i4')
        ttrue=np.array([type_index.get(targets[i]['type'],-1) if targets[i]['role'] in ('dot','mark','symbol') else -1 for i in indices],dtype='i4')
        truth=classes(rtrue,ttrue);before=[];after=[];scores=[]
        for ix,x in feature_batches(features,indices,mean,std):
            rp,tp,_=net.forward(x)
            old=legacy_predictions(rp,tp,model['typeRoles']);new=predictions(rp,tp,model['typeRoles'])
            before.append(np.stack(old[:2],axis=1));after.append(np.stack(new[:2],axis=1));scores.extend(new[2].tolist())
        old=np.concatenate(before) if before else np.empty((0,2),dtype='i4')
        new=np.concatenate(after) if after else np.empty((0,2),dtype='i4')
        old_class=classes(old[:,0],old[:,1]);new_class=classes(new[:,0],new[:,1]);known=truth>=0
        def metrics(mask):
            return {name:{'examples':int(mask.sum()),'role':f1_score(rtrue[mask],pred[mask,0],len(ROLES)),
                'type':f1_score(ttrue[mask],pred[mask,1],len(model['types'])),
                'jointLabel':f1_score(truth[mask],cls[mask],len(class_names))}
                for name,pred,cls in [('before',old,old_class),('after',new,new_class)]}
        def example(j):
            i=indices[j];old_r,old_t=map(int,old[j]);new_r,new_t=map(int,new[j])
            return {'id':targets[i]['item_id'],'targetRole':targets[i]['role'],'targetType':targets[i]['type'],
                'beforeRole':ROLES[old_r],'beforeType':model['types'][old_t] if old_t>=0 else 'unknown',
                'role':ROLES[new_r],'type':model['types'][new_t] if new_t>=0 else 'unknown','score':scores[j],
                'correctBefore':bool(old_class[j]==truth[j]),'correctAfter':bool(new_class[j]==truth[j])}
        human=np.array([bool(targets[i]['human_reviewed']) for i in indices]);changed=np.any(old!=new,axis=1)
        output['splits'][split]={'eligibleQualityAtLeast02':metrics(np.ones(len(indices),dtype=bool)),
            'higherEvidenceQualityAtLeast06':metrics(np.array([targets[i]['quality']>=.6 for i in indices])),
            'human':{'examples':int(human.sum()),'beforeCorrect':int(np.sum(human&known&(old_class==truth))),
                'afterCorrect':int(np.sum(human&known&(new_class==truth))),'predictions':[example(j) for j in np.flatnonzero(human)]},
            'changed':int(changed.sum()),'corrected':int(np.sum(known&(old_class!=truth)&(new_class==truth))),
            'regressed':int(np.sum(known&(old_class==truth)&(new_class!=truth))),
            'changedExamples':[example(j) for j in np.flatnonzero(changed)[:50]]}
    return output
