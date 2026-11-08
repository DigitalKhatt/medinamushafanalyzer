import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from automatic.common import FEATURE_DIM,pack_runs
from automatic.weak_corrections import damma_high_yeh_labels


class DammaTargetTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name);(self.root/'proposals').mkdir()
        self.db=sqlite3.connect(':memory:');self.db.row_factory=sqlite3.Row
        self.db.executescript((Path(__file__).parents[1]/'automatic/schema.sql').read_text())
        self.db.execute("insert into pages(page_id,split,source_sha,width) values ('page003','train','sha',40)")
        self.runs=[v for y in range(26) for x in range(20) if y in (0,25) or x in (0,19) for v in (40*y+x,1)]
        self.db.execute("""insert into items(id,page_id,in_scope,teacher_kind,teacher_type,origin,human_reviewed,runs)
            values ('page003:c','page003',1,'mark','small-high-yeh','machine-weak',0,?)""",(pack_runs(self.runs),))
        self.doc={'sourceSha256':'sha','paths':[{'id':'p','contourCount':1,'templateScores':{'damma':.01}}],
            'candidates':[{'id':'c','pathIds':['p']}]}
        self.save()
    def save(self):
        from review_bridge import proposal_digest
        (self.root/'proposals/page003.json').write_text(json.dumps(self.doc))
        self.db.execute('update pages set proposal_sha=?',(proposal_digest(self.doc),))
    def tearDown(self):self.db.close();self.temp.cleanup()
    def test_native_damma_cleans_only_automatic_target_without_changing_source(self):
        correction=damma_high_yeh_labels(self.db,self.root)['page003:c']
        self.assertEqual((correction['type'],correction['split'],correction['provenance']),('damma','train','automatic-geometry'))
        self.assertEqual(self.db.execute('select teacher_type,human_reviewed,runs from items').fetchone()[:],
            ('small-high-yeh',0,pack_runs(self.runs)))
    def test_held_out_pages_and_human_reviews_cannot_supply_cleaned_train_targets(self):
        for split in ('validation','test'):
            self.db.execute('update pages set split=?',(split,))
            self.assertEqual(damma_high_yeh_labels(self.db,self.root),{})
        self.db.execute("update pages set split='train'");self.db.execute('update items set human_reviewed=1')
        self.assertEqual(damma_high_yeh_labels(self.db,self.root),{})
    def test_native_template_alone_cannot_relabel_an_open_shape(self):
        opened=[v for p in self.runs[::2] if p!=10 for v in (p,1)]
        self.db.execute('update items set runs=?',(pack_runs(opened),))
        self.assertEqual(damma_high_yeh_labels(self.db,self.root),{})
    def test_wrong_source_weak_template_or_multiple_native_contours_are_rejected(self):
        for field,value in [('sourceSha256','changed'),('contourCount',2),('templateScores',{'damma':.3})]:
            if field=='sourceSha256':self.doc[field]=value
            else:self.doc['paths'][0][field]=value
            self.save();self.assertEqual(damma_high_yeh_labels(self.db,self.root),{})
            self.doc['sourceSha256']='sha';self.doc['paths'][0].update(contourCount=1,templateScores={'damma':.01})
    def test_stale_proposal_snapshot_cannot_supply_a_new_target(self):
        self.db.execute("update pages set proposal_sha='older-snapshot'")
        self.assertEqual(damma_high_yeh_labels(self.db,self.root),{})


@unittest.skipUnless(importlib.util.find_spec('numpy'),'Optional model runtime')
class ShapePairTests(unittest.TestCase):
    def test_neighborhood_and_baseline_features_do_not_affect_shape_probability(self):
        import numpy as np
        from automatic.damma_yeh import COLUMNS,probability
        a=np.zeros((1,FEATURE_DIM),dtype='u1');b=a.copy();b[:,256:512]=255;b[:,517]=255;b[:,521:524]=255
        weights={'mean':np.zeros(len(COLUMNS)),'std':np.ones(len(COLUMNS)),'w':np.ones(len(COLUMNS)),'b':0.}
        np.testing.assert_array_equal(probability(a,weights),probability(b,weights))
    def test_pair_refinement_conserves_probability_and_leaves_other_outputs_alone(self):
        import numpy as np
        from automatic.damma_yeh import COLUMNS,refine
        from automatic.learn import predictions,ROLES
        rp=np.zeros((3,len(ROLES)),dtype='f4');rp[:,2]=1
        tp=np.asarray([[.58,.40,.02],[.58,.40,.02],[.03,.02,.95]],dtype='f4');old=tp.copy()
        metadata={'types':['small-high-yeh','damma','fatha'],'typeRoles':[2,2,2]}
        roles,labels,_,_=predictions(rp,tp,metadata['typeRoles'])
        weights={'mean':np.zeros(len(COLUMNS)),'std':np.ones(len(COLUMNS)),'w':np.zeros(len(COLUMNS)),'b':12.}
        evidence,ids,_,_,_=refine(rp,tp,roles,labels,np.zeros((3,FEATURE_DIM)),metadata,weights,excluded={1})
        self.assertEqual(list(evidence),[0]);self.assertEqual(list(ids),[0]);self.assertEqual(labels.tolist(),[1,0,2])
        self.assertAlmostEqual(float(tp[0,:2].sum()),float(old[0,:2].sum()))
        np.testing.assert_array_equal(tp[1:],old[1:]);self.assertEqual(tp[0,2],old[0,2]);self.assertEqual(roles.tolist(),[2,2,2])
    def test_refinement_cannot_promote_an_unrelated_third_class(self):
        import numpy as np
        from automatic.damma_yeh import COLUMNS,refine
        from automatic.learn import predictions,ROLES
        rp=np.zeros((1,len(ROLES)),dtype='f4');rp[:,2]=1
        tp=np.asarray([[.5000001,0,.4999999]],dtype='f4');old=tp.copy()
        metadata={'types':['small-high-yeh','damma','fatha'],'typeRoles':[2,2,2]}
        roles,labels,_,_=predictions(rp,tp,metadata['typeRoles'])
        weights={'mean':np.zeros(len(COLUMNS)),'std':np.ones(len(COLUMNS)),'w':np.zeros(len(COLUMNS)),'b':12.}
        evidence,*_=refine(rp,tp,roles,labels,np.zeros((1,FEATURE_DIM)),metadata,weights)
        self.assertFalse(evidence);np.testing.assert_array_equal(tp,old)


if __name__=='__main__':unittest.main()
