import importlib.util
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import os
import subprocess
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from automatic.common import plain,pack_runs,unpack_runs,mask_path
from automatic.query import Corpus

class CorpusTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        db=sqlite3.connect(self.root/'corpus.sqlite');db.executescript((Path(__file__).resolve().parents[1]/'automatic/schema.sql').read_text())
        db.execute('INSERT INTO meta VALUES (?,?)',('build',json.dumps({'items':2,'allSourceInkConserved':True})))
        for page,split in [('page003','train'),('page037','validation')]:
            db.execute('INSERT INTO pages(page_id,split,source_sha,width,height,ink_pixels,matched_pixels,unknown_pixels,component_count,source_kind) VALUES (?,?,?,?,?,?,?,?,?,?)',(page,split,'hash',10,8,4,4,0,1,'paired-original-pdf'))
            wid=page+':w1';sid=page+':s1';id=page+':c1'
            db.execute('INSERT INTO words(id,page_id,line,ordinal,text,bases,x,y,w,h,flags) VALUES (?,?,?,?,?,?,?,?,?,?,?)',(wid,page,1,1,'ٱلْحَمْدُ',plain('ٱلْحَمْدُ'),0,0,8,8,1))
            db.execute('INSERT INTO subwords(id,page_id,word_id,line,text,bases,x,y,w,h,expected,issues) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)',(sid,page,wid,1,'حَمْدُ',plain('حَمْدُ'),1,1,4,4,'[]','[]'))
            db.execute('''INSERT INTO items(id,page_id,word_id,subword_id,line,kind,mark_type,text,bases,x,y,w,h,area,components,contours,runs,feature_row,quality,origin,human_reviewed,issues,teacher_kind,teacher_type,prediction_kind,prediction_type,model_confidence,model_margin,model_agrees,final_kind,final_type,refined,review_candidate)
             VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)''',
             (id,page,wid,sid,1,'dot','single-dot','حَمْدُ',plain('حَمْدُ'),1,1,2,2,4,1,1,pack_runs([11,2,21,2]),0 if page=='page003' else 1,.9,'machine-weak',0,'[]','dot','single-dot','dot','double-dot',.95,.8,0,'dot','double-dot',1,'legacy-c1'))
        db.commit();db.close();self.corpus=Corpus(self.root)
    def tearDown(self):self.temp.cleanup()
    def test_arabic_search_normalizes_wasla_and_vowels(self):
        for query in ['الحمد','ٱلْحَمْدُ','حَمْد']:
            result=self.corpus.search({'q':[query]});self.assertEqual(result['total'],2)
    def test_original_predicted_and_constrained_labels_are_separate(self):
        for source,type,count in [('teacher','single-dot',2),('final','double-dot',2),('model','double-dot',2),('teacher','double-dot',0)]:
            self.assertEqual(self.corpus.search({'source':[source],'type':[type]})['total'],count)
    def test_detail_exposes_separate_head_votes_and_selected_pair_scores(self):
        import struct
        from automatic.learn import ROLES
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET raw_prediction_kind='mark',raw_prediction_type='tanwin-damm',role_scores=?,type_scores=? WHERE id='page003:c1'",
                (struct.pack('<7f',0,0,.41,.59,0,0,0),struct.pack('<2f',.95,.05)))
            db.execute('INSERT INTO meta VALUES (?,?)',('inference',json.dumps({'networkOutputs':{
                'roles':ROLES,'types':['tanwin-damm','waqf-meem'],'storage':'float32-le'},'decoder':{'version':'joint-role-type-product-v1'}})))
            db.commit()
        detail=self.corpus.detail('page003:c1');votes=detail['networkOutputs']
        self.assertEqual(votes['roles'][0]['label'],'symbol')
        self.assertEqual(votes['types'][0]['label'],'tanwin-damm')
        self.assertAlmostEqual(votes['selectedRoleScore'],.41,places=6)
        self.assertAlmostEqual(votes['selectedTypeScore'],.95,places=6)
        self.assertNotIn('role_scores',detail);self.assertNotIn('type_scores',detail)
        json.dumps(detail) # No SQLite blobs leak into the dependency-free HTTP API.
    def test_page_split_metric_and_pagination_filters(self):
        result=self.corpus.search({'split':['validation'],'min':['2'],'max':['2'],'limit':['1']})
        self.assertEqual(result['total'],1);self.assertEqual(result['items'][0]['page_id'],'page037')
        self.assertEqual(self.corpus.search({'pageMin':['4']})['total'],1)
    def test_large_pages_default_to_1000_and_preserve_pagination(self):
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.executemany("INSERT INTO items(id,page_id,w,h,runs,final_kind,final_type) VALUES (?,'page003',2,2,?,'dot','double-dot')",[(f'extra:{i:04}',pack_runs([11,2])) for i in range(1003)])
            db.commit()
        first=self.corpus.search({});last=self.corpus.search({'offset':['1000']});large=self.corpus.search({'limit':['10000']})
        self.assertEqual((first['limit'],len(first['items']),last['total'],len(last['items'])),(1000,1000,1005,5))
        self.assertEqual(len(large['items']),1005)
        self.assertFalse({r['id'] for r in first['items']}&{r['id'] for r in last['items']})
    def test_source_counts_and_equal_counts_do_not_hide_different_regions(self):
        m=self.corpus.metadata()
        self.assertEqual(m['typeDifferences']['single-dot'],2)
        self.assertEqual(m['typesBySource']['teacher'][0]['type'],'single-dot')
        self.assertEqual(m['typesBySource']['model'][0]['type'],'double-dot')
        # Equal totals can represent different regions, so counts alone must
        # never be advertised as agreement between label sources.
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET teacher_type='double-dot',prediction_type='single-dot' WHERE page_id='page037'");db.commit()
        m=self.corpus.metadata()
        self.assertEqual(m['typeDifferences']['single-dot'],2)
        self.assertEqual(self.corpus.search({'type':['single-dot'],'source':['teacher']})['items'][0]['page_id'],'page003')
        self.assertEqual(self.corpus.search({'type':['single-dot'],'source':['model']})['items'][0]['page_id'],'page037')
    @unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('cv2'),'Optional structural decoding')
    def test_structural_abstention_preserves_raw_guess_and_teacher_labels(self):
        from automatic.structure import guard_triples
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.row_factory=sqlite3.Row
            db.execute("UPDATE items SET prediction_kind='dot',prediction_type='triple-dot',raw_prediction_kind='dot',raw_prediction_type='triple-dot',raw_model_confidence=.98,teacher_kind='subword-ink',teacher_type='small-kaf-body' WHERE id='page003:c1'")
            report=guard_triples(db);db.commit()
        self.assertEqual(report['withheldTripleDots'],1)
        detail=self.corpus.detail('page003:c1')
        self.assertEqual(detail['prediction_type'],'unknown');self.assertEqual(detail['raw_prediction_type'],'triple-dot')
        self.assertEqual(detail['raw_model_confidence'],.98);self.assertEqual(detail['teacher_type'],'small-kaf-body')
        self.assertEqual(detail['prediction_structure']['components'],1);self.assertTrue(detail['prediction_guard'])
        self.assertEqual(detail['masks'][0]['runs'],[11,2,21,2])
    @unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('cv2'),'Optional structural decoding')
    def test_saved_guesses_can_be_rechecked_without_retraining_or_changing_targets(self):
        from automatic.structure import recheck
        (self.root/'models').mkdir()
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute('INSERT INTO meta VALUES (?,?)',('learning',json.dumps({'types':['triple-dot']})))
            db.execute("UPDATE items SET prediction_kind='dot',prediction_type='triple-dot',raw_prediction_kind='dot',raw_prediction_type='triple-dot',raw_model_confidence=.98,raw_model_margin=.8 WHERE id='page003:c1'");db.commit()
            original=db.execute('SELECT id,runs,teacher_kind,teacher_type,final_kind,final_type FROM items ORDER BY id').fetchall()
        a=recheck(self.root,lambda *args:None);b=recheck(self.root,lambda *args:None)
        self.assertEqual(a['predictionChecks'],b['predictionChecks']);self.assertEqual(a['predictionChecks']['withheldTripleDots'],1)
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            self.assertEqual(db.execute('SELECT id,runs,teacher_kind,teacher_type,final_kind,final_type FROM items ORDER BY id').fetchall(),original)
    def test_thumbnail_uses_selected_source_without_loading_full_detail(self):
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET prediction_kind='mark',final_kind='dot' WHERE id='page003:c1'");db.commit()
        with patch.object(self.corpus,'detail',side_effect=AssertionError('Thumbnail loaded full detail')):
            self.assertIn('#168c79',self.corpus.image('page003:c1',source='model'))
            self.assertIn('#9958b7',self.corpus.image('page003:c1',source='final'))
        self.assertIn('M1 1h2v1h-2z',self.corpus.image('page003:c1'))
    def test_cached_totals_invalidate_after_a_corpus_commit(self):
        self.assertEqual(self.corpus.search({})['total'],2)
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET final_kind='mark' WHERE page_id='page037'");db.commit()
        self.assertEqual(self.corpus.search({})['total'],1)
    def test_frame_exterior_is_excluded_from_every_label_source_and_logical_search(self):
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            for table in ('items','words','subwords'):db.execute(f"UPDATE {table} SET in_scope=0 WHERE page_id='page037'")
            db.commit()
        for source in ('teacher','model','final'):
            self.assertEqual(self.corpus.search({'kind':['all'],'source':[source]})['total'],1)
        self.assertEqual(self.corpus.search({'kind':['subwords']})['total'],1)
        self.assertEqual(self.corpus.metadata()['totals']['regions'],1)
        with self.assertRaises(ValueError):self.corpus.image('page037:c1')
    def test_scope_publication_is_repeatable_and_accounts_for_every_original_pixel(self):
        from automatic.scope import apply_scopes
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            original=db.execute('SELECT id,runs,area,teacher_kind,teacher_type,human_reviewed FROM items ORDER BY id').fetchall()
        prepared=[{'pageId':'page003','framePathId':'frame','interiorContours':1,'insidePixels':4,'outsidePixels':0,'excludedIds':[],'partial':[]},
                  {'pageId':'page037','framePathId':'frame','interiorContours':1,'insidePixels':0,'outsidePixels':4,'excludedIds':['page037:c1'],'partial':[]}]
        a=apply_scopes(self.root,self.root,1,lambda *args:None,prepared)
        b=apply_scopes(self.root,self.root,1,lambda *args:None,prepared)
        self.assertEqual(a,b);self.assertEqual(a['includedInkPixels']+a['excludedInkPixels'],8)
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            self.assertEqual(db.execute('SELECT id,runs,area,teacher_kind,teacher_type,human_reviewed FROM items ORDER BY id').fetchall(),original)
        self.assertEqual(self.corpus.search({'kind':['all']})['total'],1)
    def test_read_only_database_and_parameter_validation(self):
        with self.assertRaises(sqlite3.OperationalError):
            with self.corpus.connect() as db:db.execute('DELETE FROM items')
        for params in [{'kind':['DROP TABLE items']},{'metric':['x); DROP TABLE items']},{'limit':['999999']},{'min':['nan']},{'pageMin':['-1']}]:
            with self.assertRaises(ValueError):self.corpus.search(params)
        self.assertEqual(self.corpus.search({'q':["%' OR 1=1 --"]})['total'],0)
    def test_detail_source_runs_and_review_link_are_consistent(self):
        d=self.corpus.detail('page003:c1');self.assertEqual(d['masks'][0]['runs'],[11,2,21,2])
        self.assertEqual(d['reviewUrl'],'/review.html?proposalSource=legacy&candidate=legacy-c1&suggestRole=dot&suggestType=double-dot#page003')
        self.assertIn('M1 1h2v1h-2z',d['maskPath'])
        self.assertIn('viewBox=',self.corpus.image('page003:c1'))
    def test_review_link_targets_matching_automatic_snapshot_and_rejects_stale_digest(self):
        from review_bridge import proposal_digest
        snapshot={'candidates':[{'id':'c1'}],'subwords':[{'id':'s1'}]}
        folder=self.root/'proposals';folder.mkdir();path=folder/'page003.json'
        path.write_text(json.dumps(snapshot))
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute('UPDATE pages SET proposal_sha=? WHERE page_id=?',(proposal_digest(snapshot),'page003'));db.commit()
        detail=self.corpus.detail('page003:c1')
        self.assertEqual(detail['reviewUrl'],'/review.html?proposalSource=automatic&candidate=c1&suggestRole=dot&suggestType=double-dot#page003')
        snapshot['method']='changed snapshot';path.write_text(json.dumps(snapshot))
        self.assertIn('proposalSource=legacy&candidate=legacy-c1',self.corpus.detail('page003:c1')['reviewUrl'])
    def test_subword_search_and_unresolved_body_are_explicit(self):
        r=self.corpus.search({'kind':['subwords'],'q':['الحمد']});self.assertEqual(r['total'],2)
        d=self.corpus.detail(r['items'][0]['id'],'subword');self.assertEqual(d['masks'],[])
        self.assertIn('Unresolved body',self.corpus.image(d['id'],'subword'))
    def test_rebuild_does_not_offer_stale_embedding_similarity(self):
        with self.assertRaises(ValueError):self.corpus.similar('page003:c1')
    def test_recovered_bodies_keep_text_assignment_separate(self):
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET word_id=NULL,subword_id=NULL,teacher_kind='unknown',teacher_type='unknown',final_kind='subword-ink',final_type='unknown',quality=0,resolution_method='visual-body',assignment_status='text-unassigned',unresolved_reason='legacy-omission',source_diagnostics=?,context_bbox=? WHERE id='page003:c1'",(json.dumps({'reason':'legacy-omission','matches':[]}),json.dumps([0,0,8,8])))
            db.commit()
        result=self.corpus.search({'kind':['unmapped-bodies'],'quality':['supported'],'reason':['legacy-omission']})
        self.assertEqual(result['total'],1);self.assertEqual(result['items'][0]['assignment_status'],'text-unassigned')
        self.assertEqual(self.corpus.search({'kind':['unknown'],'source':['teacher']})['total'],1)
        detail=self.corpus.detail('page003:c1');self.assertIsNone(detail['word']);self.assertIsNone(detail['subword_id'])
        self.assertEqual(detail['source_diagnostics']['reason'],'legacy-omission');self.assertEqual(detail['context_bbox'],[0,0,8,8])
        self.assertEqual(self.corpus.metadata()['unmappedBodies'],1)
    def test_rle_serialization_and_holes(self):
        runs=[11,3,21,1,23,1,31,3];self.assertEqual(unpack_runs(pack_runs(runs)),runs)
        path=mask_path(runs,10);self.assertNotIn('M2 2',path)
    def test_query_normalization_preserves_letter_distinctions(self):
        self.assertEqual(plain('ٱلرَّحْمَٰنِ'),'الرحمن');self.assertEqual(plain('لَهُۥ'),'له')
        self.assertNotEqual(plain('بَة'),plain('بَه'))
    @unittest.skipUnless(os.name=='posix','POSIX writer lock')
    def test_two_pipeline_runs_cannot_overwrite_the_same_artifacts(self):
        import fcntl
        with (self.root/'.pipeline.lock').open('a+b') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            script=Path(__file__).resolve().parents[1]/'automatic_pipeline.py'
            result=subprocess.run([sys.executable,str(script),'run','--allow-weak-labels','--output',str(self.root)],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0);self.assertIn('Another automatic run',result.stderr)

@unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('cv2'),'Optional automatic learning dependencies')
class LearningTests(unittest.TestCase):
    def test_source_components_keep_tiny_marks_and_counters(self):
        import numpy as np
        from automatic.build import components
        mask=np.zeros((15,15),dtype='u1');mask[2:10,2:10]=1;mask[4:8,4:8]=0;mask[13,13]=1
        parts=components(mask);self.assertEqual(len(parts),2)
        rebuilt=np.zeros(mask.size,dtype='u1')
        for p in parts:
            for s,n in zip(p['runs'][::2],p['runs'][1::2]):rebuilt[s:s+n]=1
        np.testing.assert_array_equal(rebuilt.reshape(mask.shape),mask)
    def test_neural_role_and_type_heads_learn_from_source_features(self):
        import numpy as np
        from automatic.learn import Network
        rng=np.random.default_rng(4);x=np.r_[rng.normal(-1,.05,(24,4)),rng.normal(1,.05,(24,4))].astype('f4')
        role=np.r_[np.ones(24,dtype='i4'),np.full(24,2,dtype='i4')];types=np.r_[np.zeros(24,dtype='i4'),np.ones(24,dtype='i4')]
        net=Network(4,16,2,3);loss=[]
        for _ in range(140):loss.append(net.batch(x,role,types,np.ones(48,dtype='f4'),.003))
        rp,tp,_=net.forward(x)
        self.assertGreater((rp.argmax(1)==role).mean(),.95);self.assertGreater((tp.argmax(1)==types).mean(),.95);self.assertLess(loss[-1],loss[0]/5)
    def test_unknown_types_do_not_become_negative_subtype_targets(self):
        import numpy as np
        from automatic.learn import Network,f1_score
        net=Network(4,8,2);net.batch(np.ones((3,4),dtype='f4'),np.ones(3,dtype='i4'),np.full(3,-1,dtype='i4'),np.ones(3,dtype='f4'),.001)
        np.testing.assert_array_equal(net.p['bt'],np.zeros(2))
        metric=f1_score(np.array([0,-1]),np.array([0,1]),2);self.assertEqual(metric['support'],[1,0])
    def test_features_have_fixed_shape_and_no_transcript_input(self):
        import numpy as np
        from automatic.common import feature_vector,FEATURE_DIM
        mask=np.zeros((20,20),dtype='u1');mask[3:8,4:9]=1
        runs=[]
        for y in range(3,8):runs.extend([y*20+4,5])
        a=feature_vector(runs,[4,3,5,5],mask,12)
        self.assertEqual(a.shape,(FEATURE_DIM,));self.assertEqual(a.dtype,np.uint8)

if __name__=='__main__':unittest.main()
