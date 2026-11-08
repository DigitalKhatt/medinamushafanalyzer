import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from automatic.common import pack_runs
from automatic.resolve import classify_horizontal_rule,recognize_unmapped_body,resolve_corpus


class ResolutionTests(unittest.TestCase):
    def test_detached_rules_use_text_band_and_reject_letter_shapes(self):
        page={'width':2041,'height':2918}
        bar={'x':800,'y':1000,'w':230,'h':4,'area':920}
        self.assertEqual(classify_horizontal_rule(bar,page,(400,1800)),'sajda-bar')
        self.assertEqual(classify_horizontal_rule({**bar,'x':170,'w':112,'area':448},page,(400,1800)),'margin-rule')
        for change in ({'h':24},{'w':40},{'area':300},{'y':10},{'w':1000}):
            self.assertIsNone(classify_horizontal_rule({**bar,**change},page,(400,1800)))

    def test_body_recognition_does_not_require_text_but_rejects_mixed_marks(self):
        body={'prediction_kind':'subword-ink','model_confidence':.999,'model_margin':.99,'w':120,'h':80,'area':1500,'subword_id':None}
        self.assertTrue(recognize_unmapped_body(body,{'matches':[]}))
        self.assertFalse(recognize_unmapped_body(body,{'matches':[{'kind':'mark','regionCoverage':.3}]}))
        self.assertFalse(recognize_unmapped_body({**body,'model_confidence':.7},{'matches':[]}))
        self.assertFalse(recognize_unmapped_body({**body,'h':4},{'matches':[]}))

    def test_resolution_is_repeatable_and_preserves_masks_teacher_and_human_ink(self):
        # Exercise the publication step with diagnostics fixed, independently of
        # rendering. Body text remains absent and human drafts stay unknown.
        class InlinePool:
            def __init__(self,**kwargs):pass
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def map(self,fn,tasks):return map(fn,tasks)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);db=sqlite3.connect(root/'corpus.sqlite')
            db.executescript((Path(__file__).resolve().parents[1]/'automatic/schema.sql').read_text())
            db.execute("INSERT INTO pages(page_id,split,source_sha,width,height,ink_pixels) VALUES ('page003','train','hash',2041,2918,3000)")
            for id,origin in [('body','unresolved'),('draft','human-draft')]:
                db.execute('''INSERT INTO items(id,page_id,x,y,w,h,area,runs,origin,human_reviewed,teacher_kind,teacher_type,prediction_kind,model_confidence,model_margin,final_kind,final_type)
                  VALUES (?, 'page003',800,1000,120,80,1500,?,?,0,'unknown','unknown','subword-ink',.999,.99,'unknown','unknown')''',(id,pack_runs([2041000,1500]),origin))
            db.commit()
            initial=db.execute('SELECT id,runs,teacher_kind,teacher_type,human_reviewed FROM items ORDER BY id').fetchall()
            diagnostics=('page003',{'body':{'reason':'legacy-omission','matches':[]},'draft':{'reason':'human-draft','matches':[]}})
            with patch('automatic.resolve.ProcessPoolExecutor',InlinePool),patch('automatic.resolve._diagnose',return_value=diagnostics):
                a=resolve_corpus(root,1,lambda *args:None)
                b=resolve_corpus(root,1,lambda *args:None)
            self.assertEqual(a,b);self.assertEqual(a['recovered'],{'body-ink':1})
            self.assertEqual(db.execute('SELECT id,runs,teacher_kind,teacher_type,human_reviewed FROM items ORDER BY id').fetchall(),initial)
            self.assertEqual(db.execute("SELECT final_kind,subword_id,word_id,assignment_status FROM items WHERE id='body'").fetchone(),('subword-ink',None,None,'text-unassigned'))
            self.assertEqual(db.execute("SELECT final_kind FROM items WHERE id='draft'").fetchone()[0],'unknown')
            self.assertEqual(db.execute('SELECT unknown_pixels FROM pages').fetchone()[0],1500)
            db.close()


if __name__=='__main__':unittest.main()
