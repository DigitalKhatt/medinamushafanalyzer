import importlib.util
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))


@unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('PIL'),'Optional audit runtime')
class ReviewedSourceTransitionTests(unittest.TestCase):
    def fixture(self):
        from audit_cache_regeneration import mask_key
        old=dict(id='page443:c',review_candidate='c',runs=[10,10],word_id='w',subword_id='s',
                 final_kind='mark',final_type='small-seen')
        new=dict(id='page443:human:p',review_candidate='c',runs=[10,11],word_id='w',subword_id='s',
                 final_kind='mark',final_type='small-seen',human_reviewed=1,maskKey=mask_key([10,11]))
        part=dict(id='p',runs=[10,11],reviewed=True,role='mark',markType='small-seen',
                  proposalSource=dict(candidateId='c',proposalSha256='original-reviewed-snapshot'))
        return old,new,dict(parts=[part])

    def test_original_approval_preserved_on_its_saved_workbench_mask(self):
        from audit_cache_regeneration import reviewed_source_transition
        old,new,doc=self.fixture()
        result=reviewed_source_transition(old,doc,{new['maskKey']:new})
        self.assertEqual(result['reviewCandidate'],'c')
        self.assertEqual(result['beforeArea'],10)
        self.assertEqual(result['afterArea'],11)

    def test_no_approval_can_be_inferred_from_proximity_or_label_agreement(self):
        from audit_cache_regeneration import reviewed_source_transition
        for field,value in [('human_reviewed',0),('review_candidate','different'),('final_type','fatha'),('word_id','other-word')]:
            old,new,doc=self.fixture();new[field]=value
            self.assertIsNone(reviewed_source_transition(old,doc,{new['maskKey']:new}))
        old,new,doc=self.fixture();doc['parts'][0]['reviewed']=False
        self.assertIsNone(reviewed_source_transition(old,doc,{new['maskKey']:new}))


if __name__=='__main__':unittest.main()
