import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from native_marks import assign_page, mask_union


class RemainingOwnershipTests(unittest.TestCase):
    cases = json.loads((ROOT / 'tools/tests/fixtures/mark-remaining-ownership.json').read_bytes())
    bank = json.loads((ROOT / 'templates/mark-shapes.json').read_bytes())['prototypes']

    def assign(self, sid, case=None):
        case = self.cases[sid] if case is None else case
        result = assign_page(case['native'], case['transcript'], shape_bank=self.bank)
        sub = next(s for s in result['subwords'] if s['id'] == sid)
        return result, sub

    def test_lam_alef_arm_and_the_separate_maddah(self):
        sid = 'page536:l14-w8-s1'
        result, sub = self.assign(sid)
        body = sub['compoundBody']
        self.assertEqual(body['pathIds'], ['l14-w8-p0', 'l14-w8-p4'])
        self.assertEqual(sub['owners'][-1]['pathIds'], ['l14-w8-p5'])
        paths = {p['id']: p for p in result['paths']}
        self.assertEqual(body['runs'], mask_union([paths[k] for k in body['pathIds']]))
        self.assertEqual(sub['unusedPathIds'], [])

    def test_arm_allows_the_shared_raster_contact_pixel(self):
        result, sub = self.assign('page259:l10-w8-s2')
        paths = {p['id']: p for p in result['paths']}
        body = sub['compoundBody']
        parts = [paths[k] for k in body['pathIds']]
        self.assertEqual(body['secondaryPathId'], 'l10-w8-p5')
        self.assertEqual(mask_union(parts), body['runs'])
        self.assertEqual(sum(sum(p['runs'][1::2]) for p in parts) - sum(body['runs'][1::2]), 1)

    def test_wrong_arm_outline_or_two_plausible_arms_are_inconclusive(self):
        sid = 'page536:l14-w8-s1'
        for scenario in ('reflect', 'duplicate'):
            with self.subTest(scenario=scenario):
                case = copy.deepcopy(self.cases[sid])
                paths = {p['id']: p for p in case['native']['paths']}
                part = paths['l14-w8-p4']
                if scenario == 'duplicate':
                    other = paths['l14-w8-p5']
                    other.update(bbox=part['bbox'], runs=part['runs'])
                else:
                    width = case['native']['raster']['width']
                    spans = list(zip(part['runs'][::2], part['runs'][1::2]))
                    left = min(start % width for start, _ in spans)
                    right = max(start % width + length for start, length in spans)
                    reflected = sorted((start // width * width + left + right - start % width - length, length)
                                       for start, length in spans)
                    part['runs'] = [value for pair in reflected for value in pair]
                _, sub = self.assign(sid, case)
                self.assertNotIn('compoundBody', sub)

    def test_two_packed_vowels_retain_their_pixels_and_individual_owners(self):
        for sid, parent, owners in (
            ('page387:l15-w4-s1', 'l15-w4-p3', [1, 2]),
            ('page303:l1-w12-s2', 'l1-w12-p9', [1, 2]),
        ):
            with self.subTest(sid=sid):
                result, sub = self.assign(sid)
                self.assertEqual(sub['dotTemplate']['packedPathId'], parent)
                self.assertEqual(sub['markDecomposition']['method'], 'dot-templates-and-packed-vowels')
                parts = [p for p in result['assignmentComponents'] if p['sourcePathId'] == parent]
                self.assertEqual(len(parts), 2)
                source = next(p for p in result['paths'] if p['id'] == parent)
                self.assertEqual(mask_union(parts), mask_union([source]))
                self.assertEqual(sum(sum(p['runs'][1::2]) for p in parts), sum(source['runs'][1::2]))
                part_ids = {p['id'] for p in parts}
                self.assertEqual([i for i,o in enumerate(sub['owners']) if part_ids.intersection(o['pathIds'])], owners)
                self.assertFalse(any(o['issues'] or o['groupingIssues'] for o in sub['owners']))

    def test_template_swaps_agree_with_owner_trace_and_cursor(self):
        for sid, owners in (
            ('page454:l1-w5-s4', [['p9'], ['p8']]),
            ('page454:l4-w2-s2', [['p4'], ['p3', 'p5'], ['p6'], []]),
            ('page560:l11-w4-s1', [['p1', 'p2', 'p3', 'p5'], ['p4', 'p6'], ['p7', 'p8', 'p9'], ['p10', 'p11']]),
            ('page579:l9-w9-s2', [['p5'], ['p4', 'p7'], ['p6', 'p8'], ['p9'], ['p10'], []]),
        ):
            with self.subTest(sid=sid):
                _, sub = self.assign(sid)
                self.assertTrue(sub['templateBoundarySwaps'])
                self.assertEqual([[p.split('-')[-1] for p in o['pathIds']] for o in sub['owners']], owners)
                for owner in sub['owners']:
                    self.assertEqual(owner['pathIds'], [t['pathId'] for t in owner['trace']])
                    self.assertEqual(owner['pathIds'], sub['consumedOrder'][owner['startIndex']:owner['endIndex']])

    def test_missing_shape_evidence_preserves_rtl(self):
        sid = 'page454:l1-w5-s4'
        case = copy.deepcopy(self.cases[sid])
        sukun = next(p for p in case['native']['paths'] if p['id'] == 'l1-w5-p8')
        sukun['signature'] = '0'
        _, sub = self.assign(sid, case)
        self.assertNotIn('templateBoundarySwaps', sub)
        self.assertEqual(sub['owners'][0]['pathIds'], ['l1-w5-p8'])

    def test_dot_stays_with_its_owner_if_the_local_body_is_inconclusive(self):
        sid = 'page579:l9-w9-s2'
        case = copy.deepcopy(self.cases[sid])
        word = case['native']['words'][0]
        body_id = next(s for s in word['subwords'] if s['id'] == 'l9-w9-s2')['pathIds'][0]
        paths = {p['id']: p for p in case['native']['paths']}
        body = paths[body_id]
        body['runs'] = mask_union([body, paths['l9-w9-p4']])
        _, sub = self.assign(sid, case)
        self.assertEqual(sub['owners'][0]['pathIds'], ['l9-w9-p4'])
        self.assertEqual(len(sub['templateBoundarySwaps']), 1)  # The vowels still have separate proof.


if __name__ == '__main__':
    unittest.main()
