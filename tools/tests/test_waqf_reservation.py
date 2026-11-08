import copy
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
from native_marks import assign_page, consume_with_vertical_quotas, reserve_top_waqf, mask_union
from native_marks import top_waqf_candidate, waqf_candidates, template_match, dilated_signature
from native_marks import segmentation_snapshot


class WaqfReservationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads((Path(__file__).parent/'fixtures/mark-waqf-reservation.json').read_text())
        cls.cases = {c['id']: c for c in cls.fixture['cases']}

    def assign(self, sid):
        c = self.cases[sid]
        result = assign_page(c['native'], c['transcript'], c['geometry'], self.fixture['shapeBank'])
        return result, next(s for s in result['subwords'] if s['id'] == sid)

    def test_page018_meem_and_sukun_return_to_their_own_quotas(self):
        _, sub = self.assign('page018:l13-w7-s2')
        self.assertEqual([o['pathIds'] for o in sub['owners']],
                         [['l13-w7-p4'], ['l13-w7-p6'], ['l13-w7-p8'], ['l13-w7-p7']])
        self.assertEqual(sub['waqfReservation']['selectionMethod'], 'geometric-templates')
        self.assertAlmostEqual(sub['waqfReservation']['clearance'], 41.3076, places=3)

    def test_recognition_corrects_meem_when_neighboring_vowel_is_higher(self):
        _, sub = self.assign('page445:l6-w7-s2')
        self.assertLess(sub['waqfReservation']['clearance'], 0)
        self.assertEqual(sub['owners'][-2]['pathIds'], ['l6-w7-p8'])
        self.assertEqual(sub['owners'][-1]['pathIds'], ['l6-w7-p7'])

    def test_multipart_qaf_and_jeem_use_whole_sign_geometry(self):
        for sid, expected in [('page027:l10-w7-s3', {'l10-w7-p7','l10-w7-p10'}),
                              ('page100:l14-w10-s1', {'l14-w10-p8','l14-w10-p10'}),
                              ('page515:l5-w7-s3', {'l5-w7-p8','l5-w7-p11'})]:
            with self.subTest(subword=sid):
                _, sub = self.assign(sid)
                self.assertEqual(sub['waqfReservation']['status'], 'applied')
                self.assertEqual(set(sub['owners'][-1]['pathIds']), expected)
                self.assertEqual(len(sub['owners'][-1]['groups']), 1)
                self.assertFalse(any(o.get('geometricGrouping', {}).get('crossOwnerGroups') for o in sub['owners']))

    def test_jeem_inner_dot_is_not_replaced_by_second_highest_vowel(self):
        sid = 'page077:l8-w12-s2'; c = self.cases[sid]
        _, sub = self.assign(sid)
        previous = next(s for s in c['beforeSubwords'] if s['id'] == sid)
        self.assertEqual(segmentation_snapshot(sub)['owners'], previous['owners'])
        self.assertEqual(set(sub['owners'][-1]['pathIds']), {'l8-w12-p11','l8-w12-p13'})
        self.assertEqual(sub['waqfReservation']['status'], 'unchanged')
        paths = [p for p in c['native']['paths'] if p['id'] in sub['pathIds'] and p['id'] != sub['bodyPathId']]
        highest_two = sorted(paths, key=lambda p:p['bbox'][1])[:2]
        self.assertEqual({p['id'] for p in highest_two}, {'l8-w12-p11', 'l8-w12-p10'})
        group, reason = top_waqf_candidate(paths, 2)
        self.assertIsNone(reason)
        self.assertEqual({p['id'] for p in group}, {'l8-w12-p11', 'l8-w12-p13'})

    def test_native_flag_wins_even_when_a_letter_mark_is_higher(self):
        sid = 'page238:l8-w7-s1'
        _, sub = self.assign(sid)
        old = next(s for s in self.cases[sid]['beforeSubwords'] if s['id'] == sid)
        self.assertEqual(segmentation_snapshot(sub), old)
        self.assertNotIn('waqfReservation', sub)

    def test_three_dot_signs_recognized_without_splitting_native_paths(self):
        for sid, sign, letter in [('page112:l4-w8-s1', {'l4-w8-p5','l4-w8-p7','l4-w8-p8'}, 'l4-w8-p6'),
                                 ('page114:l10-w2-s4', {'l10-w2-p5','l10-w2-p6'}, 'l10-w2-p8')]:
            with self.subTest(subword=sid):
                result, sub = self.assign(sid)
                self.assertEqual(set(sub['owners'][-1]['pathIds']), sign)
                self.assertIn(letter, sub['owners'][-2]['pathIds'])
                self.assertEqual(sub['owners'][-1]['assignedContours'], 3)
                self.assertFalse(result['assignmentComponents'])
                base, waqf = sub['owners'][-2:]
                self.assertEqual(waqf['role'], 'word-sign')
                self.assertEqual({g['transcriptLink']['attachedLetterId'] for g in waqf['groups']}, {base['id']})

    def test_recognized_sign_repairs_rtl_overflow_before_packed_tanwin(self):
        for sid, sign, tanwin in [('page105:l5-w7-s2', 'l5-w7-p5', 'l5-w7-p4'),
                                 ('page120:l7-w2-s1', 'l7-w2-p9', 'l7-w2-p11')]:
            with self.subTest(subword=sid):
                _, sub = self.assign(sid)
                old = next(s for s in self.cases[sid]['beforeSubwords'] if s['id'] == sid)
                self.assertEqual(old['owners'][-1]['pathIds'], [])
                self.assertIn('contour-overflow', old['owners'][-2]['issues'])
                self.assertEqual(sub['owners'][-1]['pathIds'], [sign])
                self.assertIn(tanwin, sub['owners'][-2]['pathIds'])
                self.assertTrue(sub['waqfReservation']['originalQuotaIssues'])
                self.assertEqual(sub['waqfReservation']['selectionMethod'], 'geometric-templates')
                self.assertFalse(sub['unusedPathIds'] or any(o['issues'] for o in sub['owners']))

    def test_all_fixtures_conserve_pixels_native_types_and_subword_membership(self):
        before = copy.deepcopy(self.fixture)
        for sid, c in self.cases.items():
            result, _ = self.assign(sid)
            paths = {p['id']: p for p in result['paths']}
            old = {s['id']: s for s in c['beforeSubwords']}
            for sub in result['subwords']:
                self.assertEqual(sub['pathIds'], old[sub['id']]['pathIds'])
                self.assertEqual(sub['bodyPathId'], old[sub['id']]['bodyPathId'])
                claims = [p for o in sub['owners'] for p in o['pathIds']]+sub['unusedPathIds']
                self.assertEqual(len(claims), len(set(claims)))
                self.assertEqual(set(claims), set(sub['pathIds'])-{sub['bodyPathId']})
                for owner in sub['owners']:
                    union = mask_union([paths[p] for p in owner['pathIds']])
                    self.assertEqual(mask_union(owner['groups']), union)
                    self.assertEqual(sum(sum(g['runs'][1::2]) for g in owner['groups']), sum(union[1::2]))
            self.assertTrue(result['conservation']['everyPathAccounted'])
        self.assertEqual(self.fixture, before)

    def test_recognition_is_translation_scale_location_and_prediction_independent(self):
        c = self.cases['page018:l13-w7-s2'];raw = c['native'];scale = raw['provenance']['pdfToRaster'][3]
        paths = copy.deepcopy(raw['paths']);geo = copy.deepcopy(c['geometry']);base = raw['words'][0]['baseline']
        original = waqf_candidates(paths, geo, raw['raster']['width'], base, scale)
        for p in paths:
            previous = p['id'];p['id'] = 'arbitrary-'+previous
            x,y,w,h = p['bbox'];p['bbox'] = [x*2+31,y*2-7,w*2,h*2]
            geo[p['id']] = dict(geo[previous], prediction='wrong', reviewedLabel='wrong', templateScores={'damma':0})
        moved = waqf_candidates(paths, geo, raw['raster']['width'], base*2-7, scale*2)
        self.assertEqual([(c['type'], ['arbitrary-'+p for p in c['pathIds']]) for c in original],
                         [(c['type'], c['pathIds']) for c in moved])

    def test_mask_tolerance_never_wraps_pixels_across_rows(self):
        self.assertEqual(dilated_signature(1 << 255).bit_count(), 4)
        self.assertEqual(dilated_signature(1).bit_count(), 4)
        self.assertEqual(dilated_signature(1 << 240).bit_count(), 4)
        self.assertFalse(dilated_signature(1 << 240) & (1 << 239))

    def test_missing_invalid_or_distorted_signature_does_not_match(self):
        c = self.cases['page018:l13-w7-s2'];p = next(p for p in c['native']['paths'] if p['id']=='l13-w7-p7')
        source = {**p, **c['geometry'][p['id']]};scale = c['native']['provenance']['pdfToRaster'][3]
        self.assertIsNotNone(template_match(source, 'meem.waqf', scale))
        for sig in ['0', 'not-hex', '-f', 'f'*65]:
            self.assertIsNone(template_match(dict(source, signature=sig), 'meem.waqf', scale))
        bad = copy.deepcopy(source);bad['bbox'][2] *= 2
        self.assertIsNone(template_match(bad, 'meem.waqf', scale))
        self.assertIsNone(template_match(source, 'meem.waqf', float('nan')))

    def test_ambiguous_template_candidates_do_not_force_a_reservation(self):
        c = self.cases['page018:l13-w7-s2'];old = copy.deepcopy(c['beforeSubwords'][-1])
        paths = [p for p in c['native']['paths'] if p['id'] in old['pathIds'] and p['id']!=old['bodyPathId']]
        # Supply two real geometric matches, without monkey-patching Python.
        paths = copy.deepcopy(paths); geometry = copy.deepcopy(c['geometry'])
        sign = next(p for p in paths if p['id'] == 'l13-w7-p7')
        other = next(p for p in paths if p['id'] == 'l13-w7-p8')
        other['bbox'] = copy.deepcopy(sign['bbox'])
        geometry[other['id']] = copy.deepcopy(geometry[sign['id']])
        result = reserve_top_waqf(paths, old['owners'], old, old['baseline'],
            scale=c['native']['provenance']['pdfToRaster'][3],
            geometry=geometry, width=c['native']['raster']['width'])
        self.assertEqual(result['waqfReservation']['reason'], 'multiple-waqf-template-candidates')
        self.assertEqual(result['owners'], c['beforeSubwords'][-1]['owners'])

    def test_height_fallback_rejects_near_ties_incomplete_and_ambiguous_groups(self):
        buckets = [dict(id='letter', role='letter', expectedContours=1, expectedLowerContours=0,
                        expectedSlots=[dict(type='sukun')]),
                   dict(id='sign', role='word-sign', expectedContours=1, expectedLowerContours=0,
                        expectedSlots=[dict(type='waqf-meem')])]
        paths = [dict(id='top', bbox=[20,10,5,5], contourCount=1),
                 dict(id='second', bbox=[0,12,5,5], contourCount=1)]
        original = consume_with_vertical_quotas(paths,buckets,100,[0,95,30,10],4)
        result = reserve_top_waqf(paths,buckets,copy.deepcopy(original),100,[0,95,30,10],4)
        self.assertEqual(result['waqfReservation']['reason'], 'top-edge-margin-ambiguous')
        self.assertEqual(result['owners'], original['owners'])
        original['unusedPathIds'] = ['extra']
        result = reserve_top_waqf(paths,buckets,original,100)
        self.assertEqual(result['waqfReservation']['reason'], 'original-contour-quota-difference')
        group, reason = top_waqf_candidate([dict(id=str(i), bbox=[i,10+i,10,10], contourCount=1) for i in range(3)],2)
        self.assertIsNone(group)
        self.assertEqual(reason, 'ambiguous-spatial-group')


if __name__ == '__main__':
    unittest.main()
