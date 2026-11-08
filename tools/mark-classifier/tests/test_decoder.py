import importlib.util
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))


@unittest.skipUnless(importlib.util.find_spec('numpy'),'Optional numerical runtime')
class DecoderTests(unittest.TestCase):
    def test_dammatan_type_survives_a_slightly_stronger_symbol_vote(self):
        import numpy as np
        from automatic.learn import predictions
        from automatic.decoder_audit import legacy_predictions
        # Measured head outputs for page111:l6-w7-s1-c6. The last type groups
        # the small remaining mark votes; it cannot affect either winner.
        role=np.array([[.000914459,.000249961,.411890239,.585395932,.000914264,.000546086,.000089131]])
        type=np.array([[.946685672,.017706241,.017183295,.018424792]])
        mapping=[2,3,3,2]
        self.assertEqual(tuple(v[0] for v in legacy_predictions(role,type,mapping)[:2]),(3,1))
        r,t,score,margin=predictions(role,type,mapping)
        self.assertEqual((r[0],t[0]),(2,0))
        self.assertAlmostEqual(score[0],.389930576,places=6)
        self.assertLessEqual(score[0],role[0,r[0]])
        self.assertLessEqual(score[0],type[0,t[0]])
        self.assertGreater(margin[0],.37)
    def test_untyped_body_does_not_inherit_a_confident_but_untrained_type_vote(self):
        import numpy as np
        from automatic.learn import predictions
        role=np.array([[.98,.003,.008,.004,.003,.001,.001]])
        r,t,score,_=predictions(role,np.array([[.99,.01]]),[2,3])
        self.assertEqual((r[0],t[0]),(0,-1));self.assertEqual(score[0],.98)
    def test_missing_type_vocabulary_retains_role_with_unknown_type(self):
        import numpy as np
        from automatic.learn import predictions
        role=np.array([[.01,.01,.01,.94,.01,.01,.01]])
        r,t,score,_=predictions(role,np.array([[1.]]),[2])
        self.assertEqual((r[0],t[0]),(3,-1));self.assertEqual(score[0],.94)
    def test_role_type_pairs_remain_compatible_and_scores_are_not_rescaled(self):
        import numpy as np
        from automatic.learn import predictions
        rng=np.random.default_rng(73);rp=rng.dirichlet(np.ones(7),200);tp=rng.dirichlet(np.ones(5),200)
        mapping=np.array([1,2,2,3,3]);r,t,score,margin=predictions(rp,tp,mapping);typed=t>=0
        np.testing.assert_array_equal(r[typed],mapping[t[typed]])
        self.assertTrue(np.all(score<=rp[np.arange(len(r)),r]+1e-12))
        self.assertTrue(np.all(margin>=0))
        np.testing.assert_allclose(score[typed],rp[np.flatnonzero(typed),r[typed]]*tp[np.flatnonzero(typed),t[typed]])
    def test_missing_type_predictions_count_as_misses_not_last_type(self):
        import numpy as np
        from automatic.learn import f1_score
        result=f1_score(np.array([1,1,-1]),np.array([-1,1,0]),2)
        self.assertEqual(result['support'],[0,2]);self.assertEqual(result['accuracy'],.5)
        self.assertEqual(result['confusion'][1],[0,1,1]);self.assertEqual(result['recall'][1],.5)
    def test_evaluation_uses_the_same_pair_decoder_as_inference(self):
        import numpy as np
        from automatic.learn import evaluate
        class Heads:
            def forward(self,x):
                return np.array([[.001,.001,.41,.585,.001,.001,.001]]),np.array([[.95,.05]]),None
        metric=evaluate(Heads(),np.zeros((1,2)),np.array([0]),np.array([2]),np.array([0]),
            np.zeros(2),np.ones(2),['tanwin-damm','waqf-meem'],[2,3])
        self.assertEqual(metric['role']['accuracy'],1.)
        self.assertEqual(metric['type']['accuracy'],1.)


if __name__=='__main__':unittest.main()
