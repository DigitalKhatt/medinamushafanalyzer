#!/usr/bin/env python3
"""Validate a full regenerated automatic corpus against its frozen baseline."""
from collections import Counter
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3
import sys

import numpy as np
from PIL import Image
from automatic.common import FEATURE_DIM, digest_file, unpack_runs, input_root
from automatic.word_audit import mask_union


def mask_key(runs):
    canonical=[]
    for start,count in zip(runs[::2],runs[1::2]):
        if canonical and canonical[-2]+canonical[-1]==start:canonical[-1]+=count
        else:canonical.extend((start,count))
    return hashlib.sha256(json.dumps(canonical,separators=(',',':')).encode()).hexdigest()


def read_items(db,page):
    fields='id,word_id,subword_id,final_kind,final_type,human_reviewed,origin,in_scope,feature_row,area,runs,model_confidence,review_candidate'
    result=[dict(row) for row in db.execute(f'SELECT {fields} FROM items WHERE page_id=?',(page,))]
    for row in result:
        row['runs']=unpack_runs(row['runs']);row['maskKey']=mask_key(row['runs'])
    return result


def reviewed_source_transition(previous,document,new_index):
    """Validate an existing approval on its actual saved workbench mask.

    This checks already reviewed input, never creates an approval or transfers
    a label by proximity. The prior adapter and the saved approval must identify
    the same original proposal, role/type and word/subword owners.
    """
    candidate=previous.get('review_candidate')
    if not candidate:return None
    matches=[]
    for part in document['parts']:
        if not part['reviewed'] or part.get('proposalSource',{}).get('candidateId')!=candidate:continue
        row=new_index.get(mask_key(part['runs']))
        if not row or not row['human_reviewed'] or row.get('review_candidate')!=candidate:continue
        if (row['final_kind'],row['final_type'])!=(previous['final_kind'],previous['final_type']):continue
        if (row['word_id'],row['subword_id'])!=(previous['word_id'],previous['subword_id']):continue
        if (part['role'],part.get('markType','unknown'))!=(row['final_kind'],row['final_type']):continue
        a={p for s,n in zip(previous['runs'][::2],previous['runs'][1::2]) for p in range(s,s+n)}
        b={p for s,n in zip(row['runs'][::2],row['runs'][1::2]) for p in range(s,s+n)}
        coverage_old=len(a&b)/len(a);coverage_new=len(a&b)/len(b)
        if min(coverage_old,coverage_new)<.8:continue
        matches.append(dict(beforeId=previous['id'],afterId=row['id'],reviewCandidate=candidate,
            originalApproval=part['proposalSource'],beforeArea=len(a),afterArea=len(b),
            overlapOfBefore=coverage_old,overlapOfAfter=coverage_new,afterMaskKey=row['maskKey']))
    return matches[0] if len(matches)==1 else None


def compare(stage,baseline=None):
    stage=Path(stage).resolve();before=Path(baseline).resolve() if baseline else stage/'before'
    if not (stage/'inference.json').exists():raise ValueError('Finish model inference before regression validation')
    output=stage/'regression';output.mkdir(exist_ok=True)
    if (stage/'regeneration-manifest.json').exists():
        model_sha=json.loads((stage/'regeneration-manifest.json').read_text())['modelSha256']
    else:model_sha=json.loads((stage/'rebuild-manifest.json').read_text())['modelBundle']['baseModelSha256']
    meta_before=json.loads((before/'inference.json').read_text());meta_after=json.loads((stage/'inference.json').read_text())
    assert meta_before['modelSha256']==meta_after['modelSha256']==model_sha,'Model changed'
    active_models={'base':model_sha}
    for key in ('hamzaClassifier','shapePairClassifier'):
        previous=meta_before.get(key,{}).get('weightsSha256');current=meta_after.get(key,{}).get('weightsSha256')
        assert previous==current,'Active expert changed: '+key
        if current:active_models[key]=current
    assert meta_before.get('networkOutputs')==meta_after.get('networkOutputs'),'Model vocabulary changed'
    counts=Counter();pages=[];labels=[];owners=[];scope=[];lost_human=[];new_human=[];new_regions=[];old_regions=[];review_transitions=[]
    feature_pairs=[]
    with closing(sqlite3.connect((before/'corpus.sqlite').as_uri()+'?mode=ro',uri=True)) as a, closing(sqlite3.connect((stage/'corpus.sqlite').as_uri()+'?mode=ro',uri=True)) as b:
        a.row_factory=b.row_factory=sqlite3.Row;a.execute('BEGIN');b.execute('BEGIN')
        old_pages={r['page_id']:dict(r) for r in a.execute('SELECT * FROM pages')}
        current_pages={r['page_id']:dict(r) for r in b.execute('SELECT * FROM pages')}
        assert set(old_pages)==set(current_pages) and len(current_pages)==604
        old_n=a.execute('SELECT COUNT(*) FROM items').fetchone()[0];new_n=b.execute('SELECT COUNT(*) FROM items').fetchone()[0]
        assert b.execute('SELECT MIN(feature_row),MAX(feature_row),COUNT(DISTINCT feature_row) FROM items').fetchone()[:]==(0,new_n-1,new_n)
        assert (stage/'features.u8').stat().st_size==new_n*FEATURE_DIM
        assert (stage/'embeddings.f16').stat().st_size==new_n*64*2
        for pid,newpage in sorted(current_pages.items()):
            oldpage=old_pages[pid];assert newpage['source_sha']==oldpage['source_sha'] and newpage['split']==oldpage['split']
            assert (newpage['width'],newpage['height'])==(oldpage['width'],oldpage['height'])
            old=read_items(a,pid);new=read_items(b,pid)
            size=newpage['width']*newpage['height'];u0=mask_union(old,size);u1=mask_union(new,size)
            expected_file=input_root(stage)/'annotations'/f'{pid}.json'
            source_changed=u0!=u1
            if expected_file.exists():
                doc=json.loads(expected_file.read_text());expected=mask_union(doc['parts'],size)
                assert u1==expected,(pid,'Regenerated ink differs from saved workbench ink')
                assert newpage['annotation_revision']==doc['revision']
            else:
                proposal=json.loads((stage/'proposals'/f'{pid}.json').read_text());raster=proposal['raster']
                rgb=np.asarray(Image.open(stage/'render'/f'{pid}.png').convert('RGB'))
                expected=((rgb.mean(axis=2)<raster['threshold'])&((rgb.max(axis=2).astype('i2')-rgb.min(axis=2))<=raster['chroma'])).astype('u1')
                assert np.array_equal(np.frombuffer(u1,dtype='u1'),expected.ravel()),(pid,'Regenerated ink differs from rendered source')
                assert not source_changed,(pid,'Unexplained renderer/source-ink change')
            assert sum(u1)==newpage['ink_pixels']
            assert sum(r['area'] for r in new if r['in_scope'])==newpage['scope_ink_pixels']
            assert oldpage['pdf_sha']==newpage['pdf_sha']
            old_index={r['maskKey']:r for r in old};new_index={r['maskKey']:r for r in new}
            assert len(old_index)==len(old) and len(new_index)==len(new)
            reviewed_before=[r for r in old if r['human_reviewed']]
            transition_keys=set()
            for row in reviewed_before:
                target=new_index.get(row['maskKey'])
                if not target or not target['human_reviewed'] or (target['final_kind'],target['final_type'])!=(row['final_kind'],row['final_type']):
                    transition=reviewed_source_transition(row,doc,new_index) if expected_file.exists() and source_changed and oldpage['source_kind']!=newpage['source_kind'] else None
                    if transition:
                        review_transitions.append(dict(page=pid,**transition));transition_keys.add(transition['afterMaskKey'])
                    else:lost_human.append(dict(page=pid,id=row['id'],role=row['final_kind'],type=row['final_type']))
            latest_reviewed=[]
            if expected_file.exists():
                for part in doc['parts']:
                    if not part['reviewed']:continue
                    key=mask_key(part['runs']);match=new_index.get(key)
                    assert match and match['human_reviewed'],(pid,'Reviewed annotation not preserved',part['id'])
                    assert match['final_kind']==part['role'],(pid,'Reviewed annotation role changed',part['id'])
                    latest_reviewed.append(key)
            for key,row in new_index.items():
                previous=old_index.get(key)
                if row['human_reviewed'] and (not previous or not previous['human_reviewed']) and key not in transition_keys:new_human.append(dict(page=pid,id=row['id'],role=row['final_kind']))
                if not previous:
                    new_regions.append(dict(page=pid,id=row['id'],area=row['area'],role=row['final_kind'],type=row['final_type'],humanReviewed=row['human_reviewed']));continue
                feature_pairs.append((pid,row['id'],previous['feature_row'],row['feature_row'],source_changed))
                reason='new-human-review' if row['human_reviewed'] and not previous['human_reviewed'] else 'source-ink-refresh' if source_changed else 'machine-change'
                if (row['final_kind'],row['final_type'])!=(previous['final_kind'],previous['final_type']):
                    labels.append(dict(page=pid,id=row['id'],beforeId=previous['id'],before=[previous['final_kind'],previous['final_type']],after=[row['final_kind'],row['final_type']],reason=reason))
                if (row['word_id'],row['subword_id'])!=(previous['word_id'],previous['subword_id']):
                    owners.append(dict(page=pid,id=row['id'],before=[previous['word_id'],previous['subword_id']],after=[row['word_id'],row['subword_id']],reason=reason))
                if row['in_scope']!=previous['in_scope']:scope.append(dict(page=pid,id=row['id'],before=previous['in_scope'],after=row['in_scope'],reason=reason))
            for key,row in old_index.items():
                if key not in new_index:old_regions.append(dict(page=pid,id=row['id'],area=row['area'],role=row['final_kind'],type=row['final_type'],humanReviewed=row['human_reviewed']))
            page=dict(page=pid,split=newpage['split'],sourceInkExactlyConserved=u0==u1,inkBefore=oldpage['ink_pixels'],inkAfter=newpage['ink_pixels'],sourceKindBefore=oldpage['source_kind'],sourceKindAfter=newpage['source_kind'],annotationBefore=oldpage['annotation_revision'],annotationAfter=newpage['annotation_revision'],humanBefore=len(reviewed_before),humanAfter=sum(r['human_reviewed'] for r in new),sourceVerified=True)
            if source_changed:
                p0=np.frombuffer(u0,dtype='u1');p1=np.frombuffer(u1,dtype='u1');page['sourcePixelsAdded']=int(np.count_nonzero(p1&~p0));page['sourcePixelsRemoved']=int(np.count_nonzero(p0&~p1))
            pages.append(page);counts.update(pages=1,sourceVerifiedPages=1,sourceIdenticalPages=not source_changed,annotationRevisionsUpdated=page['annotationBefore']!=page['annotationAfter'])
            if counts['pages']%25==0:print('Validated source and labels:',counts['pages'],'/ 604',flush=True)
        for name in ('words','subwords'):
            counts[name+'Before']=a.execute(f'SELECT COUNT(*) FROM {name} WHERE in_scope=1').fetchone()[0]
            counts[name+'After']=b.execute(f'SELECT COUNT(*) FROM {name} WHERE in_scope=1').fetchone()[0]
        counts['itemsBefore']=old_n;counts['itemsAfter']=new_n
        counts['reviewedBefore']=a.execute('SELECT COUNT(*) FROM items WHERE human_reviewed=1').fetchone()[0];counts['reviewedAfter']=b.execute('SELECT COUNT(*) FROM items WHERE human_reviewed=1').fetchone()[0]
    f0=np.memmap(before/'features.u8',dtype='u1',mode='r',shape=(old_n,FEATURE_DIM));f1=np.memmap(stage/'features.u8',dtype='u1',mode='r',shape=(new_n,FEATURE_DIM))
    features=[]
    for start in range(0,len(feature_pairs),4096):
        block=feature_pairs[start:start+4096];changed=np.any(f0[[r[2] for r in block]]!=f1[[r[3] for r in block]],axis=1)
        features.extend(dict(page=r[0],id=r[1],sourceChanged=r[4]) for r,change in zip(block,changed) if change)
    summary=dict(counts,modelWeightsUnchanged=True,activeModelHashes=active_models,teacherBankUnchanged=digest_file(stage/'proposals/teacher-bank.json')==digest_file(before/'proposals/teacher-bank.json'),lostHumanLabels=len(lost_human),reviewedSourceMaskTransitions=len(review_transitions),newHumanLabels=len(new_human),unchangedMaskLabelChanges=len(labels),unchangedMaskLabelChangesByReason=dict(Counter(r['reason'] for r in labels)),ownershipChanges=len(owners),scopeChanges=len(scope),featureChangesOnIdenticalMasks=len(features),newRegions=len(new_regions),retiredRegions=len(old_regions),policy='All fresh source masks checked pixel-for-pixel. Human input snapshots are authoritative and untouched. Baseline-relative differences are reported, not presumed correct. Transcript inventory checks are a separate proxy, not human-verified accuracy.')
    document=dict(summary=summary,pages=pages,labelChanges=labels,ownershipChanges=owners,scopeChanges=scope,featureChanges=features,lostHumanLabels=lost_human,newHumanLabels=new_human,reviewedSourceMaskTransitions=review_transitions,newRegions=new_regions,retiredRegions=old_regions)
    for name,value in [('summary.json',summary),('details.json',document)]: (output/name).write_text(json.dumps(value,ensure_ascii=False,indent=2))
    print(json.dumps(summary,ensure_ascii=False,indent=2),flush=True)
    return document


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('stage',type=Path);parser.add_argument('--baseline',type=Path)
    args=parser.parse_args();compare(args.stage,args.baseline)
