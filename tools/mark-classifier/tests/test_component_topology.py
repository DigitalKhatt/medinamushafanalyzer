import importlib.util
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from automatic.common import ink_components, merge_runs


@unittest.skipUnless(importlib.util.find_spec('cv2') and importlib.util.find_spec('numpy'),'Optional image runtime')
class ComponentTopologyTests(unittest.TestCase):
    def test_merged_three_dot_annotation_still_has_three_ink_components(self):
        pieces=[dict(runs=[11,2,21,2]),dict(runs=[15,2,25,2]),dict(runs=[53,2,63,2])]
        runs=merge_runs(pieces)
        self.assertEqual(ink_components(runs,[1,1,6,6],10),3)
        self.assertEqual(ink_components(merge_runs([dict(runs=runs)]),[1,1,6,6],10),3)

    def test_split_adjacent_annotation_parts_are_one_connected_ink_component(self):
        runs=merge_runs([dict(runs=[11,2,21,2]),dict(runs=[13,2,23,2])])
        self.assertEqual(ink_components(runs,[1,1,4,2],10),1)


if __name__=='__main__':unittest.main()
