"""Data boundaries between offline labels, training and model-generated corpus."""
from contextlib import closing
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from automatic.labels import prepare,load
from automatic.inference import finalize
from automatic.common import FEATURE_DIM,pack_runs


class LabelPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.executescript((Path(__file__).resolve().parents[1]/'automatic/schema.sql').read_text())
            db.execute("INSERT INTO pages(page_id,split,source_sha,width,height,ink_pixels,scope_ink_pixels) VALUES ('page003','train','sha',10,8,16,12)")
            for i,(origin,human,scope) in enumerate([('machine-weak',0,1),('human-reviewed',1,1),('human-draft',0,1),('machine-weak',0,0)]):
                db.execute('''INSERT INTO items(id,page_id,feature_row,x,y,w,h,area,contours,runs,in_scope,kind,mark_type,
                    teacher_kind,teacher_type,quality,origin,human_reviewed,prediction_kind,prediction_type)
                    VALUES (?,'page003',?,1,1,2,2,4,1,?,?,'subword-ink','unknown','subword-ink','unknown',.99,?,?,'dot','triple-dot')''',
                    (f'c{i}',i,pack_runs([11,2,21,2]),scope,origin,human))
            db.commit()
        (self.root/'features.u8').write_bytes(bytes(4*FEATURE_DIM))
    def tearDown(self):self.temp.cleanup()
    def test_rule_teacher_supplies_automatic_target_without_reviewing_source_ink(self):
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE pages SET width=1000,height=1000")
            db.execute("INSERT INTO words(id,page_id,x,y,w,h,in_scope) VALUES ('w','page003',100,300,700,100,1)")
            db.execute("UPDATE items SET kind='unknown',teacher_kind='unknown',origin='unresolved',x=200,y=400,w=120,h=3,area=360,runs=? WHERE id='c0'",
                (pack_runs([400200,120,401200,120,402200,120]),));db.commit()
        manifest,rows=prepare(self.root),load(self.root)[1]
        self.assertEqual((rows[0]['role'],rows[0]['type'],rows[0]['provenance'],rows[0]['human_reviewed']),('symbol','sajda-bar','automatic',0))
        self.assertEqual(manifest['automaticCorrections']['actions']['supply'],1)
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            self.assertEqual(db.execute("SELECT teacher_kind,human_reviewed FROM items WHERE id='c0'").fetchone(),('unknown',0))
    def test_regrouped_source_component_resolves_to_complete_mark(self):
        import zlib
        from automatic.query import Corpus
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET id='page003:c0',issues='[]' WHERE id='c0'");db.commit()
        pages=self.root/'pages';pages.mkdir()
        (pages/'page003.json.zlib').write_bytes(zlib.compress(json.dumps({'items':[
            {'id':'page003:c0','region_ids':['part-00001','part-00002']}]}).encode()))
        detail=Corpus(self.root).detail('page003:source:part-00002')
        self.assertEqual(detail['id'],'page003:c0')
        self.assertEqual(detail['requestedComponentId'],'page003:source:part-00002')
    def test_frozen_targets_ignore_drafts_and_exterior_and_keep_provenance(self):
        prepare(self.root);manifest,rows=load(self.root)
        self.assertEqual([r['role'] for r in rows],['subword-ink','subword-ink',None,None])
        self.assertEqual([r['human_reviewed'] for r in rows],[0,1,0,0])
        self.assertEqual([r['provenance'] for r in rows],['automatic','human-reviewed','unlabelled','automatic'])
        self.assertEqual(rows[0]['quality'],.99)
        self.assertEqual(rows[1]['quality'],1.)
        self.assertEqual(manifest['items'],4)
        # A live teacher change cannot mutate the targets already prepared.
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:db.execute("UPDATE items SET teacher_kind='mark' WHERE id='c0'");db.commit()
        self.assertEqual(load(self.root)[1][0]['role'],'subword-ink')
    def test_changed_source_split_or_feature_bytes_require_new_dataset(self):
        prepare(self.root)
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:db.execute("UPDATE pages SET split='test'");db.commit()
        with self.assertRaisesRegex(ValueError,'splits'):load(self.root)
        prepare(self.root)
        (self.root/'features.u8').write_bytes(bytes([1])+bytes(4*FEATURE_DIM-1))
        with self.assertRaisesRegex(ValueError,'features'):load(self.root)
    def test_only_reviewed_human_overrides_model_not_automatic_or_draft(self):
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET prediction_guard='Suspicious shape' WHERE id='c0'")
            report=finalize(db)
            labels=list(db.execute('SELECT final_kind,final_type,final_source FROM items ORDER BY feature_row'))
            self.assertEqual(labels[0],('dot','triple-dot','model'))
            self.assertEqual(labels[1],('subword-ink','unknown','human-reviewed'))
            self.assertEqual(labels[2],('dot','triple-dot','model'))
            self.assertEqual(report['structuralPolicy'],'diagnostic-only')
            self.assertEqual(report['structuralFlags'],1)
    def test_model_evidence_filters_use_scores_and_flags_not_teacher_agreement(self):
        from automatic.query import Corpus
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute("UPDATE items SET model_confidence=.99,model_agrees=0")
            db.execute("UPDATE items SET model_confidence=.1 WHERE id='c1'")
            db.execute("UPDATE items SET prediction_guard='Suspicious shape' WHERE id='c0'")
            finalize(db);db.commit()
        corpus=Corpus(self.root)
        result=corpus.search({'kind':'all','quality':'supported'})
        self.assertEqual({i['id'] for i in result['items']},{'c1','c2'})
        self.assertEqual({i['id'] for i in corpus.search({'kind':'all','quality':'supported','source':'model'})['items']},{'c2'})
        self.assertEqual(corpus.search({'kind':'all','quality':'flagged'})['items'][0]['id'],'c0')
        # Optional performance indexes must never change filtering semantics.
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            db.execute('DROP INDEX item_final_supported_browse');db.execute('DROP INDEX item_model_supported_browse');db.commit()
        self.assertEqual({i['id'] for i in corpus.search({'kind':'all','quality':'supported'})['items']},{'c1','c2'})
        self.assertEqual({i['id'] for i in corpus.search({'kind':'all','quality':'supported','source':'model'})['items']},{'c2'})
    @unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('cv2'),'Optional model runtime')
    def test_saved_model_inference_needs_no_training_labels_and_checks_are_diagnostic(self):
        import numpy as np
        from automatic.learn import Network
        from automatic.labels import digest_file
        from automatic.inference import infer_corpus
        folder=self.root/'models';folder.mkdir()
        net=Network(FEATURE_DIM,8,2)
        for v in net.p.values():v[:]=0
        net.p['br'][1]=10;net.p['bt'][0]=10
        np.savez_compressed(folder/'selected.npz',**net.p,mean=np.zeros(FEATURE_DIM),std=np.ones(FEATURE_DIM))
        (folder/'model.json').write_text(json.dumps({'types':['triple-dot','fatha'],'typeRoles':[1,2],
            'selectedRound':1,'modelSha256':digest_file(folder/'selected.npz')}))
        for filtered in (False,True,False):
            report=infer_corpus(self.root,lambda *args:None,filtered)
            with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
                row=db.execute("SELECT final_type,raw_prediction_type,prediction_guard FROM items WHERE id='c0'").fetchone()
                human=db.execute("SELECT final_kind FROM items WHERE id='c1'").fetchone()[0]
            self.assertEqual(row[0],'unknown' if filtered else 'triple-dot')
            self.assertEqual(row[1],'triple-dot');self.assertTrue(row[2])
            self.assertEqual(human,'subword-ink')
            self.assertEqual(report['structuralPolicy'],'filter' if filtered else 'diagnostic-only')
            self.assertEqual(report['decoder']['version'],'joint-role-type-product-v1')
            self.assertEqual(report['networkOutputs']['types'],['triple-dot','fatha'])
        # Rechecking later must retain the diagnostic-only policy as well.
        from automatic.structure import recheck
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            published=json.loads(db.execute("SELECT value FROM meta WHERE key='learning'").fetchone()[0])
            self.assertEqual(published['types'],['triple-dot','fatha'])
            self.assertEqual(published['modelSha256'],report['modelSha256'])
        recheck(self.root,lambda *args:None)
        with closing(sqlite3.connect(self.root/'corpus.sqlite')) as db:
            self.assertEqual(db.execute("SELECT prediction_type,final_type FROM items WHERE id='c0'").fetchone(),('triple-dot','triple-dot'))
            self.assertEqual(json.loads(db.execute("SELECT value FROM meta WHERE key='inference'").fetchone()[0])['decoder']['version'],'joint-role-type-product-v1')


if __name__=='__main__':unittest.main()
