import sqlite3
import unittest

from automatic.common import pack_runs
from automatic.weak_corrections import corrections,pieces,shape_type,holes,double_damma,dammatan_family


class WeakCorrectionTests(unittest.TestCase):
    def test_loop_evidence_requires_two_complete_native_scale_components(self):
        ring={(y,x) for y in range(5) for x in range(5) if y in (0,4) or x in (0,4)}
        self.assertEqual(holes(ring),1)
        self.assertEqual(holes(ring-{(0,2)}),0)
        a={'x':0,'y':0,'w':17,'h':26,'area':136,'holes':1}
        b={**a,'x':14,'w':19,'area':141}
        self.assertTrue(double_damma([a,b]))
        self.assertFalse(double_damma([a,{**b,'holes':0}]))
        self.assertFalse(double_damma([a,{**b,'x':100}]))
        self.assertFalse(double_damma([{**a,'h':70},b]))
        arc={**a,'w':23,'h':24,'area':117,'holes':0}
        loop={**b,'x':0,'y':8,'w':21,'area':162}
        self.assertEqual(dammatan_family([arc,loop]),'arc-and-loop')
        self.assertIsNone(dammatan_family([{**arc,'y':20},loop]))
    def setUp(self):
        self.db=sqlite3.connect(':memory:');self.db.row_factory=sqlite3.Row
        self.db.executescript('''CREATE TABLE pages(page_id TEXT,width INTEGER,source_sha TEXT,split TEXT);
            CREATE TABLE items(id TEXT,page_id TEXT,subword_id TEXT,in_scope INTEGER,origin TEXT,
            human_reviewed INTEGER,teacher_type TEXT,teacher_kind TEXT,runs BLOB);
            INSERT INTO pages VALUES ('page003',40,'sha','train');''')
        bowl={41,42,43,44,45,81,121,161,201,241,281,282,283,284,285,124}
        bars=set(range(401,410))|set(range(481,490))
        self.masks=[sorted(bowl),sorted(bars)]
        for n,(label,role,mask) in enumerate(zip(['tanwin-fath','waqf-jeem'],['mark','symbol'],self.masks)):
            runs=[v for p in mask for v in (p,1)]
            self.db.execute('INSERT INTO items VALUES (?,\'page003\',\'s1\',1,\'machine-weak\',0,?,?,?)',
                (str(n),label,role,pack_runs(runs)))
    def tearDown(self):self.db.close()
    def test_reciprocal_swap_is_source_only_and_keeps_originals(self):
        result=corrections(self.db)
        self.assertEqual({k:v['type'] for k,v in result.items()},{'0':'waqf-jeem','1':'tanwin-fath'})
        self.assertEqual(result['0']['pairedItemId'],'1')
        self.assertEqual(result['0']['provenance'],'automatic-geometry')
        self.assertEqual(self.db.execute("SELECT teacher_type FROM items WHERE id='0'").fetchone()[0],'tanwin-fath')
    def test_no_correction_across_subwords_or_human_review(self):
        self.db.execute("UPDATE items SET subword_id='s2' WHERE id='1'")
        self.assertEqual(corrections(self.db),{})
        self.db.execute("UPDATE items SET subword_id='s1',human_reviewed=1 WHERE id='1'")
        self.assertEqual(corrections(self.db),{})
    def test_single_disagreement_and_extra_components_remain_unresolved(self):
        self.db.execute("DELETE FROM items WHERE id='1'")
        self.assertEqual(corrections(self.db),{})
        self.assertIsNone(shape_type(pieces([0,1,100,1,200,1],40)))


if __name__=='__main__':unittest.main()
