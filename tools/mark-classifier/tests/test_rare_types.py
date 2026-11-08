import importlib.util
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from automatic.learn import type_vocabulary


class VocabularyTests(unittest.TestCase):
    def test_rare_training_types_survive_but_held_out_only_types_do_not_leak(self):
        base={'in_scope':1,'split':'train','teacher_kind':'symbol','teacher_type':'waqf-meem','quality':1.}
        rows=[base,{**base,'teacher_type':'test-only','split':'test'},
            {**base,'teacher_type':'validation-only','split':'validation'},
            {**base,'teacher_type':'exterior','in_scope':0},
            {**base,'teacher_type':'unusable','quality':.1},
            {**base,'teacher_type':'small-kaf-body','teacher_kind':'subword-ink'}]
        self.assertEqual(type_vocabulary(rows),['waqf-meem'])


@unittest.skipUnless(importlib.util.find_spec('numpy'),'Optional model runtime')
class RareSamplingTests(unittest.TestCase):
    def test_hard_example_mining_uses_frozen_training_targets_only(self):
        import numpy as np
        from automatic.learn import hard_example_indices
        base={'split':'train','in_scope':1,'quality':.9,'teacher_kind':'mark','teacher_type':'fatha'}
        rows=[{**base,'id':str(i)} for i in range(7)]
        rows[1]['split']='validation';rows[2]['split']='test';rows[3]['quality']=.3
        rows[4]['teacher_kind']='unknown'
        scores=np.array([.2,.1,.1,.2,.1,.99,.3]);agreement=np.array([False,True,True,False,True,True,True])
        selected=hard_example_indices(rows,scores,agreement)
        self.assertEqual(set(selected),{6})
    def test_single_letter_body_negatives_stay_on_the_requested_split(self):
        from automatic.learn import body_hard_negatives
        base={'in_scope':1,'teacher_kind':'subword-ink','teacher_type':'unknown','quality':.72,'bases':'م','split':'train'}
        rows=[base]*350+[{**base,'split':'validation'}]*20
        selected,counts=body_hard_negatives(rows,42)
        self.assertEqual(len(selected),300)
        self.assertTrue(all(i<350 for i in selected))
        validation,_=body_hard_negatives(rows,42,limit=100,split='validation')
        self.assertEqual(len(validation),20)
        self.assertTrue(all(i>=350 for i in validation))
    def test_weak_type_gets_full_budget_without_using_other_splits(self):
        import numpy as np
        from automatic.learn import supplement_types
        types=np.array([0]*1500+[1]*20+[0]*100)
        quality=np.array([.32]*1500+[1.]*20+[1.]*100)
        initial=np.array(list(range(250))+list(range(1500,1520)))
        result=supplement_types(initial,np.arange(1520),types,quality,1400,42)
        self.assertEqual(np.sum(types[result]==0),1400)
        self.assertEqual(len(result),len(np.unique(result)))
        self.assertTrue(np.all(result<1520))
        np.testing.assert_array_equal(result,supplement_types(initial,np.arange(1520),types,quality,1400,42))
    def test_repetition_is_bounded_balanced_and_keeps_common_and_untyped_examples(self):
        import numpy as np
        from automatic.learn import training_draw_positions
        types=np.array([0]*16+[1]*150+[2]+[-1]*10)
        draws=training_draw_positions(types)
        counts=np.bincount(draws,minlength=len(types))
        self.assertTrue(np.all(counts[:16]==8))
        self.assertTrue(np.all(counts[16:166]==1))
        self.assertEqual(counts[166],8)
        self.assertTrue(np.all(counts[167:]==1))
        self.assertEqual(len(np.unique(draws)),len(types))
        np.testing.assert_array_equal(draws,training_draw_positions(types))
    def test_reviewed_metrics_check_raw_role_and_type_without_overrides(self):
        import numpy as np
        from automatic.learn import reviewed_metrics,Network,ROLES
        rows=[{'id':'held-out','teacher_kind':'symbol','teacher_type':'waqf-meem'}]
        net=Network(2,4,2)
        for value in net.p.values():value[:]=0
        net.p['br'][ROLES.index('symbol')]=10;net.p['bt'][1]=10
        args=(net,np.zeros((1,2)),np.array([0]),rows,np.zeros(2),np.ones(2),['waqf-meem','waqf-sad'],[3,3])
        self.assertEqual(reviewed_metrics(*args)['correct'],0)
        net.p['bt'][:]=[10,0]
        self.assertEqual(reviewed_metrics(*args)['correct'],1)


if __name__=='__main__':unittest.main()
