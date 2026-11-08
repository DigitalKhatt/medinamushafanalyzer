"""Conservative adapter from vector proposals to immutable workbench source ink."""
from array import array
import copy
import hashlib
import json
import re


def proposal_digest(proposal):
    return hashlib.sha256(json.dumps(proposal,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def reconcile(doc, proposal):
    if proposal.get('schemaVersion')!=1 or proposal['pageId']!=doc['pageId'] or proposal['sourceSha256']!=doc['sourceSha256']:
        raise ValueError('Proposal belongs to a different source SVG.')
    if proposal['raster']!=doc['raster']:
        raise ValueError('Proposal raster/transform differs from this annotation. Regenerate proposals for this raster.')
    width,height=doc['raster']['width'],doc['raster']['height']
    size=width*height
    labels=array('I',[0])*size
    for i,part in enumerate(doc['parts'],1):
        for start,length in zip(part['runs'][::2],part['runs'][1::2]): labels[start:start+length]=array('I',[i])*length
    path_map={p['id']:p for p in proposal['paths']}
    candidates=copy.deepcopy(proposal['candidates'])
    # Include source ink omitted by the old cache in the same review queue.
    counts=[]; winners={}; digest=proposal_digest(proposal)
    for ci,candidate in enumerate(candidates):
        hits={}; predicted=0
        for pid in candidate['pathIds']:
            path=path_map[pid]; runs=path['runs']
            if len(runs)%2: raise ValueError('Invalid proposal runs')
            for start,length in zip(runs[::2],runs[1::2]):
                if type(start)!=int or type(length)!=int or start<0 or length<1 or start+length>size:
                    raise ValueError('Proposal mask exceeds source raster.')
                predicted+=length
                for label in labels[start:start+length]:
                    if label: hits[label]=hits.get(label,0)+1
        counts.append((hits,predicted))
        for label,n in hits.items():
            winners.setdefault(label,[]).append((n,ci))
    matched={}; associated=set(); union_area=0
    for label,choices in winners.items():
        choices.sort(reverse=True)
        count,ci=choices[0]; part=doc['parts'][label-1]
        # Exact intersections, without dilation or moving source pixels.
        # Tiny internal dots can lose several edge pixels between the PDF and
        # cached SVG rasterizations. Accept only a uniquely overlapping dot
        # inside a multi-contour waqf-jeem path, retaining its exact source ink.
        candidate=candidates[ci]
        tiny_jeem=False
        if (candidate.get('markType')=='waqf-jeem' and not part['reviewed']
                and part['role']=='unknown' and not part.get('owner')
                and part['area']<=25 and max(part['bbox'][2:])<=7
                and count/part['area']>=.40):
            x,y,w,h=part['bbox']
            for pid in candidate['pathIds']:
                path=path_map[pid];box=path.get('bbox')
                if box and path.get('contourCount',0)>=2:
                    px,py,pw,ph=box
                    if px<=x and py<=y and x+w<=px+pw and y+h<=py+ph:
                        tiny_jeem=True
        if (count/part['area']>=.80 or tiny_jeem) and (len(choices)==1 or choices[1][0]/part['area']<.10):
            matched.setdefault(ci,[]).append(part)
            associated.add(label); union_area+=part['area']
    records=doc.get('proposalReview',{})
    part_numbers={p['id']:i for i,p in enumerate(doc['parts'],1)}
    for ci,candidate in enumerate(candidates):
        regions=matched.get(ci,[])
        hits,predicted=counts[ci]
        overlap=sum(hits.get(part_numbers[p['id']],0) for p in regions)
        source_area=sum(p['area'] for p in regions)
        mask_precision=overlap/max(1,predicted)
        candidate['regionIds']=[p['id'] for p in regions]
        candidate['match']={'sourceCoverage':overlap/max(1,source_area),'proposalCoverage':mask_precision}
        candidate['canApply']=bool(regions) and mask_precision>=.80
        if not candidate['canApply']: candidate['issues'].append('Geometry unresolved: correct this ink in the workbench')
        record=records.get(candidate['id'])
        candidate['review']=record if record and record.get('proposalSha256')==digest else None
        if candidate['review'] and record.get('decision')=='approved':
            current=[p for p in doc['parts'] if p.get('proposalSource',{}).get('candidateId')==candidate['id'] and p.get('proposalSource',{}).get('proposalSha256')==digest]
            if {p['id'] for p in current}!=set(record['regionIds']) or any(not p['reviewed'] or p['role']!=record['role'] or p.get('markType')!=record['markType'] for p in current):
                candidate['review']={**record,'decision':'changed'}
        candidate['protected']=any(p['reviewed'] or p['role']!='unknown' or p.get('owner') for p in regions)
    for i,part in enumerate(doc['parts'],1):
        if i in associated: continue
        candidate={'id':'source:'+part['id'],'kind':'unknown','markType':'unknown','pathIds':[],
            'subwordId':None,'wordId':None,'line':part.get('line'),'regionIds':[part['id']],
            'issues':['Source ink absent from, or ambiguous in, legacy proposals'],
            'match':{'sourceCoverage':1,'proposalCoverage':1},'canApply':True,
            'protected':part['reviewed'] or part['role']!='unknown' or bool(part.get('owner'))}
        record=records.get(candidate['id'])
        candidate['review']=record if record and record.get('proposalSha256')==digest else None
        if candidate['review'] and record.get('decision')=='approved' and (not part['reviewed'] or part['role']!=record['role'] or part.get('markType')!=record['markType'] or part.get('proposalSource',{}).get('proposalSha256')!=digest):
            candidate['review']={**record,'decision':'changed'}
        candidates.append(candidate)
    applied={pid for c in candidates if c['pathIds'] and c['canApply'] for pid in c['regionIds']}
    diagnostics={}
    for label,part in enumerate(doc['parts'],1):
        if part['id'] in applied:continue
        choices=sorted(winners.get(label,[]),reverse=True)
        matches=[]
        for n,ci in choices[:4]:
            candidate=candidates[ci]
            matches.append({'candidateId':candidate['id'],'kind':candidate['kind'],
                'wordId':candidate.get('wordId'),'subwordId':candidate.get('subwordId'),
                'regionCoverage':min(1,n/part['area']),'proposalCoverage':min(1,n/max(1,counts[ci][1]))})
        if not matches:reason='legacy-omission'
        elif len(matches)>1 and matches[1]['regionCoverage']>=.10:reason='ambiguous-geometry'
        elif matches[0]['regionCoverage']<.80:reason='incomplete-geometry'
        else:reason='proposal-mismatch'
        diagnostics[part['id']]={'reason':reason,'matches':matches}
    subword_reviews={id:({**sub,'decision':'changed'} if sub.get('reviewed') and sub.get('proposalSha256') not in (None,digest) else sub) for id,sub in doc.get('subwords',{}).items()}
    return {**proposal,'candidates':candidates,'proposalSha256':digest,
        'alignment':{'matchedInkPixels':union_area,'totalInkPixels':doc['inkPixels'],
                     'matchedFraction':union_area/doc['inkPixels'],'unresolvedRegions':len(doc['parts'])-len(associated)},
        'subwordReview':subword_reviews,'sourceDiagnostics':diagnostics}


def combine_runs(parts):
    spans=sorted((s,n) for p in parts for s,n in zip(p['runs'][::2],p['runs'][1::2]))
    runs=[]
    for start,length in spans:
        if runs and runs[-2]+runs[-1]==start: runs[-1]+=length
        else: runs.extend((start,length))
    return runs


def apply_review(doc, reconciled, request, describe_runs):
    if request.get('proposalSha256')!=reconciled['proposalSha256']:
        raise ValueError('Proposals changed. Reload the review page.')
    action=request.get('action')
    if action in ('approve','approve-subword'):
        collection=reconciled['subwords'] if action=='approve-subword' else reconciled['candidates']
        target=next((c for c in collection if c['id']==request.get('id')),None)
        if target and target.get('inScope') is False:raise ValueError('This ink is outside the frame interior.')
    if action not in ('approve','reject','reopen','approve-subword','reject-subword','reopen-subword'):
        raise ValueError('Unknown review action.')
    output=copy.deepcopy(doc)
    records=output.setdefault('proposalReview',{})
    subwords=output.setdefault('subwords',{})
    digest=reconciled['proposalSha256']
    index={c['id']:c for c in reconciled['candidates']}
    if action.endswith('subword'):
        sub=next((s for s in reconciled['subwords'] if s['id']==request.get('id')),None)
        if not sub: raise ValueError('Subword not found.')
        if action=='reopen-subword':
            if sub['id'] not in subwords: raise ValueError('Subword has no review to reopen.')
            subwords[sub['id']].update(reviewed=False,decision='pending')
            return output
        if action=='reject-subword':
            existing=subwords.get(sub['id'])
            if existing and existing.get('reviewed'): raise ValueError('Reopen approved context before replacing it.')
            subwords[sub['id']]={'text':sub['text'],'reviewed':False,'decision':'rejected','proposalSha256':digest}
            return output
        text=request.get('text',sub['text'])
        if not isinstance(text,str) or not text.strip() or len(text)>256: raise ValueError('Subword text is required.')
        selected=request.get('bodyCandidateIds',sub['bodyCandidateIds'])
        if not isinstance(selected,list) or not selected or len(set(selected))!=len(selected) or any(cid not in sub['candidateIds'] for cid in selected):
            raise ValueError('Select the ink forming this subword body.')
        body_ids=set()
        for cid in selected:
            candidate=index[cid]
            if not candidate['canApply']: raise ValueError('Resolve body geometry in the workbench before approving the subword.')
            body_ids.update(candidate['regionIds'])
        for p in output['parts']:
            if p.get('subwordId')==sub['id'] and p.get('subwordBody') and p['id'] not in body_ids:
                p['subwordBody']=False
                p['subwordAttachmentReviewed']=False
            if p['id'] in body_ids:
                if p.get('subwordId') not in (None,sub['id']): raise ValueError('Ink already belongs to another subword; edit its boundary in the workbench.')
                if p['id'] in body_ids and p['role'] in ('dot','mark','symbol','ornament','other') and p['reviewed']:
                    raise ValueError('Selected body ink has an approved mark/sign label. Reopen that label before changing its purpose.')
                p['subwordId']=sub['id']; p['subwordBody']=True
        subwords[sub['id']]={'text':text.strip(),'line':sub['line'],'wordId':sub['wordId'],
            'wordText':sub['wordText'],'reviewed':True,'decision':'approved','proposalSha256':digest}
        return output
    candidate=index.get(request.get('id'))
    if not candidate: raise ValueError('Candidate not found.')
    prior=records.get(candidate['id'])
    if action=='reopen':
        if not prior: raise ValueError('Candidate has no review to reopen.')
        if prior.get('decision')=='approved':
            associated=[p for p in output['parts'] if p.get('proposalSource',{}).get('candidateId')==candidate['id'] and p.get('proposalSource',{}).get('proposalSha256')==prior.get('proposalSha256')]
            if (set(prior['regionIds'])!={p['id'] for p in associated} or
                any(not p['reviewed'] or p['role']!=prior['role'] or p.get('markType')!=prior['markType'] for p in associated)):
                raise ValueError('This ink has changed in the workbench. Reopen it there to preserve those edits.')
            for part in associated:
                part.update(role='unknown',reviewed=False)
                if prior.get('attachmentReviewed'):
                    if part.get('subwordId')!=prior.get('subwordId'):
                        raise ValueError('Subword attachment changed in the workbench; reopen it there.')
                    part['subwordAttachmentReviewed']=False
                part.pop('markType',None); part.pop('proposalSource',None); part.pop('reviewAttribution',None)
        records[candidate['id']]={'decision':'pending','proposalSha256':digest}
        return output
    if prior and prior.get('decision')=='approved': raise ValueError('This ink was already approved. Reopen or edit it in the workbench.')
    if action=='reject':
        records[candidate['id']]={'decision':'rejected','proposalSha256':digest}
        return output
    if not candidate['canApply']: raise ValueError('Resolve this candidate’s geometry in the workbench first.')
    role=request.get('role',candidate['kind'])
    if role not in ('dot','mark','symbol','other'): raise ValueError('Approve letter bodies as subword context, or cut characters in the workbench.')
    subtype=request.get('markType',candidate['markType']) or 'unknown'
    if not isinstance(subtype,str) or not re.fullmatch(r'[a-z][a-z0-9_-]{0,63}',subtype): raise ValueError('Use a lowercase mark type tag.')
    region_ids=set(candidate['regionIds'])
    parts=[p for p in output['parts'] if p['id'] in region_ids]
    if candidate['protected']: raise ValueError('Existing labels are protected. Edit or reopen them in the workbench.')
    common_subword={p.get('subwordId') for p in parts}
    if len(common_subword)>1: raise ValueError('Cannot merge ink assigned to different subwords.')
    merged={**parts[0],'runs':combine_runs(parts),'role':role,'reviewed':True,'markType':subtype,
        'proposalSource':{'candidateId':candidate['id'],'proposalSha256':digest},'line':candidate['line']}
    merged.pop('landmarks',None)
    merged['subwordAttachmentReviewed']=all(p.get('subwordAttachmentReviewed',False) for p in parts)
    merged['area'],merged['bbox']=describe_runs(merged['runs'],doc['raster']['width'])
    # Approval of a mark does not approve a suggested letter owner or subword boundary.
    for part in parts:
        if part.get('subwordBody'):
            subwords[part['subwordId']].update(reviewed=False,decision='pending')
    merged['subwordBody']=False
    if request.get('attachSubword'):
        sub=next((s for s in reconciled['subwords'] if s['id']==candidate.get('subwordId')),None)
        if not sub: raise ValueError('No proposed subword attachment to validate.')
        if merged.get('subwordId') not in (None,sub['id']): raise ValueError('Ink already has another subword assignment. Edit it in the workbench.')
        subwords.setdefault(sub['id'],{'text':sub['text'],'wordText':sub['wordText'],'wordId':sub['wordId'],
            'line':sub['line'],'reviewed':False,'decision':'pending','proposalSha256':digest})
        merged.update(subwordId=sub['id'],subwordBody=False,subwordAttachmentReviewed=True)
    output['parts']=[p for p in output['parts'] if p['id'] not in region_ids]+[merged]
    records[candidate['id']]={'decision':'approved','role':role,'markType':subtype,
        'regionIds':[merged['id']],'proposalSha256':digest,
        'attachmentReviewed':bool(request.get('attachSubword')),'subwordId':merged.get('subwordId')}
    if request.get('reviewAttribution') is not None:
        attribution=request['reviewAttribution']
        required=('reviewedBy','recordedBy','basis','statement','recordedAt','batchId')
        if not isinstance(attribution,dict) or any(not isinstance(attribution.get(k),str) or not attribution[k].strip() or len(attribution[k])>2000 for k in required):
            raise ValueError('Review attribution must identify the reviewer, recorder and explicit review statement.')
        merged['reviewAttribution']={k:attribution[k] for k in required}
        records[candidate['id']]['reviewAttribution']=copy.deepcopy(merged['reviewAttribution'])
    return output


def export_subwords(doc, png_bytes,scope=None):
    width,height=doc['raster']['width'],doc['raster']['height']
    pixels=bytearray(b'\xff\xff')*(width*height) if scope else bytearray(width*height*2); records=[]
    if scope:
        for start,length in zip(scope['runs'][::2],scope['runs'][1::2]):pixels[start*2:(start+length)*2]=b'\0\0'*length
    for sid,sub in doc.get('subwords',{}).items():
        if not sub.get('reviewed'): continue
        parts=[p for p in doc['parts'] if p.get('subwordId')==sid and p.get('subwordBody')]
        if not parts: continue
        number=len(records)+1
        if number>65534: raise ValueError('Too many subwords.')
        for part in parts:
            for start,length in zip(part['runs'][::2],part['runs'][1::2]): pixels[start*2:(start+length)*2]=number.to_bytes(2,'big')*length
        records.append({'id':sid,'instanceId':number,**sub,'bodyRegionIds':[p['id'] for p in parts],
            'attachedMarkRegionIds':[p['id'] for p in doc['parts'] if p.get('subwordId')==sid and not p.get('subwordBody') and p.get('subwordAttachmentReviewed')]})
    # Unreviewed ink remains 65535 (ignore), white background 0.
    for part in doc['parts']:
        if not (part.get('subwordBody') and doc.get('subwords',{}).get(part.get('subwordId'),{}).get('reviewed')):
            for start,length in zip(part['runs'][::2],part['runs'][1::2]): pixels[start*2:(start+length)*2]=b'\xff\xff'*length
    return {'schemaVersion':1,'pageId':doc['pageId'],'sourceSha256':doc['sourceSha256'],
        'raster':doc['raster'],'instances':records,'ignoreIndex':65535,
        'note':'Reviewed subword body instances; individual letter and kashida cuts remain unknown.'},png_bytes(width,height,pixels,16)
