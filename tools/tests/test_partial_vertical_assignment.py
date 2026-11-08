import copy
from collections import Counter
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
from native_marks import assign_page, consume, consume_with_vertical_quotas


def path(name, x, y, count=1):
    return dict(id=name, bbox=[x, y, 10, 2], contourCount=count, legacyType=0)


def bucket(name, total, below):
    return dict(id=name, expectedContours=total, expectedLowerContours=below)


class PartialVerticalTests(unittest.TestCase):
    def setUp(self):
        self.paths = [path('below', 60, 30), path('above', 55, 10), path('ambiguous', 20, 19)]
        self.buckets = [bucket('first', 1, 0), bucket('last', 2, 1)]
        self.kwargs = dict(baseline=20, body_bbox=[0, 0, 100, 40], scale=4)

    def assign(self):
        return consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs)

    def assert_retained(self, result, reason):
        self.assertEqual(result['verticalAssignment']['status'], 'fallback')
        self.assertEqual({k: v for k, v in result.items() if k != 'verticalAssignment'},
                         consume(self.paths, self.buckets))
        self.assertEqual(result['verticalAssignment']['ambiguousReservation']['reason'], reason)

    def test_clear_marks_move_while_ambiguous_ink_keeps_its_owner(self):
        before = copy.deepcopy((self.paths, self.buckets))
        result = self.assign()
        self.assertEqual([o['pathIds'] for o in result['owners']], [['above'], ['below', 'ambiguous']])
        detail = result['verticalAssignment']
        self.assertEqual(detail['reason'], 'clear-vertical-order-with-locked-ambiguous-paths')
        locked = detail['ambiguousReservation']['owners'][0]
        self.assertEqual(locked['ownerId'], 'last')
        self.assertEqual(locked['pathIds'], ['ambiguous'])
        self.assertEqual((locked['reservedAboveContours'], locked['reservedBelowContours']), (1, 0))
        self.assertEqual((self.paths, self.buckets), before)

    def test_multiple_locked_owners_with_unique_reservations_are_supported(self):
        self.paths = [path('below-a', 100, 30), path('above-a', 95, 10), path('amb-a', 60, 19),
                      path('below-b', 40, 30), path('above-b', 35, 10), path('amb-b', 0, 19)]
        self.buckets = [bucket('a', 1, 0), bucket('b', 2, 1), bucket('c', 1, 0), bucket('d', 2, 1)]
        result = self.assign()
        self.assertEqual([o['pathIds'] for o in result['owners']],
                         [['above-a'], ['below-a', 'amb-a'], ['above-b'], ['below-b', 'amb-b']])

    def test_multiple_residual_quota_solutions_keep_original_ownership(self):
        self.paths = [path('above', 80, 10), path('amb-a', 60, 19),
                      path('below', 40, 30), path('amb-b', 20, 19)]
        self.buckets = [bucket('a', 2, 1), bucket('b', 2, 1)]
        self.assert_retained(self.assign(), 'ambiguous-side-reservations-not-unique')

    def test_distant_ambiguous_ink_stays_with_its_original_owner(self):
        self.paths = [path('ambiguous', 60, 19), path('above', 40, 10)]
        self.buckets = [bucket('a', 1, 0), bucket('b', 1, 1)]
        self.assert_retained(self.assign(), 'ambiguous-contours-cannot-fill-side-quotas')

    def test_excess_clear_contours_cannot_be_hidden_by_ambiguous_reservations(self):
        self.paths[0]['bbox'][1] = 10
        self.buckets = [bucket('a', 1, 0), bucket('b', 2, 2)]
        self.assert_retained(self.assign(), 'clear-contours-exceed-side-quotas')

    def test_packed_ambiguous_path_cannot_be_split_between_side_quotas(self):
        self.paths[-1]['contourCount'] = 2
        self.buckets[-1] = bucket('last', 3, 2)
        self.assert_retained(self.assign(), 'ambiguous-contours-cannot-fill-side-quotas')

    def test_whole_packed_ambiguous_path_can_be_reserved_without_splitting(self):
        self.paths[-1]['contourCount'] = 2
        self.buckets[-1] = bucket('last', 3, 1)
        result = self.assign()
        self.assertEqual([o['pathIds'] for o in result['owners']], [['above'], ['below', 'ambiguous']])
        self.assertEqual(result['owners'][-1]['assignedContours'], 3)

    def test_failed_clear_stream_consumption_restores_every_original_owner(self):
        self.paths[1]['contourCount'] = 2
        self.buckets[-1] = bucket('last', 3, 1)
        result = self.assign()
        self.assertEqual(result['verticalAssignment']['reason'], 'vertical-stream-contour-overflow-or-shortfall')
        self.assertEqual({k: v for k, v in result.items() if k != 'verticalAssignment'},
                         consume(self.paths, self.buckets))

    def test_reservations_are_per_owner_without_assigning_sides_to_individual_ambiguous_paths(self):
        self.paths.append(path('ambiguous-two', 0, 19))
        self.buckets[-1] = bucket('last', 3, 2)
        result = self.assign()
        self.assertEqual([o['pathIds'] for o in result['owners']], [['above'], ['below', 'ambiguous', 'ambiguous-two']])
        locked = result['verticalAssignment']['ambiguousReservation']['owners'][0]
        self.assertEqual((locked['reservedAboveContours'], locked['reservedBelowContours']), (1, 1))
        self.assertTrue(all(result['verticalAssignment']['pathSides'][pid] == 'ambiguous' for pid in locked['pathIds']))

    def test_translation_scale_path_order_and_predictions_do_not_choose_ownership(self):
        expected = [o['pathIds'] for o in self.assign()['owners']]
        for p in self.paths:
            x, y, w, h = p['bbox']; p['bbox'] = [2*x+7, 2*y+11, 2*w, 2*h]
            p['prediction'] = 'WRONG'; p['templateScores'] = {'kasra': 1}
        for b in self.buckets:
            b['expectedInventory'] = {'made-up-label': 100}
        self.paths.reverse()
        self.kwargs = dict(baseline=51, body_bbox=[7, 11, 200, 80], scale=8)
        self.assertEqual([o['pathIds'] for o in self.assign()['owners']], expected)

    def test_nonoverlapping_or_just_touching_replacements_keep_original_ownership(self):
        for x in (40, 50):
            with self.subTest(x=x):
                self.paths[1]['bbox'][0] = x
                result = self.assign()
                self.assertEqual(result['verticalAssignment']['reason'], 'partial-vertical-exchange-not-local')
                self.assertEqual({k: v for k, v in result.items() if k != 'verticalAssignment'},
                                 consume(self.paths, self.buckets))

    def test_page005_real_masks_keep_small_yeh_and_correct_sukun_kasra(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/mark-partial-vertical-page005.json').read_text())
        before = copy.deepcopy(fixture)
        result = assign_page(fixture['native'], fixture['transcript'])
        sub = next(s for s in result['subwords'] if s['id'] == 'page005:l5-w3-s1')
        owners = {o['text']: o for o in sub['owners']}
        self.assertEqual(owners['حْ']['pathIds'], ['l5-w3-p7'])
        self.assertEqual(owners['يِۦٓ']['pathIds'], ['l5-w3-p6', 'l5-w3-p8', 'l5-w3-p9'])
        self.assertEqual(sub['verticalAssignment']['ambiguousReservation']['owners'][0]['pathIds'], ['l5-w3-p9'])
        self.assertEqual(sub['bodyPathId'], 'l5-w3-p1')
        native = {p['id']: p for p in fixture['native']['paths']}
        for p in result['paths']:
            self.assertEqual(p['runs'], native[p['id']]['runs'])
            self.assertEqual(p['bbox'], native[p['id']]['bbox'])
        assigned = [pid for o in sub['owners'] for pid in o['pathIds']]+[sub['bodyPathId']]
        self.assertEqual(Counter(assigned), Counter(native.keys()))
        self.assertTrue(result['conservation']['everyPathAccounted'])
        self.assertEqual(fixture, before)

    def test_page320_global_baseline_does_not_move_marks_across_distant_letters(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/mark-partial-vertical-page320.json').read_text())
        result = assign_page(fixture['native'], fixture['transcript'])
        sub = next(s for s in result['subwords'] if s['id'] == 'page320:l4-w1-s1')
        owners = {o['text']: o for o in sub['owners']}
        self.assertEqual(sub['verticalAssignment']['reason'], 'partial-vertical-exchange-not-local')
        self.assertEqual(owners['ئِ']['pathIds'], ['l4-w1-p7', 'l4-w1-p8'])
        self.assertEqual(owners['كَ']['pathIds'], ['l4-w1-p9'])
        self.assertEqual(owners['ةِ']['pathIds'], ['l4-w1-p10', 'l4-w1-p11'])
        self.assertTrue(result['conservation']['everyPathAccounted'])


class BoundaryPreferenceTests(unittest.TestCase):
    def setUp(self):
        # Page 575, line 12, word 10, subword 2. Only the native geometry and
        # transcript contour quotas enter this stage, before mark classification.
        boxes = [[637.98, 1988.64, 17.36, 11.74], [597.15, 1981.91, 27.88, 14.09],
                 [581.57, 2070.75, 24.82, 13.37], [596.58, 2029.72, 8.18, 23.95],
                 [577.55, 2020.44, 9.42, 9.36], [545.32, 2089.27, 27.88, 14.09]]
        self.paths = [dict(id=f'p{i}', bbox=box, contourCount=1) for i, box in enumerate(boxes, 3)]
        self.owners = [bucket('lam', 1, 0), bucket('kaf', 2, 0), bucket('fa', 2, 1), bucket('ra', 1, 1)]
        self.baseline = 2052.72

    def assign(self):
        return consume_with_vertical_quotas(self.paths, self.owners, self.baseline,
                                            body_bbox=[529.69, 1975.65, 129.78, 109.19], scale=5.33)

    def test_clear_kasra_yields_to_next_ambiguous_path_for_kaf(self):
        result = self.assign()
        self.assertEqual([o['pathIds'] for o in result['owners']],
                         [['p3'], ['p4', 'p6'], ['p5', 'p7'], ['p8']])
        trace = result['verticalAssignment']
        self.assertEqual(trace['pathSides']['p6'], 'ambiguous')
        self.assertEqual(trace['boundaryPreferences'], [
            dict(ownerId='kaf', nextOwnerId='fa', preferredPathId='p6',
                 deferredPathId='p5', deferredSide='below')])
        self.assertTrue(all(o['assignedContours'] == o['expectedContours'] for o in result['owners']))

    def test_same_preference_for_a_letter_requiring_below_ink(self):
        for path in self.paths:
            x, y, width, height = path['bbox']
            path['bbox'] = [x, 2*self.baseline-y-height, width, height]
        for owner in self.owners:
            owner['expectedLowerContours'] = owner['expectedContours']-owner['expectedLowerContours']
        self.paths.reverse()
        result = self.assign()
        self.assertEqual([o['pathIds'] for o in result['owners']],
                         [['p3'], ['p4', 'p6'], ['p5', 'p7'], ['p8']])
        self.assertEqual(result['verticalAssignment']['boundaryPreferences'][0]['deferredSide'], 'above')

    def test_compatible_clear_path_keeps_its_turn(self):
        self.owners[1]['expectedLowerContours'] = 1
        self.owners[2]['expectedLowerContours'] = 0
        result = self.assign()
        self.assertEqual([o['pathIds'] for o in result['owners']],
                         [['p3'], ['p4', 'p5'], ['p6', 'p7'], ['p8']])
        self.assertNotIn('boundaryPreferences', result['verticalAssignment'])

    def test_touching_boxes_do_not_exchange_even_when_the_next_path_is_ambiguous(self):
        self.paths[2]['bbox'] = [582, 2071, 24, 13]
        self.paths[3]['bbox'] = [574, 2030, 8, 24]
        self.paths[4]['bbox'][0] = 568
        result = self.assign()
        self.assertIn('p6', result['owners'][2]['pathIds'])
        self.assertNotIn('boundaryPreferences', result['verticalAssignment'])

    def test_reject_exchange_if_an_earlier_letter_would_take_the_deferred_path(self):
        paths = [path('upper', 60, 10), path('lower', 59, 30),
                 path('unknown', 58, 19), path('other-lower', 57, 30)]
        owners = [bucket('a', 1, 1), bucket('b', 1, 0), bucket('c', 1, 1), bucket('d', 1, 0)]
        result = consume_with_vertical_quotas(paths, owners, 20, body_bbox=[0, 0, 100, 40], scale=4)
        self.assertEqual([o['pathIds'] for o in result['owners']],
                         [['upper'], ['lower'], ['unknown'], ['other-lower']])
        self.assertNotIn('boundaryPreferences', result['verticalAssignment'])


if __name__ == '__main__':
    unittest.main()
