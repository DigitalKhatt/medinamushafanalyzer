"""Associate each physical mask with a specific transcript mark or dot group.

This uses source geometry and transcript slots, never learned type predictions.
The classifier is an independent downstream comparison, not an assignment input.
"""
from collections import defaultdict

from automatic.common import unpack_runs
from .geometry import pixels


def geometry(source, width):
    import numpy as np
    from scipy import ndimage as ndi
    from automatic.common import local_mask
    mask=local_mask(unpack_runs(source['runs']),[source[k] for k in ('x','y','w','h')],width)>0
    yy,xx=np.where(mask)
    if len(xx)<3:return dict(stroke=False,vertical=False,counter=False)
    covariance=np.cov(np.stack([xx,yy]))
    values,vectors=np.linalg.eigh(covariance)
    direction=vectors[:,-1];angle=np.arctan2(direction[1],direction[0])
    if angle>np.pi/2:angle-=np.pi
    if angle<-np.pi/2:angle+=np.pi
    components=ndi.label(mask,np.ones((3,3)))[1]
    return dict(stroke=bool(components==1 and values[-1]>=9*max(values[0],.01) and -.85<=angle<=.12),
                vertical=source['h']>=2.2*source['w'],counter=bool((ndi.binary_fill_holes(mask)&~mask).any()))


def shape_cost(features, name):
    if name in ('fatha','kasra'):return 0. if features['stroke'] else 2.
    if name=='small-alef':return 0. if features['vertical'] else 2.
    if name in ('shadda','hamza-above','hamza-below','damma','wasla'):
        return 2. if features['stroke'] else 0.
    if name=='sukun':return 0. if features['counter'] else 1.
    return 0.


def associate(marks, slots, anchors, source, width):
    import numpy as np
    from scipy.optimize import linear_sum_assignment
    byid={r['id']:r for r in source}
    result={m['id']:dict(mark_id=m['id'],page_id=m['page_id'],subword_id=m['subword_id'],slot_id=None,
        expected_type=None,character=None,source_character=None,word_text_offset=None,dot_count=None,
        status='unresolved-owner',method='transcript-slot-and-geometry',margin=None,candidates='[]') for m in marks}
    groups=defaultdict(list)
    for mark in marks:
        if mark['status'] in ('proposed','word-sign'):
            groups[(mark['predicted_letter_id'],mark['kind'])].append(mark)
    import json
    for (owner,kind),group in groups.items():
        expected=[s for s in slots if s['letter_id']==owner and
            (kind=='dot')==s['type'].endswith('-dot') and (kind=='symbol')==(owner is None)]
        if len(group)!=len(expected):
            for mark in group:result[mark['id']]['status']='incomplete-owner-quota'
            continue
        costs=np.zeros((len(group),len(expected)))
        for a,mark in enumerate(group):
            src=byid[mark['id']];features=geometry(src,width)
            cy=src['y']+src['h']/2
            for b,slot in enumerate(expected):
                cost=shape_cost(features,slot['type'])
                body=anchors.get(owner)
                if body:
                    if slot['side']=='above' and cy>body['y']+body['h']:cost+=4
                    if slot['side']=='below' and cy<body['y']:cost+=4
                costs[a,b]=cost
        rr,cc=linear_sum_assignment(costs);best=float(costs[rr,cc].sum())
        for r,c in zip(rr,cc):
            record=result[group[r]['id']];slot=expected[c]
            # All near-optimal alternatives remain inspectable, including
            # interchangeable equal-type occurrences on the same letter.
            possible=[]
            for candidate in range(len(expected)):
                constrained=costs.copy();constrained[r,:]=1e6;constrained[r,candidate]=costs[r,candidate]
                ar,ac=linear_sum_assignment(constrained)
                if float(constrained[ar,ac].sum())-best<.25:possible.append(expected[candidate])
            record['candidates']=json.dumps([s['id'] for s in possible])
            names={s['type'] for s in possible}
            if len(names)!=1:
                record['status']='ambiguous-transcript-mark';continue
            record.update(expected_type=slot['type'],character=slot['character'],
                source_character=slot['source_character'],dot_count=slot['dot_count'],status='type-proposed')
            if len(possible)==1:
                record.update(slot_id=slot['id'],word_text_offset=slot['word_text_offset'],status='proposed')
                slot['mark_id']=group[r]['id']
            if len(expected)>1:
                alternative=costs.copy();alternative[r,c]=1e6
                ar,ac=linear_sum_assignment(alternative)
                record['margin']=float(alternative[ar,ac].sum()-best)
    return list(result.values())
