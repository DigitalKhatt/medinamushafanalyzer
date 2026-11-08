"""CPU neural classifier, page-held-out evaluation, and constrained refinement.

The frozen teacher supplies weak labels; its agreement is a proxy, not ground
truth accuracy. Test pages are evaluated only after all round selection ends.
"""
from __future__ import annotations
from collections import Counter
import copy
import hashlib
import json
import math
from pathlib import Path
import sqlite3
import time

from .common import FEATURE_DIM
ROLES=['subword-ink','dot','mark','symbol','other','body','kashida']
DECODER_VERSION='joint-role-type-product-v1'


def decoder_policy():
    return {'version':DECODER_VERSION,
        'rule':'Score each learned type together with its compatible role: role score × type score. Roles without learned types use the role score alone. Choose the largest score across all candidates.',
        'score':'Unnormalized agreement score, not calibrated accuracy. No rescaling within a prematurely selected role.',
        'margin':'Winning candidate score minus the next candidate score.'}


def softmax(x):
    import numpy as np
    e=np.exp(x-x.max(axis=1,keepdims=True));return e/np.maximum(e.sum(axis=1,keepdims=True),1e-12)


def f1_score(truth,prediction,size):
    import numpy as np
    # An absent type (-1) is a miss, never Python's last-column index.
    matrix=np.zeros((size,size+1),dtype='i8');valid=truth>=0
    predicted=np.where((prediction>=0)&(prediction<size),prediction,size)
    np.add.at(matrix,(truth[valid],predicted[valid]),1)
    support=matrix.sum(1);tp=np.diag(matrix);precision=tp/np.maximum(1,matrix[:,:size].sum(0));recall=tp/np.maximum(1,support)
    f1=2*precision*recall/np.maximum(1e-12,precision+recall)
    return {'accuracy':float(tp.sum()/max(1,support.sum())),
        'macroF1':float(f1[support>0].mean()) if np.any(support) else 0.,
        'support':support.tolist(),'precision':precision.tolist(),'recall':recall.tolist(),'f1':f1.tolist(),
        'confusion':matrix.tolist(),'missingPredictionColumn':size}


class Network:
    def __init__(self,input_dim,hidden,type_count,seed=42):
        import numpy as np
        self.rng=np.random.default_rng(seed)
        self.p={}
        for name,a,b in [('w1',input_dim,hidden),('w2',hidden,64),('wr',64,len(ROLES)),('wt',64,type_count)]:
            self.p[name]=(self.rng.normal(0,math.sqrt(2/a),(a,b))).astype('f4');self.p['b'+name[1:]]=np.zeros(b,dtype='f4')
        self.m={k:np.zeros_like(v) for k,v in self.p.items()};self.v={k:np.zeros_like(v) for k,v in self.p.items()};self.step=0
    def forward(self,x):
        import numpy as np
        p=self.p;a=np.maximum(0,x@p['w1']+p['b1']);b=np.maximum(0,a@p['w2']+p['b2'])
        return softmax(b@p['wr']+p['br']),softmax(b@p['wt']+p['bt']),(x,a,b)
    def batch(self,x,role,type_target,weights,lr):
        import numpy as np
        rp,tp,(x,a,b)=self.forward(x);n=len(x);ix=np.arange(n)
        dr=rp.copy();dr[ix,role]-=1;dr*=weights[:,None]/max(1,weights.sum())
        dt=tp.copy();known=type_target>=0;dt[~known]=0;dt[ix[known],type_target[known]]-=1
        dt*=weights[:,None]/max(1,weights[known].sum())
        grad={};grad['wr']=b.T@dr;grad['br']=dr.sum(0);grad['wt']=b.T@dt;grad['bt']=dt.sum(0)
        db=(dr@self.p['wr'].T+dt@self.p['wt'].T)*(b>0)
        grad['w2']=a.T@db;grad['b2']=db.sum(0)
        da=(db@self.p['w2'].T)*(a>0);grad['w1']=x.T@da;grad['b1']=da.sum(0)
        self.step+=1
        for key,g in grad.items():
            g=np.clip(g,-5,5)
            if key.startswith('w'):g+=.0002*self.p[key]
            self.m[key]=.9*self.m[key]+.1*g;self.v[key]=.999*self.v[key]+.001*g*g
            self.p[key]-=lr*(self.m[key]/(1-.9**self.step))/(np.sqrt(self.v[key]/(1-.999**self.step))+1e-8)
        loss=-np.log(np.maximum(1e-9,rp[ix,role]))
        loss[known]-=np.log(np.maximum(1e-9,tp[ix[known],type_target[known]]))
        return float(np.average(loss,weights=weights))


def feature_batches(features,indices,mean,std,batch=1024):
    import numpy as np
    for start in range(0,len(indices),batch):
        ix=indices[start:start+batch];x=(np.asarray(features[ix],dtype='f4')/255-mean)/std
        yield ix,x


def evaluate(net,features,indices,roles,types,mean,std,type_names,type_roles):
    import numpy as np
    rpred=[];tpred=[];loss=0.;known_count=0
    for ix,x in feature_batches(features,indices,mean,std):
        rp,tp,_=net.forward(x);r,t,_,_=predictions(rp,tp,type_roles)
        rpred.extend(r);tpred.extend(t);known=types[ix]>=0
        loss+=float(-np.log(np.maximum(1e-9,rp[np.arange(len(ix)),roles[ix]])).sum())
        if np.any(known):loss+=float(-np.log(np.maximum(1e-9,tp[np.flatnonzero(known),types[ix][known]])).sum());known_count+=int(known.sum())
    rm=f1_score(roles[indices],np.asarray(rpred),len(ROLES));tm=f1_score(types[indices],np.asarray(tpred),len(type_names))
    return {'reference':'Held-out frozen labels: mostly automatic proposals plus any explicit human reviews.',
        'decoder':DECODER_VERSION,'examples':len(indices),
        'loss':loss/max(1,len(indices)+known_count),'role':rm,'type':tm,'score':.55*rm['macroF1']+.45*tm['macroF1']}


def sample_indices(indices,roles,types,quality,per_type,seed,min_quality=.25):
    import numpy as np
    rng=np.random.default_rng(seed);selected=[]
    strata={}
    for i in indices:
        if quality[i]>=min_quality:strata.setdefault((int(roles[i]),int(types[i])),[]).append(int(i))
    for key,values in sorted(strata.items()):
        values=np.asarray(values,dtype='i8');limit=per_type*2 if key[0]==0 else per_type
        if len(values)>limit:
            probabilities=np.maximum(.03,quality[values]);probabilities/=probabilities.sum()
            values=rng.choice(values,limit,replace=False,p=probabilities)
        selected.extend(values.tolist())
    rng.shuffle(selected);return np.asarray(selected,dtype='i8')


def type_vocabulary(rows):
    """A type needs usable training ink, not an arbitrary minimum frequency."""
    return sorted({row['teacher_type'] for row in rows if row['in_scope'] and row['split']=='train'
        and row['teacher_kind'] in ('dot','mark','symbol') and row['quality']>=.2
        and row['teacher_type'] not in (None,'unknown')})


def supplement_types(indices,pool,types,quality,per_type,seed):
    """Give weak-only types the same bounded sampling budget as other types.

    Some valid multipart signs have uniformly low proposal evidence. A fixed
    250-example fallback previously omitted most of their shape variation.
    """
    import numpy as np
    rng=np.random.default_rng(seed);added=[]
    selected=np.zeros(len(types),dtype=bool);selected[indices]=True
    for ti in sorted(set(types[pool])-{-1}):
        count=int(np.sum(types[indices]==ti))
        need=per_type-count
        if need<=0:continue
        options=pool[(types[pool]==ti)&(quality[pool]>=.2)&~selected[pool]]
        if len(options):added.extend(rng.choice(options,min(need,len(options)),replace=False))
    return np.concatenate((indices,np.asarray(added,dtype='i8')))


def training_draw_positions(types,target=128,max_repeats=8,seed=42):
    """Bounded balanced repetition of training examples; no synthetic evidence."""
    import numpy as np
    rng=np.random.default_rng(seed);positions=list(range(len(types)))
    for ti in sorted(set(types)-{-1}):
        available=np.flatnonzero(types==ti)
        count=min(target,len(available)*max_repeats)-len(available)
        if count>0:
            repeated=np.tile(rng.permutation(available),max_repeats-1)
            positions.extend(repeated[:count].tolist())
    return np.asarray(positions,dtype='i8')


def reviewed_metrics(net,features,indices,rows,mean,std,type_names,type_roles):
    """Exact role AND type recognition, before any human corpus override."""
    examples=[];by_type={}
    for ix,x in feature_batches(features,indices,mean,std):
        rp,tp,_=net.forward(x);r,t,score,_=predictions(rp,tp,type_roles)
        for j,i in enumerate(ix):
            row=rows[i];name=type_names[t[j]] if t[j]>=0 else 'unknown'
            correct=ROLES[r[j]]==row['teacher_kind'] and name==row['teacher_type']
            examples.append({'id':row['id'],'target':row['teacher_type'],'prediction':name,
                'predictedRole':ROLES[r[j]],'score':float(score[j]),'correct':bool(correct)})
            entry=by_type.setdefault(row['teacher_type'],{'examples':0,'correct':0})
            entry['examples']+=1;entry['correct']+=int(correct)
    return {'reference':'Human-reviewed held-out labels; raw model output without reviewed overrides.',
        'examples':len(examples),'correct':sum(e['correct'] for e in examples),'byType':by_type,'predictions':examples,
        'limitation':'A small positive-only review set measures recognition of these examples, not corpus-wide precision.'}


def body_hard_negatives(rows,seed,limit=1000,split='train'):
    """Sample compact letter bodies from frozen train teachers, never own guesses.
    The heh-shaped stem of isolated ta marbuta is also a body, with dots separate.
    """
    import numpy as np
    families={};rng=np.random.default_rng(seed)
    for i,row in enumerate(rows):
        if row['split']!=split or not row.get('in_scope',1) or row['teacher_kind']!='subword-ink' or row['quality']<.65:continue
        base=row.get('bases','')
        family=('small-kaf-body' if row['teacher_type']=='small-kaf-body' else
            'isolated-heh-body' if base in ('ه','ة') else
            'single-letter-body:'+base if len(base)==1 and base in 'ابتثجحخدذرزسشصضطظعغفقكلمنويىء' else None)
        if family:families.setdefault(family,[]).append(i)
    selected=[];counts={}
    for family,indices in sorted(families.items()):
        cap=min(limit,300) if family.startswith('single-letter-body:') else limit
        sample=rng.choice(indices,min(cap,len(indices)),replace=False);selected.extend(map(int,sample));counts[family]=len(sample)
    return np.asarray(selected,dtype='i8'),counts


def hard_example_indices(rows,scores,agreements,limit=200):
    """Select difficult frozen training targets, never held-out predictions."""
    import numpy as np
    groups={}
    for i,row in enumerate(rows):
        if row['split']!='train' or not row.get('in_scope',1) or row['quality']<.2 or not np.isfinite(scores[i]) or scores[i]>=.97:continue
        if not agreements[i]:continue
        if row['teacher_kind']=='unknown':continue
        groups.setdefault((row['teacher_kind'],row['teacher_type']),[]).append(i)
    chosen=[]
    for key,ids in sorted(groups.items()):
        cap=700 if key[0]=='subword-ink' else limit
        chosen.extend(sorted(ids,key=lambda i:(scores[i],rows[i]['id']))[:cap])
    return np.asarray(chosen,dtype='i8')


def mine_hard_examples(folder,features,rows):
    import numpy as np
    folder=Path(folder);meta=json.loads((folder/'model.json').read_text())
    if hashlib.sha256((folder/'selected.npz').read_bytes()).hexdigest()!=meta['modelSha256']:raise ValueError('Hard-example checkpoint hash mismatch')
    weights=np.load(folder/'selected.npz');net=Network(FEATURE_DIM,weights['w1'].shape[1],len(meta['types']))
    net.p={k:weights[k] for k in net.p};mapping={name:i for i,name in enumerate(meta['types'])}
    pool=np.asarray([i for i,row in enumerate(rows) if row['split']=='train' and row['quality']>=.2
        and row['teacher_kind'] in ROLES and (row['teacher_kind'] not in ('dot','mark','symbol') or row['teacher_type'] in mapping)],dtype='i8')
    scores=np.full(len(rows),np.nan);agreements=np.zeros(len(rows),dtype=bool)
    for ix,x in feature_batches(features,pool,weights['mean'],weights['std']):
        rp,tp,_=net.forward(x);pr,pt,_,_=predictions(rp,tp,meta['typeRoles'])
        for j,i in enumerate(ix):
            row=rows[i];role=ROLES.index(row['teacher_kind']);ti=mapping.get(row['teacher_type'],-1)
            typed=row['teacher_kind'] in ('dot','mark','symbol')
            scores[i]=rp[j,role]*(tp[j,ti] if typed else 1.)
            agreements[i]=pr[j]==role and (not typed or pt[j]==ti)
    selected=hard_example_indices(rows,scores,agreements)
    report={'checkpointSha256':meta['modelSha256'],'examples':len(selected),
        'policy':'Train pages only; frozen targets and weights retained. Existing model selects difficult examples, never supplies replacement labels.',
        'byType':dict(Counter(rows[i]['teacher_type'] if rows[i]['teacher_type']!='unknown' else rows[i]['teacher_kind'] for i in selected)),
        'itemIds':[rows[i]['id'] for i in selected]}
    return selected,report


def infer(net,features,mean,std,type_roles,progress,round_index,output):
    import numpy as np
    n=len(features);rp_all=np.empty((n,len(ROLES)),dtype='f4');tp_all=np.empty((n,len(type_roles)),dtype='f4')
    embedding_temp=Path(output)/'embeddings.building.f16'
    embeddings=np.memmap(embedding_temp,mode='w+',dtype='f2',shape=(n,64))
    indices=np.arange(n)
    for ix,x in feature_batches(features,indices,mean,std,2048):
        rp,tp,(_,_,hidden)=net.forward(x);rp_all[ix]=rp;tp_all[ix]=tp
        embeddings[ix]=hidden/np.maximum(1e-8,np.linalg.norm(hidden,axis=1,keepdims=True))
    embeddings.flush()
    del embeddings
    embedding_temp.replace(Path(output)/'embeddings.f16')
    progress('predict',round_index,round_index,{'instances':n,'round':round_index})
    return rp_all,tp_all


def predictions(rp,tp,type_roles):
    """Select a compatible role/type pair without hard-gating on the role head.

    The type head has no training target for body/connection/other ink, so those
    roles remain independent candidates and never inherit a meaningless type.
    Products preserve disagreement between heads in the reported score.
    """
    import numpy as np
    type_roles=np.asarray(type_roles,dtype='i4')
    if rp.ndim!=2 or tp.ndim!=2 or len(rp)!=len(tp) or tp.shape[1]!=len(type_roles):
        raise ValueError('Role/type scores and vocabulary dimensions differ')
    if np.any(~np.isin(type_roles,[1,2,3])) or np.any(type_roles>=rp.shape[1]):
        raise ValueError('Type vocabulary must map to dot, mark or symbol roles')
    role_only=rp.copy();role_only[:,np.unique(type_roles)]=0
    candidates=np.concatenate((role_only,tp*rp[:,type_roles]),axis=1)
    winner=candidates.argmax(1);has_type=winner>=rp.shape[1]
    r=winner.copy();t=np.full(len(rp),-1,dtype='i4')
    t[has_type]=winner[has_type]-rp.shape[1];r[has_type]=type_roles[t[has_type]]
    score=candidates[np.arange(len(rp)),winner]
    ordered=np.partition(candidates,-2,axis=1);margin=ordered[:,-1]-ordered[:,-2]
    return r,t,score,margin


def inventory_error(expected,observed):
    return sum(abs(expected.get(k,0)-observed.get(k,0)) for k in set(expected)|set(observed))


def refine(db,rows,rp,tp,type_names,type_roles):
    """Infer within fixed source masks. Frozen human/draft ink is never relabelled.
    Only accept reassignments that reduce transcript inventory error; the visual
    model must also have a meaningful score/margin. No invented ownership masks.
    """
    import numpy as np
    from scipy.optimize import linear_sum_assignment
    import sys
    teacher_root=Path(__file__).resolve().parents[2]/'medinamushafanalyzer/tools'
    if str(teacher_root) not in sys.path:sys.path.insert(0,str(teacher_root))
    from prepare_vector_review import slots,CAPACITY
    by_sub={}
    for i,row in enumerate(rows):
        if row['subword_id'] and row.get('in_scope',1):by_sub.setdefault(row['subword_id'],[]).append(i)
    changes={};before=after=0;eligible=0;changed_subwords=0
    name_index={name:i for i,name in enumerate(type_names)}
    predicted_roles,predicted_types,scores,margins=predictions(rp,tp,type_roles)
    for sub in db.execute('SELECT id,expected FROM subwords WHERE in_scope=1'):
        expected_slots=slots({'characters':json.loads(sub['expected'])})
        expected=[s['type'] for s in expected_slots if s['type']!='small-kaf-body']
        indices=[i for i in by_sub.get(sub['id'],[]) if rows[i]['teacher_kind'] in ('dot','mark','symbol','unknown')]
        if not indices or len(indices)>16 or len(expected)>16:continue
        inventory=Counter(expected);current=Counter(rows[i]['teacher_type'] for i in indices if rows[i]['teacher_type']!='unknown')
        initial=inventory_error(inventory,current);before+=initial
        if not initial:after+=initial;continue
        eligible+=1
        # Missing and unused are explicit options in the square assignment.
        n,m=len(indices),len(expected);cost=np.full((n+m,n+m),6.,dtype='f8');cost[n:,m:]=0
        for a,i in enumerate(indices):
            row=rows[i];frozen=row['origin'] in ('human-reviewed','human-draft')
            cost[a,m+a]=1.5
            for b,name in enumerate(expected):
                if name not in name_index:continue
                if name=='triple-dot' and row['teacher_type']!='triple-dot':continue
                ti=name_index[name];role=type_roles[ti];prob=float(tp[i,ti]*rp[i,role])
                cap=CAPACITY.get(name,1)
                if row['contours']!=cap:continue
                if frozen and name!=row['teacher_type']:continue
                prior=.25 if name!=row['teacher_type'] else 0.
                cost[a,b]=-math.log(max(1e-9,prob))+prior
        for b in range(m):cost[n+b,b]=1.6
        aa,bb=linear_sum_assignment(cost);proposed={}
        for a,b in zip(aa,bb):
            if a<n and b<m and cost[a,b]<1.3:
                i=indices[a];name=expected[b]
                if name!=rows[i]['teacher_type'] and rows[i]['origin']=='machine-weak':
                    ti=name_index[name];score=float(tp[i,ti]*rp[i,type_roles[ti]])
                    if score>=.75 and margins[i]>=.45:proposed[i]=(ROLES[type_roles[ti]],name,score)
        new=Counter(proposed.get(i,(None,rows[i]['teacher_type']))[1] for i in indices if proposed.get(i,(None,rows[i]['teacher_type']))[1]!='unknown')
        final=inventory_error(inventory,new)
        if final<initial:
            changes.update(proposed);changed_subwords+=1;after+=final
        else:after+=initial
    return changes,{'reference':'transcript inventory consistency; not mask/ownership accuracy',
        'countErrorBefore':before,'countErrorAfter':after,'eligibleSubwords':eligible,'changedSubwords':changed_subwords,'changedInstances':len(changes)}


def train_loop(output,rounds,epochs,progress,labels=None,hard_example_model=None):
    import numpy as np
    output=Path(output);db=sqlite3.connect(output/'corpus.sqlite');db.row_factory=sqlite3.Row
    from .labels import load
    label_manifest,targets=load(output,labels)
    # Target values come only from the frozen dataset, never live proposals or
    # the model's previous guesses. Reviewed and automatic targets share a schema.
    rows=[dict(id=t['item_id'],feature_row=t['feature_row'],split=t['split'],
        teacher_kind=t['role'] or 'unknown',teacher_type=t['type'] or 'unknown',
        quality=t['quality'],origin='human-reviewed' if t['human_reviewed'] else 'machine-weak' if t['provenance']=='automatic' else 'unresolved',
        bases=t['bases'],in_scope=int(t['role'] is not None)) for t in targets]
    features=np.memmap(output/'features.u8',mode='r',dtype='u1',shape=(len(rows),FEATURE_DIM))
    roles=np.asarray([ROLES.index(row['teacher_kind']) if row['in_scope'] and row['teacher_kind'] in ROLES else -1 for row in rows],dtype='i4')
    quality=np.asarray([row['quality'] for row in rows],dtype='f4');splits=np.asarray([row['split'] for row in rows])
    type_names=type_vocabulary(rows)
    if len(type_names)<2:raise ValueError('Need at least two mark types with usable training examples')
    type_index={name:i for i,name in enumerate(type_names)}
    types=np.asarray([type_index.get(row['teacher_type'],-1) if row['in_scope'] and row['teacher_kind'] in ('dot','mark','symbol') else -1 for row in rows],dtype='i4')
    human=np.asarray([row['origin']=='human-reviewed' for row in rows])&(roles>=0)
    type_roles=[ROLES.index(label_manifest['typeRoles'][name]) for name in type_names]
    train_pool=np.flatnonzero((splits=='train')&(roles>=0));valid_pool=np.flatnonzero((splits=='validation')&(roles>=0)&(quality>=.60))
    mined=np.array([],dtype='i8');mining=None
    if hard_example_model:
        mined,mining=mine_hard_examples(hard_example_model,features,rows)
        (output/'models/hard-examples.json').write_text(json.dumps(mining,ensure_ascii=False,indent=2))
    if len(valid_pool)<100:raise ValueError('Need at least 100 eligible validation examples from separate pages')
    validation=sample_indices(valid_pool,roles,types,quality,350,102,min_quality=.60)
    validation=supplement_types(validation,np.flatnonzero((splits=='validation')&(roles>=0)),types,quality,350,103)
    validation_bodies,_=body_hard_negatives(rows,222,limit=100,split='validation')
    validation=np.unique(np.concatenate((validation,validation_bodies,np.flatnonzero(human&(splits=='validation')))))
    history=[];best=None;best_score=-1;best_round=0
    for round_index in range(1,rounds+1):
        threshold=[.60,.45,.25][min(round_index-1,2)];per_type=[900,1400,1800][min(round_index-1,2)]
        indices=sample_indices(train_pool,roles,types,quality,per_type,42+round_index,min_quality=threshold)
        hard_negatives,hard_negative_counts=body_hard_negatives(rows,1000+round_index)
        indices=np.unique(np.concatenate((indices,hard_negatives,mined,np.flatnonzero(human&(splits=='train')))))
        # Include a few examples of low-frequency weak types instead of pretending
        # those types were learned from zero training instances.
        indices=supplement_types(indices,train_pool,types,quality,per_type,900+round_index)
        rng=np.random.default_rng(900+round_index)
        sample=np.asarray(features[indices],dtype='f4')/255;mean=sample.mean(0);std=np.maximum(.12,sample.std(0));sample=(sample-mean)/std
        # Fit normalization on unique train examples. Repetition changes exposure,
        # never the split, source ink, or number of distinct observations.
        draws=training_draw_positions(types[indices],seed=1400+round_index)
        type_support={name:{'trainingAvailable':int(np.sum((splits=='train')&(types==ti)&(quality>=.2))),
            'trainingUnique':int(np.sum(types[indices]==ti)),
            'trainingDrawsPerEpoch':int(np.sum(types[indices[draws]]==ti)),
            'humanTraining':int(np.sum(human[indices]&(types[indices]==ti))),
            'validationAvailable':int(np.sum((splits=='validation')&(types==ti)&(quality>=.2))),
            'validationHuman':int(np.sum(human&(splits=='validation')&(types==ti))),
            'testAvailable':int(np.sum((splits=='test')&(types==ti)&(quality>=.2))),
            'testHuman':int(np.sum(human&(splits=='test')&(types==ti)))} for ti,name in enumerate(type_names)}
        counts=Counter(map(int,draws))
        (output/'models'/f'round-{round_index}-samples.json').write_text(json.dumps({
            'trainingLabelsSha256':label_manifest['labelsSha256'],
            'examples':[{'id':rows[i]['id'],'drawsPerEpoch':counts[j]} for j,i in enumerate(indices)]}))
        hidden=[96,128,192][min(round_index-1,2)];net=Network(FEATURE_DIM,hidden,len(type_names),42+round_index)
        best_epoch=None;epoch_score=-1;epochs_this=epochs+4*(round_index-1);curve=[]
        weight=np.maximum(.15,quality[indices]);weight[np.asarray([rows[i]['origin']=='human-reviewed' for i in indices])]=2.
        for epoch in range(1,epochs_this+1):
            order=rng.permutation(draws);losses=[];lr=.0015*(.2+.8*.5*(1+math.cos(math.pi*(epoch-1)/epochs_this)))
            for start in range(0,len(order),256):
                positions=order[start:start+256];ix=indices[positions]
                losses.append(net.batch(sample[positions],roles[ix],types[ix],weight[positions],lr))
            metric=evaluate(net,features,validation,roles,types,mean,std,type_names,type_roles)
            curve.append({'epoch':epoch,'trainingLoss':float(np.mean(losses)),'validationLoss':metric['loss'],'validationRoleF1':metric['role']['macroF1'],'validationTypeF1':metric['type']['macroF1']})
            if metric['score']>epoch_score:
                epoch_score=metric['score'];best_epoch=copy.deepcopy(net.p)
            progress('train',epoch,epochs_this,{'round':round_index,'trainExamples':len(indices),**curve[-1]})
        net.p=best_epoch
        metric=evaluate(net,features,validation,roles,types,mean,std,type_names,type_roles)
        path=output/'models'/f'round-{round_index}.npz';np.savez_compressed(path,**net.p,mean=mean,std=std)
        record={'round':round_index,'trainingExamples':len(indices),'trainingPages':len({rows[i]['id'].split(':')[0] for i in indices}),
            'validationExamples':len(validation),'hiddenUnits':hidden,'validation':metric,'curve':curve,'modelFile':path.name,
            'automaticSupervision':True,'humanReferenceExamples':sum(rows[i]['origin']=='human-reviewed' for i in validation),
            'trainingDrawsPerEpoch':len(draws),'typeSupport':type_support,
            'humanValidation':reviewed_metrics(net,features,np.flatnonzero(human&(splits=='validation')),rows,mean,std,type_names,type_roles),
            'modelSha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        history.append(record)
        (output/'rounds.json').write_text(json.dumps({'roles':ROLES,'types':type_names,'rounds':history},indent=2))
        if metric['score']>best_score:
            best_score=metric['score'];best_round=round_index;best=(copy.deepcopy(net.p),mean.copy(),std.copy(),hidden)
        progress('round-complete',round_index,rounds,{'round':round_index,'validationRoleF1':metric['role']['macroF1'],'validationTypeF1':metric['type']['macroF1'],'bestRound':best_round})
        if round_index>=2 and best_score>=.985 and history[-1]['validation']['score']-history[-2]['validation']['score']<.001:break
    weights,mean,std,hidden=best;selected=Network(FEATURE_DIM,hidden,len(type_names));selected.p=weights
    predictions_file=output/'models'/'selected.npz';np.savez_compressed(predictions_file,**weights,mean=mean,std=std)
    test_pool=np.flatnonzero((splits=='test')&(roles>=0)&(quality>=.60))
    test=sample_indices(test_pool,roles,types,quality,350,777,min_quality=.60)
    test=supplement_types(test,np.flatnonzero((splits=='test')&(roles>=0)),types,quality,350,778)
    test_bodies,_=body_hard_negatives(rows,779,limit=100,split='test')
    test=np.unique(np.concatenate((test,test_bodies,np.flatnonzero(human&(splits=='test')))))
    test_metric=evaluate(selected,features,test,roles,types,mean,std,type_names,type_roles) if len(test) else None
    metadata={'architecture':f'Masked source region + local context + geometry; shared MLP {FEATURE_DIM}-{hidden}-64 with separate role/type heads',
        'framework':'NumPy CPU; SVG/PDF masks supply geometry, not a trained mask decoder','roles':ROLES,'types':type_names,
        'selectedRound':best_round,'validationScore':best_score,'roundCount':len(history),'rounds':history,
        'typeRoles':type_roles,'trainingLabelsSha256':label_manifest['labelsSha256'],
        'decoder':decoder_policy(),
        'hardExampleMining':{k:v for k,v in mining.items() if k!='itemIds'} if mining else None,
        'typeSupport':history[best_round-1]['typeSupport'],
        'rareTypePolicy':{'minimumEligibleTrainingExamples':1,'targetDrawsPerEpoch':128,'maximumRepeatsPerExample':8,
            'weakOnlyTypeBudget':'Supplement eligible quality >= 0.2 examples up to each round per-type budget; held-out type coverage capped at 350.',
            'note':'Training examples only. Repetition is not new evidence. Human training labels are always included.'},
        'untrainedTypes':sorted({row['teacher_type'] for row in rows if row['in_scope'] and row['teacher_kind'] in ('dot','mark','symbol')
            and row['teacher_type'] not in ('unknown',None)}-set(type_names)),
        'modelSha256':hashlib.sha256(predictions_file.read_bytes()).hexdigest(),'hardNegativeBodies':hard_negative_counts,
        'testAfterSelection':test_metric,'inventory':None,'humanReferenceAvailable':any(row['in_scope'] and row['origin']=='human-reviewed' and row['split']!='train' for row in rows),
        'humanValidation':history[best_round-1]['humanValidation'],
        'humanTestAfterSelection':reviewed_metrics(selected,features,np.flatnonzero(human&(splits=='test')),rows,mean,std,type_names,type_roles),
        'scopePolicy':'Exterior ink is excluded from fitting, normalization, validation, test sampling and transcript refinement.',
        'metricWarning':'All automatic-label metrics are teacher agreement or transcript consistency, not measured human-ground-truth segmentation accuracy.',
        'trainingRule':'Frozen independent training-label dataset; automatic and reviewed provenance retained. Rounds never replace targets with own predictions.',
        'testPolicy':'No test pages in fitting, normalization, pseudo-label sampling, early stopping, or round selection.'}
    db.execute('INSERT OR REPLACE INTO meta VALUES (?,?)',('learning',json.dumps(metadata)));db.commit();db.close()
    (output/'models'/'model.json').write_text(json.dumps(metadata,indent=2))
    progress('training-complete',len(rows),len(rows),{'selectedRound':best_round,'trainingLabelsSha256':label_manifest['labelsSha256'],'proxyValidationScore':best_score})
    return metadata
