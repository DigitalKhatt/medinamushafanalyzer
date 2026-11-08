import copy
from collections import Counter
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
from native_marks import select_alef_hamza_body
from native_marks import assign_page


class AlefHamzaBodyTests(unittest.TestCase):
    def setUp(self):
        self.paths = [
            dict(id='hamza', bbox=[42, 80, 10, 10], contourCount=1, legacyType=0),
            dict(id='kasra', bbox=[43, 95, 14, 5], contourCount=1, legacyType=0),
            dict(id='alef', bbox=[40, 40, 6, 32], contourCount=1, legacyType=0),
        ]
        self.owners = [dict(id='w:c0', role='letter', character='إ', expectedContours=2,
                            expectedInventory={'hamza-below': 1, 'kasra': 1})]

    def choose(self, native='hamza'):
        return select_alef_hamza_body(self.paths, self.owners, native)

    def test_selects_stem_and_preserves_original_choice_without_mutating_inputs(self):
        before = copy.deepcopy((self.paths, self.owners))
        result = self.choose()
        self.assertEqual(result['status'], 'applied')
        self.assertEqual(result['nativeBodyPathId'], 'hamza')
        self.assertEqual(result['selectedBodyPathId'], 'alef')
        self.assertEqual((self.paths, self.owners), before)

    def test_decomposed_alef_with_hamza_below_is_supported(self):
        self.owners[0]['character'] = 'ا'
        self.assertEqual(self.choose()['selectedBodyPathId'], 'alef')

    def test_correct_native_alef_remains_selected(self):
        result = self.choose('alef')
        self.assertEqual(result['status'], 'unchanged')
        self.assertEqual(result['selectedBodyPathId'], 'alef')

    def test_other_bases_hamza_above_and_word_signs_are_ineligible(self):
        for char in ('ب', 'أ', 'آ', 'ٱ', 'ـ'):
            with self.subTest(character=char):
                self.owners[0]['character'] = char
                self.assertEqual(self.choose()['status'], 'not-applicable')
        self.owners[0]['character'] = 'إ'
        self.owners[0]['expectedInventory'] = {'hamza-above': 1, 'kasra': 1}
        self.assertEqual(self.choose()['status'], 'not-applicable')
        self.owners[0]['role'] = 'word-sign'
        self.assertEqual(self.choose()['status'], 'not-applicable')

    def test_connected_multi_letter_body_is_not_reselected(self):
        self.owners.insert(0, dict(id='w:lam', role='letter', character='ل', expectedContours=0,
                                  expectedInventory={}))
        self.assertEqual(self.choose()['status'], 'not-applicable')

    def test_ambiguous_upright_candidates_retain_native_body(self):
        self.paths.append(dict(id='small-alef', bbox=[43, 35, 6, 35], contourCount=1, legacyType=0))
        self.owners[0]['expectedContours'] += 1
        self.owners[0]['expectedInventory']['small-alef'] = 1
        result = self.choose()
        self.assertEqual(result['reason'], 'multiple-upright-candidates')
        self.assertEqual(result['selectedBodyPathId'], 'hamza')

    def test_missing_ink_or_extra_ink_cannot_justify_body_change(self):
        for count in (1, 3):
            with self.subTest(expectedContours=count):
                self.owners[0]['expectedContours'] = count
                result = self.choose()
                self.assertEqual(result['reason'], 'contour-totals-disagree')
                self.assertEqual(result['selectedBodyPathId'], 'hamza')

    def test_fused_or_tagged_native_body_is_not_split_or_reselected(self):
        self.paths[0]['contourCount'] = 2
        self.owners[0]['expectedContours'] = 3
        self.assertEqual(self.choose()['reason'], 'native-body-not-single-untyped-contour')
        self.paths[0]['contourCount'] = 1
        self.owners[0]['expectedContours'] = 2
        self.paths[0]['legacyType'] = 1
        self.assertEqual(self.choose()['reason'], 'native-body-not-single-untyped-contour')

    def test_short_wide_low_distant_or_tagged_strokes_are_not_body_candidates(self):
        original = copy.deepcopy(self.paths[2])
        for bbox, kind, count in [([40, 40, 15, 32], 0, 1), ([40, 40, 4, 19], 0, 1),
                                  ([40, 80, 6, 32], 0, 1), ([80, 40, 6, 32], 0, 1),
                                  ([40, 40, 6, 32], 1, 1), ([40, 40, 6, 32], 0, 2)]:
            with self.subTest(bbox=bbox, kind=kind, count=count):
                self.paths[2] = dict(original, bbox=bbox, legacyType=kind, contourCount=count)
                self.owners[0]['expectedContours'] = 1 + count
                result = self.choose()
                self.assertEqual(result['status'], 'fallback')
                self.assertEqual(result['selectedBodyPathId'], 'hamza')

    def test_missing_body_and_invalid_geometry_are_retained(self):
        self.assertEqual(self.choose(None)['reason'], 'native-body-unavailable')
        for bbox in ([40, 40, 0, 32], [40, 40, 6, -1], [float('nan'), 40, 6, 32]):
            with self.subTest(bbox=bbox):
                self.paths[2]['bbox'] = bbox
                self.assertEqual(self.choose()['reason'], 'invalid-bounding-box')

    def test_uniform_scale_translation_and_path_order_do_not_change_selection(self):
        original = copy.deepcopy(self.paths)
        for scale in (.2, 1.7, 5.):
            with self.subTest(scale=scale):
                self.paths = copy.deepcopy(original)
                for p in self.paths:
                    x, y, w, h = p['bbox']
                    p['bbox'] = [scale*x-103, scale*y+71, scale*w, scale*h]
                self.paths.reverse()
                self.assertEqual(self.choose()['selectedBodyPathId'], 'alef')

    def test_classifier_and_template_metadata_cannot_select_body(self):
        expected = self.choose()
        for p in self.paths:
            p['prediction'] = 'WRONG'
            p['templateScores'] = {'alef': 1 if p['id'] == 'alef' else 0}
        self.assertEqual(self.choose(), expected)

    def test_real_page003_preserves_masks_membership_and_returns_hamza_to_marks(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/mark-body-page003.json').read_text())
        before = copy.deepcopy(fixture)
        result = assign_page(fixture['native'], fixture['transcript'])
        sub = next(s for s in result['subwords'] if s['id'] == 'page003:l12-w5-s2')
        self.assertEqual(sub['nativeBodyPathId'], 'l12-w5-p3')
        self.assertEqual(sub['bodyPathId'], 'l12-w5-p4')
        self.assertEqual(sub['pathIds'], ['l12-w5-p3', 'l12-w5-p2', 'l12-w5-p4'])
        self.assertEqual(sub['owners'][0]['pathIds'], ['l12-w5-p2', 'l12-w5-p3'])
        native = {p['id']: p for p in fixture['native']['paths']}
        for path in result['paths']:
            self.assertEqual(path['runs'], native[path['id']]['runs'])
            self.assertEqual(path['bbox'], native[path['id']]['bbox'])
        claimed = [p for s in result['subwords'] for o in s['owners'] for p in o['pathIds']]
        claimed += [s['bodyPathId'] for s in result['subwords'] if s['bodyPathId']]
        self.assertEqual(Counter(claimed), Counter(native.keys()))
        self.assertEqual(fixture, before)
        self.assertTrue(result['conservation']['everyPathAccounted'])


if __name__ == '__main__':
    unittest.main()
