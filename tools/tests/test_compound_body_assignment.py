import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from native_marks import assign_page, mask_union, painted_source_paths


def pixels(runs):
    return {i for start, count in zip(runs[::2], runs[1::2]) for i in range(start, start + count)}


class CompoundBodyTests(unittest.TestCase):
    cases = json.loads((ROOT/'tools/tests/fixtures/mark-compound-bodies.json').read_text())

    def assign(self, case, sid):
        result = assign_page(case['native'], case['transcript'])
        sub = next(s for s in result['subwords'] if s['id'].endswith(sid))
        return result, sub

    def check_body(self, page, sid, primary, secondary, operation, owners):
        case = self.cases[page]
        result, sub = self.assign(case, sid)
        body = sub['compoundBody']
        prefix = sid.split('-s')[0] + '-p'
        self.assertEqual(body['operation'], operation)
        self.assertEqual(body['pathIds'], [prefix+str(primary), prefix+str(secondary)])
        self.assertEqual(sub['bodyPathId'], body['primaryPathId'])
        self.assertEqual(sub['unusedPathIds'], [])
        self.assertEqual([[p.removeprefix(prefix) for p in o['pathIds']] for o in sub['owners']],
                         [[str(i) for i in ids] for ids in owners])
        by_id = {p['id']: pixels(p['runs']) for p in case['native']['paths']}
        self.assertEqual(by_id, {p['id']: pixels(p['runs']) for p in result['paths']})
        a, b = (by_id[p] for p in body['pathIds'])
        if operation == 'add':
            self.assertTrue(a.isdisjoint(b))
            self.assertEqual(pixels(body['runs']), a | b)
        else:
            self.assertTrue(b < a)
            self.assertEqual(len(b), 228)
            self.assertEqual(pixels(body['runs']), a - b)
        inventory = [p for owner in sub['owners'] for p in owner['pathIds']]
        self.assertCountEqual(sub['pathIds'], body['pathIds'] + inventory)
        painted = pixels(mask_union(painted_source_paths(result)))
        self.assertEqual(result['conservation']['inkPixels'], len(painted))
        raw = set.union(*by_id.values())
        self.assertEqual(raw - painted, b if operation == 'subtract' else set())
        return sub

    def test_seen_heh_fragments_leave_final_sukun_with_meem(self):
        self.check_body('page189', 'l15-w1-s3', 10, 5, 'add', [[6], [7, 8], [9], [11], [12]])

    def test_end_relative_lam_hah_exception_keeps_tanwin_fath_on_hah(self):
        self.check_body('page203', 'l7-w8-s1', 0, 4, 'add', [[1, 2], [3], [5], []])

    def test_noon_jeem_fragments_keep_auxiliary_small_kaf_on_kaf(self):
        sub = self.check_body('page219', 'l6-w5-s1', 5, 0, 'add', [[1, 2], [3, 4], [6, 7, 8], [9], [10, 11]])
        self.assertIn('small-kaf-body', [slot['type'] for slot in sub['owners'][-1]['expectedSlots']])

    def test_zah_counter_restores_white_opening_and_final_teh_dots(self):
        self.check_body('page437', 'l1-w7-s2', 2, 3, 'subtract', [[], [4, 5, 6], [7], [8, 9], [10, 11]])

    def test_ambiguous_or_unsupported_body_geometry_keeps_the_existing_assignment(self):
        for scenario in ('two-candidates', 'different-word', 'off-baseline', 'too-far', 'no-surplus', 'typed'):
            with self.subTest(scenario=scenario):
                case = copy.deepcopy(self.cases['page189'])
                word = case['native']['words'][0]
                paths = {p['id']: p for p in case['native']['paths']}
                part, dot = paths['l15-w1-p5'], paths['l15-w1-p6']
                if scenario == 'two-candidates':
                    dot.update({k: part[k] for k in ('bbox', 'runs')})
                elif scenario == 'different-word':
                    word['index'] = 2
                elif scenario == 'off-baseline':
                    word['baseline'] += 20
                elif scenario == 'too-far':
                    part['runs'][::2] = [start + 35 for start in part['runs'][::2]]
                    part['bbox'][0] += 35
                elif scenario == 'no-surplus':
                    # Remove a real mark without moving the body pieces.
                    for ids in (word['pathIds'], word['subwords'][-1]['pathIds']):
                        ids.remove(dot['id'])
                    case['native']['paths'].remove(dot)
                else:
                    part['legacyType'] = 1
                _, sub = self.assign(case, 'l15-w1-s3')
                self.assertNotIn('compoundBody', sub)

    def test_counter_requires_actual_mask_containment_and_preserves_overlapping_ink(self):
        case = copy.deepcopy(self.cases['page437'])
        paths = {p['id']: p for p in case['native']['paths']}
        hole = paths['l1-w7-p3']
        # A distinct source path can still paint the same page pixel.
        point = hole['runs'][0]
        extra = dict(hole, id='independent-ink', runs=[point, 1])
        case['native']['paths'].append(extra)
        result, _ = self.assign(case, 'l1-w7-s2')
        self.assertIn(point, pixels(mask_union(painted_source_paths(result))))
        # Identical bounds, but one pixel escapes the filled body.
        hole['runs'][:0] = [1, 1]
        _, sub = self.assign(case, 'l1-w7-s2')
        self.assertNotIn('compoundBody', sub)


if __name__ == '__main__':
    unittest.main()
