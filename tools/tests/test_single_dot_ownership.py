import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from native_marks import assign_page, mask_union


class SingleDotOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = json.loads((ROOT/'tools/tests/fixtures/mark-single-dot-vowels.json').read_text())
        cls.bank = json.loads((ROOT/'templates/mark-shapes.json').read_text())['prototypes']

    def assign(self, case):
        return assign_page(case['native'], case['transcript'], shape_bank=self.bank)

    def test_upper_dot_and_two_fathas_keep_all_original_group_pixels(self):
        case = copy.deepcopy(self.cases['page303'])
        result = self.assign(case)
        sub = result['subwords'][0]
        self.assertEqual([o['pathIds'] for o in sub['owners'][:2]],
                         [['l5-w10-p1-stroke-2', 'l5-w10-p2'], ['l5-w10-p1-stroke-1']])
        match = sub['singleDotTemplate']['matches']['l5-w10-p2']
        self.assertEqual((match['previousOwnerId'], match['ownerId']),
                         (sub['owners'][1]['id'], sub['owners'][0]['id']))
        source = next(p for p in case['native']['paths'] if p['id'] == 'l5-w10-p1')
        self.assertEqual(mask_union(result['assignmentComponents']), mask_union([source]))
        next(p for p in case['native']['paths'] if p['id'] == 'l5-w10-p2')['templateScores'].clear()
        previous = self.assign(case)
        self.assertCountEqual([g['runs'] for g in previous['groups']], [g['runs'] for g in result['groups']])
        self.assertFalse(previous['assignmentComponents'])

    def test_lower_dot_goes_to_beh_and_each_damma_has_its_own_letter(self):
        sub = self.assign(self.cases['page521'])['subwords'][1]
        hah, beh = sub['owners'][1:3]
        self.assertEqual(hah['pathIds'], ['l1-w3-p4-damma-1'])
        self.assertEqual(beh['pathIds'], ['l1-w3-p5', 'l1-w3-p4-damma-2'])
        self.assertEqual(sub['singleDotTemplate']['matches']['l1-w3-p5']['side'], 'below')

    def test_missing_weak_extra_and_wrong_side_dot_evidence_preserve_packed_ownership(self):
        for evidence in ('missing', 'at-threshold', 'extra', 'ambiguous', 'wrong-side'):
            with self.subTest(evidence=evidence):
                case = copy.deepcopy(self.cases['page303'])
                native = case['native']
                paths = {p['id']: p for p in native['paths']}
                dot = paths['l5-w10-p2']
                dot['prediction'] = 'single-dot'
                if evidence == 'missing':
                    del dot['templateScores']
                elif evidence == 'at-threshold':
                    dot['templateScores']['onedot'] = 0.1
                elif evidence == 'extra':
                    other = paths['l5-w10-p3']
                    other['bbox'] = [*other['bbox'][:2], *dot['bbox'][2:]]
                    other['templateScores']['onedot'] = 0
                else:
                    x, y, width, height = dot['bbox']
                    native['words'][0]['baseline'] = y+height/2 if evidence == 'ambiguous' else y-5
                sub = self.assign(case)['subwords'][0]
                self.assertNotIn('singleDotTemplate', sub)
                self.assertEqual(sub['owners'][0]['pathIds'], ['l5-w10-p1'])

    def test_explicit_tanwin_slot_keeps_its_packed_strokes(self):
        # Same five contours, but a transcript with a two-contour tanwin slot.
        case = copy.deepcopy(self.cases['page303'])
        word = case['transcript']['words'][0]
        chars = word['characters']
        chars[0].update(text='ل', nativeContourCount=1)
        chars[2].update(text='ف', nativeContourCount=2)
        chars[3].update(text='ً', nativeContourCount=2)
        del chars[5]
        word['text'] = ''.join(c['text'] for c in chars)
        source_word = case['native']['words'][0]
        source_word['text'] = word['text']
        source_word['subwords'][0]['text'] = word['text'][:-1]
        result = self.assign(case)
        self.assertFalse(result['assignmentComponents'])
        self.assertNotIn('singleDotTemplate', result['subwords'][0])


if __name__ == '__main__':
    unittest.main()
