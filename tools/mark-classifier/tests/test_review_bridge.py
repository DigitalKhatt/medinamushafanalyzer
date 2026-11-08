import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import segment
from review_bridge import reconcile,apply_review,export_subwords

class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); root=Path(self.temp.name); (root/'source').mkdir()
        (root/'source/page003.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 4"/>')
        config={'name':'test','source_dir':str(root/'source')};segment.prepare(config,root/'data')
        self.store=segment.Store(config,root/'data');self.page=self.store.pages['page003']
        def part(id,runs):
            area,bbox=segment.describe_runs(runs,8)
            return {'id':id,'runs':runs,'area':area,'bbox':bbox,'role':'unknown','owner':None,'line':None,'reviewed':False}
        self.doc={'schemaVersion':1,'characterMetadataVersion':1,'pageId':'page003','sourceSha256':self.page['sha256'],'revision':0,
            'raster':{'width':8,'height':4,'scale':1,'viewBox':[0.,0.,8.,4.],'threshold':170,'chroma':45},
            'parts':[part('body',[8,4,16,4]),part('dot-a',[0,1]),part('dot-b',[2,1]),part('orphan',[31,1])],
            'groups':{},'nextGroup':1,'lines':[],'inkPixels':11}
        self.proposal={'schemaVersion':1,'pageId':'page003','sourceSha256':self.page['sha256'],'raster':self.doc['raster'],
            'paths':[{'id':'p-body','runs':[8,4,16,4]},{'id':'p-a','runs':[0,1]},{'id':'p-b','runs':[2,1]}],
            'candidates':[{'id':'body','kind':'subword-ink','markType':'unknown','pathIds':['p-body'],'subwordId':'s1','wordId':'w1','line':1,'issues':[]},
                {'id':'dots','kind':'dot','markType':'double-dot','pathIds':['p-a','p-b'],'subwordId':'s1','wordId':'w1','line':1,'issues':[]}],
            'subwords':[{'id':'s1','text':'تَ','line':1,'wordId':'w1','wordText':'تَ','candidateIds':['body','dots'],'bodyCandidateIds':['body']}], 'words':[]}
    def tearDown(self):self.temp.cleanup()
    def apply(self,action='approve',id='dots',**extra):
        aligned=reconcile(self.doc,self.proposal)
        return apply_review(self.doc,aligned,{'action':action,'id':id,'proposalSha256':aligned['proposalSha256'],**extra},segment.describe_runs)
    def test_hash_and_transform_mismatch(self):
        for field,value in [('sourceSha256','other'),('raster',{})]:
            p=copy.deepcopy(self.proposal);p[field]=value
            with self.assertRaises(ValueError):reconcile(self.doc,p)
    def test_internal_jeem_dot_accepts_raster_edge_difference_only_with_unique_overlap(self):
        self.doc['parts'][1].update(runs=[0,2],area=2,bbox=[0,0,2,1])
        self.proposal['paths'][1].update(bbox=[0,0,4,3],contourCount=2)
        candidate=self.proposal['candidates'][1]
        candidate.update(kind='symbol',markType='waqf-jeem')
        result=reconcile(self.doc,self.proposal)
        self.assertIn('dot-a',result['candidates'][1]['regionIds'])
        candidate['markType']='double-dot'
        self.assertNotIn('dot-a',reconcile(self.doc,self.proposal)['candidates'][1]['regionIds'])
        candidate['markType']='waqf-jeem'
        self.proposal['candidates'].append({**candidate,'id':'competing'})
        self.assertTrue(any(c['id']=='source:dot-a' for c in reconcile(self.doc,self.proposal)['candidates']))
    def test_unmapped_source_ink_is_retained_in_queue(self):
        r=reconcile(self.doc,self.proposal)
        self.assertEqual(r['alignment']['unresolvedRegions'],1)
        self.assertEqual(r['candidates'][-1]['regionIds'],['orphan'])
        self.assertFalse(r['candidates'][-1]['protected'])
        self.assertEqual(r['sourceDiagnostics']['orphan'],{'reason':'legacy-omission','matches':[]})
    def test_frame_scope_hides_outside_ink_without_changing_review_flags(self):
        from automatic.scope import scoped_review
        original=copy.deepcopy(self.doc)
        r=scoped_review(reconcile(self.doc,self.proposal),self.doc,{'runs':[8,4,16,4]})
        self.assertTrue(next(c for c in r['candidates'] if c['id']=='body')['inScope'])
        self.assertFalse(next(c for c in r['candidates'] if c['id']=='dots')['inScope'])
        with self.assertRaisesRegex(ValueError,'outside the frame'):
            apply_review(self.doc,r,{'action':'approve','id':'dots','proposalSha256':r['proposalSha256']},segment.describe_runs)
        self.assertEqual(self.doc,original)
    def test_scoped_export_ignores_outside_ink_and_keeps_original_annotation(self):
        from test_store import decode_png
        self.doc=self.apply()
        original=copy.deepcopy(self.doc)
        scope={'runs':[8,4,16,4],'width':8,'height':4,'sourceSha256':self.page['sha256'],'scopeSha256':'scopehash','framePathId':'frame','version':'test'}
        dest=Path(self.temp.name)/'scoped-export';segment.export_page(self.doc,dest,self.page,scope)
        semantic=decode_png(dest/'semantic.png');self.assertEqual(semantic[0],255);self.assertEqual(semantic[9],255);self.assertEqual(semantic[12],255)
        marks=json.loads((dest/'marks.json').read_text());self.assertTrue(all(c['target'] is None for c in marks['candidates']))
        self.assertEqual(json.loads((dest/'annotation.json').read_text()),original);self.assertEqual(self.doc,original)
    def test_unresolved_diagnostics_distinguish_competition_and_partial_overlap(self):
        self.proposal['candidates'].append({**self.proposal['candidates'][0],'id':'competing-body','issues':[]})
        result=reconcile(self.doc,self.proposal)
        self.assertEqual(result['sourceDiagnostics']['body']['reason'],'ambiguous-geometry')
        self.assertEqual(len(result['sourceDiagnostics']['body']['matches']),2)
        self.proposal['candidates'].pop()
        self.proposal['paths'][0]['runs']=[8,4]
        result=reconcile(self.doc,self.proposal)
        self.assertEqual(result['sourceDiagnostics']['body']['reason'],'incomplete-geometry')
        self.assertEqual(result['sourceDiagnostics']['body']['matches'][0]['regionCoverage'],.5)
    def test_approve_multipart_dot_conserves_ink(self):
        updated=self.apply()
        self.assertEqual(self.doc['parts'][1]['role'],'unknown')
        self.assertEqual(len(updated['parts']),3)
        dot=next(p for p in updated['parts'] if p['role']=='dot')
        self.assertEqual(dot['runs'],[0,1,2,1]);self.assertTrue(dot['reviewed']);self.assertIsNone(dot['owner'])
        self.assertEqual(segment.validate_annotation(updated,self.page),segment.validate_annotation(self.doc,self.page))
    def test_attested_review_keeps_reviewer_separate_from_recorder(self):
        attribution={'reviewedBy':'user','recordedBy':'assistant','basis':'explicit-user-attestation',
            'statement':'I checked these dots.','recordedAt':'2026-11-02','batchId':'test-review'}
        self.doc=self.apply(reviewAttribution=attribution)
        self.assertEqual(self.doc['parts'][-1]['reviewAttribution'],attribution)
        self.assertEqual(self.doc['proposalReview']['dots']['reviewAttribution'],attribution)
        reopened=self.apply('reopen')
        self.assertNotIn('reviewAttribution',reopened['parts'][-1])
    def test_fixed_review_manifest_syncs_only_exact_masks_and_preserves_teacher(self):
        import sqlite3,zlib
        from automatic.common import pack_runs
        from automatic.reviews import make_manifest,record
        from review_bridge import proposal_digest
        self.store.save('page003',self.doc)
        root=self.store.data_dir/'automatic';root.mkdir(exist_ok=True)
        segment.atomic_json(root/'proposals/page003.json',self.proposal)
        db=sqlite3.connect(root/'corpus.sqlite')
        db.executescript((Path(__file__).resolve().parents[1]/'automatic/schema.sql').read_text())
        db.execute('INSERT INTO pages(page_id,split,source_sha,proposal_sha,annotation_revision) VALUES (?,?,?,?,1)',
            ('page003','train',self.page['sha256'],proposal_digest(self.proposal)))
        db.execute('''INSERT INTO items(id,page_id,runs,kind,mark_type,teacher_kind,teacher_type,origin,human_reviewed,quality)
            VALUES ('page003:dots','page003',?,'dot','double-dot','dot','double-dot','machine-weak',0,.3)''',(pack_runs([0,1,2,1]),))
        db.commit()
        (root/'pages').mkdir()
        path=root/'pages/page003.json.zlib'
        path.write_bytes(zlib.compress(json.dumps({'page':{},'items':[{'id':'page003:dots','candidate_id':'dots','runs':[0,1,2,1],
            'origin':'machine-weak','quality':.3,'teacher_type':'double-dot'}]}).encode()))
        manifest=make_manifest(root,['page003:dots'],'I checked these dots.','test-batch')
        corrupted=copy.deepcopy(manifest);corrupted['items'][0]['maskSha256']='wrong'
        with self.assertRaisesRegex(ValueError,'mask changed'):record(self.store,root,corrupted)
        self.assertEqual(self.store.annotation('page003')['revision'],1)
        self.assertEqual(record(self.store,root,manifest)['reviewed'],1)
        self.assertEqual(db.execute('SELECT origin,proposal_origin,quality,teacher_type,final_type,human_reviewed FROM items').fetchone(),
            ('human-reviewed','machine-weak',.3,'double-dot','double-dot',1))
        self.assertEqual(self.store.proposals('page003')['candidates'][1]['review']['decision'],'approved')
        record(self.store,root,manifest) # Idempotent after a successful or interrupted run.
        self.assertEqual(self.store.annotation('page003')['revision'],2)
        self.assertEqual(json.loads(zlib.decompress(path.read_bytes()))['items'][0]['quality'],.3)
        db.close()
    def test_reject_does_not_create_training_target(self):
        updated=self.apply('reject')
        self.assertEqual(updated['parts'],self.doc['parts'])
        self.assertEqual(updated['proposalReview']['dots']['decision'],'rejected')
    def test_human_labels_and_drafts_are_protected(self):
        for reviewed,role in [(True,'dot'),(False,'mark')]:
            self.doc['parts'][1].update(role=role,reviewed=reviewed)
            with self.assertRaisesRegex(ValueError,'protected'):self.apply()
    def test_subword_approval_does_not_invent_letter_roles(self):
        updated=self.apply('approve-subword','s1',text='تَ',bodyCandidateIds=['body'])
        segment.validate_annotation(updated,self.page)
        self.assertTrue(updated['subwords']['s1']['reviewed'])
        self.assertTrue(updated['parts'][0]['subwordBody'])
        self.assertIsNone(updated['parts'][1].get('subwordId'))
        self.assertTrue(all(p['role']=='unknown' and not p['reviewed'] for p in updated['parts']))
        records,mask=export_subwords(updated,segment.png_bytes)
        self.assertEqual(records['instances'][0]['bodyRegionIds'],['body'])
        dest=self.store.data_dir/'dataset';segment.export_page(updated,dest,self.page)
        from test_store import decode_png
        semantic=decode_png(dest/'semantic.png');subwords=decode_png(dest/'subword-instances.png')
        self.assertEqual(semantic[8],255);self.assertEqual(subwords[16:18],b'\0\1')
        self.assertEqual(subwords[:2],b'\xff\xff');self.assertEqual(subwords[2:4],b'\0\0')
    def test_attachment_requires_an_explicit_review(self):
        plain=self.apply()
        self.assertFalse(plain['parts'][-1].get('subwordAttachmentReviewed',False))
        self.doc=self.apply(attachSubword=True)
        dot=self.doc['parts'][-1]
        self.assertEqual(dot['subwordId'],'s1');self.assertTrue(dot['subwordAttachmentReviewed'])
        self.assertFalse(self.doc['subwords']['s1']['reviewed'])
        dest=self.store.data_dir/'dataset';segment.export_page(self.doc,dest,self.page)
        record=next(c for c in json.loads((dest/'marks.json').read_text())['candidates'] if c['target']==1)
        self.assertEqual(record['subwordOwner'],'s1');self.assertFalse(record['subwordBodyReviewed'])
        reopened=self.apply('reopen')
        self.assertFalse(reopened['parts'][-1]['subwordAttachmentReviewed'])
    def test_reopen_does_not_delete_source_or_character_annotations(self):
        self.doc=self.apply();updated=self.apply('reopen')
        self.assertEqual(segment.validate_annotation(updated,self.page),segment.validate_annotation(self.doc,self.page))
        self.assertFalse(any(p['reviewed'] for p in updated['parts']))
    def test_reopen_refuses_to_erase_workbench_edits(self):
        self.doc=self.apply();self.doc['parts'][-1]['markType']='single-dot'
        with self.assertRaisesRegex(ValueError,'changed in the workbench'):self.apply('reopen')
        self.assertEqual(reconcile(self.doc,self.proposal)['candidates'][1]['review']['decision'],'changed')
    def test_ambiguous_geometry_cannot_be_approved(self):
        self.proposal['candidates'][0]['pathIds']=['p-a']
        with self.assertRaisesRegex(ValueError,'geometry'):self.apply()
    def test_review_requires_current_revision_and_proposal_digest(self):
        self.store.save('page003',self.doc)
        segment.atomic_json(self.store.data_dir/'proposals/page003.json',self.proposal)
        with self.assertRaises(segment.Conflict):self.store.review('page003',{'revision':0})
        r=self.store.proposals('page003')
        with self.assertRaisesRegex(ValueError,'changed'):self.store.review('page003',{'revision':1,'action':'approve','id':'dots','proposalSha256':'stale'})
        response=self.store.review('page003',{'revision':1,'action':'approve','id':'dots','proposalSha256':r['proposalSha256']})
        self.assertEqual(response['revision'],2)
        saved=self.store.annotation('page003');self.assertTrue(any(p['role']=='dot' and p['reviewed'] for p in saved['parts']))
        self.assertTrue((self.store.data_dir/'history/page003/revision-000002.json').exists())
    def test_reload_prefers_automatic_without_mutating_original_annotations_or_proposals(self):
        self.store.save('page003',self.doc)
        legacy=self.store.data_dir/'proposals/page003.json';segment.atomic_json(legacy,self.proposal)
        automatic={**copy.deepcopy(self.proposal),'method':'updated automatic teacher'}
        automatic['candidates'][1]['markType']='triple-dot'
        segment.atomic_json(self.store.data_dir/'automatic/proposals/page003.json',automatic)
        before=(self.store.data_dir/'annotations/page003.json').read_bytes();original=legacy.read_bytes()
        latest=self.store.proposals('page003');old=self.store.proposals('page003','legacy')
        self.assertEqual(latest['proposalSource'],'automatic');self.assertEqual(latest['candidates'][1]['markType'],'triple-dot')
        self.assertEqual(old['candidates'][1]['markType'],'double-dot')
        self.assertEqual((self.store.data_dir/'annotations/page003.json').read_bytes(),before);self.assertEqual(legacy.read_bytes(),original)
        # A form loaded from another snapshot cannot approve the new masks.
        with self.assertRaisesRegex(ValueError,'Proposals changed'):
            self.store.review('page003',{'revision':1,'action':'approve','id':'dots','proposalSha256':old['proposalSha256']})
    def test_automatic_reviews_are_separate_and_do_not_overwrite_legacy_reviews(self):
        legacy_doc=self.apply('reject');self.store.save('page003',legacy_doc)
        segment.atomic_json(self.store.data_dir/'proposals/page003.json',self.proposal)
        automatic={**copy.deepcopy(self.proposal),'method':'updated automatic teacher'}
        segment.atomic_json(self.store.data_dir/'automatic/proposals/page003.json',automatic)
        latest=self.store.proposals('page003')
        self.assertIsNone(latest['candidates'][1]['review'])
        self.store.review('page003',{'revision':1,'action':'approve','id':'dots','proposalSource':'automatic','proposalSha256':latest['proposalSha256']})
        saved=self.store.annotation('page003');self.assertEqual(saved['proposalReview']['dots']['decision'],'rejected')
        self.assertEqual(saved['proposalReviewSets'][latest['proposalReviewSetKey']]['dots']['decision'],'approved')
        self.assertEqual(self.store.proposals('page003')['candidates'][1]['review']['decision'],'approved')
        self.assertEqual(self.store.proposals('page003','legacy')['candidates'][1]['review']['decision'],'rejected')
    def test_new_automatic_snapshot_does_not_reuse_review_for_the_same_candidate_id(self):
        self.store.save('page003',self.doc)
        first={**copy.deepcopy(self.proposal),'method':'automatic v1'}
        segment.atomic_json(self.store.data_dir/'automatic/proposals/page003.json',first)
        loaded=self.store.proposals('page003')
        self.store.review('page003',{'revision':1,'action':'reject','id':'dots','proposalSha256':loaded['proposalSha256']})
        second={**first,'method':'automatic v2'};segment.atomic_json(self.store.data_dir/'automatic/proposals/page003.json',second)
        self.assertIsNone(self.store.proposals('page003')['candidates'][1]['review'])
        self.assertEqual(self.store.annotation('page003')['proposalReviewSets'][loaded['proposalReviewSetKey']]['dots']['decision'],'rejected')
    def test_subword_cannot_swallow_approved_mark(self):
        self.doc=self.apply()
        with self.assertRaisesRegex(ValueError,'approved mark'):self.apply('approve-subword','s1',bodyCandidateIds=['body','dots'])
    def test_workbench_edits_reopen_synthetic_candidate_status(self):
        self.doc=self.apply(id='source:orphan',role='mark',markType='fatha')
        part=next(p for p in self.doc['parts'] if p['id']=='orphan');part['reviewed']=False
        candidate=next(c for c in reconcile(self.doc,self.proposal)['candidates'] if c['id']=='source:orphan')
        self.assertEqual(candidate['review']['decision'],'changed')
    def test_mark_correction_reopens_affected_body_boundary(self):
        self.doc['subwords']={'s1':{'text':'ت','reviewed':True,'decision':'approved'}}
        for p in self.doc['parts'][:3]:p.update(subwordId='s1',subwordBody=p['id'] in ('body','dot-b'))
        updated=self.apply()
        self.assertFalse(updated['subwords']['s1']['reviewed'])
        self.assertFalse(updated['parts'][-1]['subwordBody'])
        segment.validate_annotation(updated,self.page)

if __name__=='__main__':unittest.main()
