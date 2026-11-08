import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
from vector_review.staggered_geometry import find_staggered_groups, apply_staggered_groups, mask_runs
from vector_review.stroke_geometry import stroke_pair


class IndependentStaggeredTests(unittest.TestCase):
    def cases(self):
        return json.loads((Path(__file__).parent/'fixtures/staggered-kasr-regressions.json').read_text())

    def groups(self, case):
        paths = [{**p, 'line': 1} for p in case['paths']]
        baseline = min(p['bbox'][1] for p in paths)-10
        return paths, find_staggered_groups(paths, case['width'], {'1': baseline})

    def test_five_pairs_detected_without_words_subwords_or_transcript(self):
        for case in self.cases():
            with self.subTest(word=case['word']):
                paths, groups = self.groups(case)
                self.assertEqual(len(groups), 1)
                self.assertEqual(mask_runs(groups[0]['paths']), mask_runs(paths))
                self.assertFalse(stroke_pair(paths))
                renamed = [{**p, 'id': 'outline-'+str(i), 'wordId': 'wrong',
                            'subwordId': 'wrong', 'text': 'unrelated'} for i, p in enumerate(paths)]
                again = find_staggered_groups(renamed, case['width'],
                                             {'1': min(p['bbox'][1] for p in paths)-10})
                self.assertEqual([g['id'] for g in groups], [g['id'] for g in again])
                # Moving/rescaling the outlines preserves their geometric relation.
                moved = []
                for p in paths:
                    x, y, a, length = p['stroke']
                    bx, by, w, h = p['bbox']
                    moved.append({**p, 'stroke': (x*2.3+17, y*2.3+31, a, length*2.3),
                                  'bbox': [bx*2.3+17, by*2.3+31, w*2.3, h*2.3]})
                self.assertTrue(stroke_pair(moved, staggered=True))

    def test_side_by_side_kasras_cannot_use_staggered_reach(self):
        for case in self.cases():
            paths = copy.deepcopy(case['paths'])
            # Same direction/size/along-offset, but no vertical stacking.
            paths[1]['bbox'][1] = paths[0]['bbox'][1]
            self.assertFalse(stroke_pair(paths, staggered=True))
            paths = copy.deepcopy(case['paths'])
            paths[1]['bbox'][0] = paths[0]['bbox'][0]-paths[1]['bbox'][2]-1
            self.assertFalse(stroke_pair(paths, staggered=True))

    def test_group_is_applied_independently_of_teacher_labels_and_owners(self):
        case = self.cases()[0]
        paths, groups = self.groups(case)
        for same_owner in (True, False):
            document = {'paths': paths, 'provenance': {}, 'subwords': [],
                'candidates': [{'id': str(i), 'pathIds': [path['id']], 'kind': 'mark',
                    'markType': 'kasra', 'wordId': 'w1' if same_owner else 'w'+str(i),
                    'subwordId': 's1' if same_owner else 's'+str(i), 'line': 1, 'issues': []}
                    for i, path in enumerate(paths)]}
            result = apply_staggered_groups(copy.deepcopy(document), groups)
            self.assertEqual(len(result['candidates']), 1)
            group = result['candidates'][0]
            self.assertEqual(set(group['pathIds']), {p['id'] for p in paths})
            self.assertEqual(group['wordId'], 'w1' if same_owner else None)
            self.assertEqual(group['subwordId'], 's1' if same_owner else None)
            self.assertEqual(apply_staggered_groups(copy.deepcopy(result), groups), result)

    def test_baseline_and_bad_shape_prevent_grouping(self):
        for case in self.cases():
            paths, _ = self.groups(case)
            self.assertEqual(find_staggered_groups(paths, case['width'], {}), [])
            below_all = max(p['bbox'][1]+p['bbox'][3] for p in paths)+10
            self.assertEqual(find_staggered_groups(paths, case['width'], {'1': below_all}), [])
            paths = copy.deepcopy(case['paths'])
            x, y, a, length = paths[1]['stroke']
            paths[1]['stroke'] = (x, y, a, length*1.4)
            self.assertFalse(stroke_pair(paths, staggered=True))
