import importlib.util
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))


@unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('cv2'),'Optional visual dependencies')
class StructureTests(unittest.TestCase):
    def test_letter_body_and_kaf_stroke_cannot_be_triple_dots(self):
        import numpy as np
        from automatic.structure import triple_structure
        ring=np.zeros((28,24),dtype='u1');ring[3:24,3:21]=255;ring[7:20,7:17]=0
        stroke=np.zeros_like(ring);stroke[3:24,10:14]=255;stroke[12:16,5:14]=255
        for mask in (ring,stroke):
            evidence=triple_structure(mask);self.assertFalse(evidence['accepted']);self.assertEqual(evidence['components'],1)
    def test_cluster_requires_aligned_single_above_pair_and_no_counters(self):
        import numpy as np
        from automatic.structure import triple_structure
        mask=np.zeros((25,24),dtype='u1');mask[0:8,6:14]=255;mask[10:18,2:18]=255
        self.assertTrue(triple_structure(mask)['accepted'])
        horizontal=np.zeros((25,40),dtype='u1');horizontal[0:8,0:8]=255;horizontal[0:8,12:28]=255
        self.assertFalse(triple_structure(horizontal)['accepted'])
        mask[2:5,8:11]=0;self.assertFalse(triple_structure(mask)['accepted'])
    def test_three_separate_waqf_dots_are_not_joined_letter_triple_dots(self):
        import numpy as np
        from automatic.structure import triple_structure
        mask=np.zeros((24,24),dtype='u1');mask[1:7,8:14]=255;mask[12:18,1:7]=255;mask[12:18,15:21]=255
        result=triple_structure(mask);self.assertEqual(result['components'],3);self.assertFalse(result['accepted'])
    def test_tiny_raster_gap_in_lower_pair_is_preserved(self):
        import numpy as np
        from automatic.structure import triple_structure
        mask=np.zeros((24,24),dtype='u1');mask[0:8,3:11]=255;mask[10:18,0:8]=255;mask[9:17,9:17]=255
        result=triple_structure(mask);self.assertEqual(result['components'],3);self.assertTrue(result['accepted'])
        self.assertEqual(result['lowerPairRasterGap'],1)
        self.assertTrue(triple_structure(mask,native_contours=2)['accepted'])
        self.assertFalse(triple_structure(mask,native_contours=3)['accepted'])
    def test_hard_negatives_use_only_clean_train_body_labels(self):
        from automatic.learn import body_hard_negatives
        base={'split':'train','in_scope':1,'teacher_kind':'subword-ink','teacher_type':'unknown','quality':.8,'bases':'ه'}
        rows=[base,{**base,'bases':'ة'},{**base,'teacher_type':'small-kaf-body','bases':'بك'},
              {**base,'split':'validation'},{**base,'split':'test'},{**base,'in_scope':0},
              {**base,'teacher_kind':'unknown'},{**base,'quality':.2}]
        selected,counts=body_hard_negatives(rows,42)
        self.assertEqual(set(selected),{0,1,2});self.assertEqual(counts,{'isolated-heh-body':2,'small-kaf-body':1})


if __name__=='__main__':unittest.main()
