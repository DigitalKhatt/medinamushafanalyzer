import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
import native_marks as sequence
from classify_assigned_marks import classify_regions
from mark_assignment_collection import aggregate
from audit_mark_assignments import summarize


def path(name, x, width=10, contours=1, legacy_type=0):
    return dict(id=name, bbox=[x, 10, width, 2], contourCount=contours, legacyType=legacy_type,
                runs=[1000+x, width, 1100+x, width])


def bucket(name, count):
    return dict(id=name, expectedContours=count)


class SequentialQuotaTests(unittest.TestCase):
    def test_right_edge_not_left_edge_or_center_controls_order(self):
        result = sequence.consume([path('wide', 0, 80), path('narrow', 65, 4)],
                                  [bucket('first', 1), bucket('second', 1)])
        self.assertEqual([o['pathIds'] for o in result['owners']], [['wide'], ['narrow']])

    def test_fill_whole_letter_before_advancing(self):
        paths = [path('last', 10), path('first', 60), path('middle', 40)]
        result = sequence.consume(paths, [bucket('a', 0), bucket('b', 2), bucket('c', 1)])
        self.assertEqual([o['pathIds'] for o in result['owners']], [[], ['first', 'middle'], ['last']])
        self.assertFalse(result['unusedPathIds'])

    def test_native_overflow_swap_then_advance(self):
        result = sequence.consume([path('packed', 60, contours=2), path('single', 30)],
                                  [bucket('a', 1), bucket('b', 2)])
        self.assertEqual([o['pathIds'] for o in result['owners']], [['single'], ['packed']])
        self.assertEqual(len(result['swaps']), 1)
        self.assertTrue(all(not o['issues'] for o in result['owners']))

    def test_overflow_shortfall_and_unused_ink_stay_explicit(self):
        result = sequence.consume([path('too-big', 60, contours=2)], [bucket('a', 1), bucket('b', 1)])
        self.assertEqual([o['issues'] for o in result['owners']], [['contour-overflow'], ['contour-shortfall']])
        self.assertEqual(result['owners'][0]['pathIds'], ['too-big'])
        result = sequence.consume([path('leftover', 60)], [bucket('a', 0)])
        self.assertEqual(result['unusedPathIds'], ['leftover'])

    def test_waqf_sorts_last_like_native_segmentation(self):
        result = sequence.consume([path('stop', 90, legacy_type=1), path('letter-mark', 10)],
                                  [bucket('letter', 1), bucket('sign', 1)])
        self.assertEqual(result['initialOrder'], ['letter-mark', 'stop'])

    def test_predictions_and_templates_cannot_select_owners(self):
        paths = [path('right', 60), path('left', 10)]
        for p in paths:
            p['templateScores'] = {'damma': 0, 'onedot': 0}
            p['prediction'] = 'kasra'
        expected = [dict(bucket('first', 1), expectedInventory={'fatha': 1}),
                    dict(bucket('second', 1), expectedInventory={'kasra': 1})]
        before = sequence.consume(paths, expected)
        paths[0]['prediction'] = 'fatha'
        paths[0]['templateScores'] = {'damma': 1000}
        expected[0]['expectedInventory'] = {'maddah': 1}
        after = sequence.consume(paths[::-1], expected)
        self.assertEqual([o['pathIds'] for o in before['owners']], [o['pathIds'] for o in after['owners']])

    def test_invalid_contours_rejected_even_if_unused(self):
        with self.assertRaisesRegex(ValueError, 'positive'):
            sequence.consume([path('invalid', 2, contours=0)], [])


class NativeFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads((Path(__file__).parent/'fixtures/mark-sequence-page003.json').read_text())
        cls.assignment = sequence.assign_page(cls.fixture['native'], cls.fixture['transcript'])

    def test_kaf_feh_reh_consumes_one_two_one_contours(self):
        sub = next(s for s in self.assignment['subwords'] if s['id'] == 'page003:l1-w3-s1')
        self.assertEqual([o['text'] for o in sub['owners']], ['كَ', 'فَ', 'رُ'])
        self.assertEqual([o['pathIds'] for o in sub['owners']],
                         [['l1-w3-p1'], ['l1-w3-p2', 'l1-w3-p3'], ['l1-w3-p4']])
        self.assertEqual([o['expectedContours'] for o in sub['owners']], [1, 2, 1])
        self.assertFalse(sub['unusedPathIds'])

    def test_multipart_grouping_stays_inside_frozen_owner_and_conserves_ink(self):
        merged = []
        for sub in self.assignment['subwords']:
            for owner in sub['owners']:
                self.assertFalse(owner['issues'])
                self.assertFalse(owner['groupingIssues'])
                for group in owner['groups']:
                    self.assertTrue(set(group['pathIds']) <= set(owner['pathIds']))
                    if len(group['pathIds']) > 1:
                        merged.append(group)
        self.assertGreaterEqual(len(merged), 2)
        self.assertTrue(self.assignment['conservation']['everyPathAccounted'])
        self.assertTrue(self.assignment['conservation']['masksUnchanged'])

    def test_compact_native_hamza_is_retained_in_trace_and_returned_to_marks(self):
        sub = next(s for s in self.assignment['subwords'] if s['id'] == 'page003:l7-w8-s2')
        self.assertEqual(sub['nativeBodyPathId'], 'l7-w8-p3')
        self.assertEqual(sub['bodyPathId'], 'l7-w8-p4')
        self.assertIn('l7-w8-p3', sub['owners'][0]['pathIds'])

    def test_equivalent_alef_hamza_maddah_requires_fatha_and_hamza(self):
        word = next(w for w in self.fixture['transcript']['words'] if 'أٓ' in w['text'])
        owner = next(o for o in sequence.expected_letters(word) if 'أٓ' in o['text'])
        self.assertEqual(owner['expectedInventory'], {'hamza-above': 1, 'fatha': 1})

    def test_templates_do_not_choose_owners_and_predictions_are_not_read(self):
        doc = copy.deepcopy(self.fixture['native'])
        for p in doc['paths']:
            del p['templateScores']
            p['prediction'] = 'WRONG'
        result = sequence.assign_page(doc, self.fixture['transcript'])
        self.assertEqual([[o['pathIds'] for o in s['owners']] for s in result['subwords']],
                         [[o['pathIds'] for o in s['owners']] for s in self.assignment['subwords']])
        doc = copy.deepcopy(self.fixture['native'])
        for p in doc['paths']:
            p['prediction'] = 'WRONG'
        self.assertEqual(sequence.assign_page(doc, self.fixture['transcript']), self.assignment)

    def test_old_transcript_without_native_counts_is_rejected(self):
        transcript = copy.deepcopy(self.fixture['transcript'])
        del transcript['words'][0]['characters'][0]['nativeContourCount']
        with self.assertRaisesRegex(ValueError, 'Re-export'):
            sequence.assign_page(self.fixture['native'], transcript)

    def test_duplicate_source_membership_is_rejected(self):
        doc = copy.deepcopy(self.fixture['native'])
        doc['words'][0]['subwords'][1]['pathIds'].append(doc['words'][0]['subwords'][0]['pathIds'][0])
        with self.assertRaisesRegex(ValueError, 'duplicate subword'):
            sequence.assign_page(doc, self.fixture['transcript'])


class NativeVerticalQuotaTests(unittest.TestCase):
    def setUp(self):
        self.paths = [path('below-right', 60), path('above-middle', 40), path('above-left', 20)]
        self.paths[0]['bbox'][1] = 30
        self.buckets = [dict(bucket('lam', 1), expectedLowerContours=0),
                        dict(bucket('hah', 1), expectedLowerContours=1),
                        dict(bucket('seen', 1), expectedLowerContours=0)]
        self.kwargs = dict(baseline=20, body_bbox=[0, 0, 100, 40], scale=4)

    def assert_original(self, result):
        self.assertEqual({k: v for k, v in result.items() if k != 'verticalAssignment'},
                         sequence.consume(self.paths, self.buckets))
        self.assertEqual(result['verticalAssignment']['status'], 'fallback')

    def test_clear_native_lower_quota_corrects_lam_hah(self):
        result = sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs)
        self.assertEqual([o['pathIds'] for o in result['owners']],
                         [['above-middle'], ['below-right'], ['above-left']])
        self.assertEqual(result['verticalAssignment']['status'], 'applied')

    def test_ambiguous_path_with_incompatible_owner_quota_keeps_original_assignment(self):
        for y in (17, 19, 20, 21, 22):
            self.paths[0]['bbox'][1] = y
            result = sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs)
            self.assert_original(result)
            self.assertEqual(result['verticalAssignment']['reason'], 'ink-intersects-native-baseline-band')

    def test_wrong_vertical_total_or_body_off_baseline_keeps_original(self):
        self.paths[0]['bbox'][1] = 5
        self.assert_original(sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs))
        self.paths[0]['bbox'][1] = 30
        self.kwargs['body_bbox'] = [0, 0, 100, 10]
        self.assert_original(sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs))

    def test_unused_ink_or_missing_shared_quota_keeps_original(self):
        self.paths.append(path('extra', 80))
        self.assert_original(sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs))
        self.paths.pop()
        del self.buckets[0]['expectedLowerContours']
        self.assert_original(sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs))

    def test_unsplittable_packed_path_keeps_original(self):
        self.paths = [path('above-packed', 60, contours=2), path('below-a', 40), path('below-b', 20)]
        for p in self.paths[1:]: p['bbox'][1] = 30
        self.buckets = [dict(bucket('a', 2), expectedLowerContours=1), dict(bucket('b', 2), expectedLowerContours=1)]
        result = sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs)
        self.assert_original(result)
        self.assertEqual(result['verticalAssignment']['reason'], 'vertical-stream-contour-overflow-or-shortfall')

    def test_assignment_is_translation_scale_and_prediction_independent(self):
        expected = sequence.consume_with_vertical_quotas(self.paths, self.buckets, **self.kwargs)
        for p in self.paths:
            x,y,w,h = p['bbox']; p['bbox'] = [2*x+7, 2*y+11, 2*w, 2*h]
            p['templateScores'] = {'damma': 0}; p['prediction'] = 'WRONG'
        result = sequence.consume_with_vertical_quotas(self.paths, self.buckets, baseline=51,
                                                      body_bbox=[7,11,200,80], scale=8)
        self.assertEqual([o['pathIds'] for o in result['owners']], [o['pathIds'] for o in expected['owners']])

    def test_page031_real_source_paths_use_exported_native_lower_counts(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/mark-vertical-page031.json').read_text())
        result = sequence.assign_page(fixture['native'], fixture['transcript'])
        sub = next(s for s in result['subwords'] if s['id'] == 'page031:l15-w7-s2')
        self.assertEqual([o['expectedLowerContours'] for o in sub['owners']], [0,1,0,0])
        self.assertEqual([o['pathIds'] for o in sub['owners']], [['l15-w7-p4'],['l15-w7-p3'],['l15-w7-p5'],[]])
        self.assertTrue(result['conservation']['everyPathAccounted'])


class NativeWaqfFlagTests(unittest.TestCase):
    def test_restored_stop_sign_and_ambiguous_ink_keep_their_correct_owners(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/mark-waqf-page002.json').read_text())
        assignment = sequence.assign_page(fixture['native'], fixture['transcript'])
        sub = next(s for s in assignment['subwords'] if s['id'] == 'page002:l7-w5-s2')
        self.assertEqual(sub['verticalAssignment']['status'], 'unchanged')
        self.assertEqual(sub['verticalAssignment']['ambiguousReservation']['owners'][0]['pathIds'], ['l7-w5-p5'])
        owners = {o['text']: o for o in sub['owners']}
        self.assertEqual(owners['مْ']['pathIds'], ['l7-w5-p9'])
        self.assertEqual(owners['ۖ']['pathIds'], ['l7-w5-p8'])
        self.assertEqual(owners['بِّ']['pathIds'], ['l7-w5-p3','l7-w5-p5','l7-w5-p6'])
        self.assertEqual(owners['هِ']['pathIds'], ['l7-w5-p7'])
        self.assertEqual(sub['initialOrder'][-1], 'l7-w5-p8')
        self.assertTrue(assignment['conservation']['everyPathAccounted'])


class IndependentValidationTests(unittest.TestCase):
    def setUp(self):
        self.assignment = dict(groups=[{'id': 'a:g1'}, {'id': 'b:g1'}], subwords=[dict(id='s', unusedPathIds=[], owners=[
            dict(id='a', groups=[{'id': 'a:g1'}], expectedInventory={'fatha': 1},
                 expectedContours=1, assignedContours=1, groupingIssues=[]),
            dict(id='b', groups=[{'id': 'b:g1'}], expectedInventory={'kasra': 1},
                 expectedContours=1, assignedContours=1, groupingIssues=[])])])

    def test_matching_subword_total_does_not_hide_swapped_identities(self):
        before = sequence.digest_json(self.assignment)
        predictions = [dict(id='a:g1', kind='mark', type='kasra'), dict(id='b:g1', kind='mark', type='fatha')]
        result = sequence.compare_predictions(self.assignment, predictions)
        self.assertFalse(result[0]['matches'])
        self.assertEqual(result[0]['owners'][0]['missing'], {'fatha': 1})
        self.assertEqual(result[0]['owners'][0]['extra'], {'kasra': 1})
        self.assertEqual(sequence.digest_json(self.assignment), before)

    def test_missing_and_extra_are_empty_only_after_identity_check(self):
        predictions = [dict(id='a:g1', kind='mark', type='fatha'), dict(id='b:g1', kind='mark', type='kasra')]
        result = sequence.compare_predictions(self.assignment, predictions)
        self.assertTrue(result[0]['matches'])
        self.assertTrue(all(not o['missing'] and not o['extra'] for o in result[0]['owners']))

    def test_missing_or_duplicate_predictions_are_rejected(self):
        for records in [[], [dict(id='a:g1'), dict(id='a:g1')]]:
            with self.assertRaisesRegex(ValueError, 'exactly once'):
                sequence.compare_predictions(self.assignment, records)

    def test_connection_ink_is_not_counted_as_auxiliary_body(self):
        self.assignment['subwords'][0]['owners'][0]['expectedInventory'] = {'body-ink': 1}
        predictions = [dict(id='a:g1', kind='kashida', type='unknown'), dict(id='b:g1', kind='mark', type='kasra')]
        row = sequence.compare_predictions(self.assignment, predictions)[0]['owners'][0]
        self.assertEqual(row['missing'], {'body-ink': 1})
        self.assertEqual(row['extra'], {'kashida': 1})

    def test_classifier_api_rejects_expected_labels_before_importing_model(self):
        with self.assertRaisesRegex(ValueError, 'only opaque ID'):
            classify_regions([dict(id='region', runs=[], bbox=[], baseline=0, expectedType='fatha')],
                             [], {}, '/not-opened', '/not-opened')

    def test_classifier_api_rejects_ownership_in_visual_context(self):
        with self.assertRaisesRegex(ValueError, 'source ink only'):
            classify_regions([], [dict(runs=[], owner='alef')], {}, '/not-opened', '/not-opened')


class AllPageInputTests(unittest.TestCase):
    def test_carrier_owns_its_hamza_and_vowel(self):
        chars = [('ب', 'initial', 2), ('ِ', 'unknown', 1), ('ـ', 'medial', 1),
                 ('ٔ', 'unknown', 1), ('ُ', 'unknown', 1), ('و', 'final', 1)]
        word = dict(id='w', text=''.join(c[0] for c in chars),
                    characters=[dict(text=c, position=p, nativeContourCount=n) for c, p, n in chars])
        owners = sequence.expected_letters(word)
        self.assertEqual([o['text'] for o in owners], ['بِ', 'ـُٔ', 'و'])
        self.assertEqual(owners[0]['expectedInventory'], {'single-dot': 1, 'kasra': 1})
        self.assertEqual(owners[1]['expectedInventory'], {'hamza-above': 1, 'damma': 1})
        self.assertEqual(owners[1]['expectedContours'], 2)
        self.assertTrue(owners[1]['carrier'])

    def test_detached_small_waw_joins_previous_native_body_without_mutation(self):
        native = [dict(id='s1', text='هُ', pathIds=['body', 'damma']),
                  dict(id='s2', text='ۥ', pathIds=['small-waw'])]
        before = copy.deepcopy(native)
        normalized = sequence.normalize_detached_marks(native)
        self.assertEqual(native, before)
        self.assertEqual(normalized[0]['text'], 'هُۥ')
        self.assertEqual(normalized[0]['pathIds'], ['body', 'damma', 'small-waw'])
        self.assertEqual(normalized[0]['nativeSubwordIds'], ['s1', 's2'])

    def test_hizb_and_sajda_are_sign_quotas_without_letter_bodies(self):
        for char, count, kind in [('۞', 8, 'hizb-sign'), ('۩', 2, 'quranic-sign')]:
            word = dict(id='w', text=char, characters=[dict(text=char, position='unknown', nativeContourCount=count)])
            owner, = sequence.expected_letters(word)
            self.assertEqual(owner['role'], 'word-sign')
            self.assertEqual(owner['expectedContours'], count)
            self.assertEqual(owner['expectedInventory'], {kind: 1})

    def test_eight_hizb_contours_form_one_semantic_group_without_identity_matching(self):
        paths = {str(i): path(str(i), i*5, width=2) for i in range(8)}
        owner = dict(id='sign', pathIds=list(paths), expectedMarkCount=1,
                     expectedSlots=[dict(type='hizb-sign', capacity=8)])
        grouped = sequence.group_owned_ink(owner, paths, 100, 30)
        self.assertFalse(grouped['groupingIssues'])
        self.assertEqual(len(grouped['groups']), 1)
        self.assertEqual(grouped['groups'][0]['contours'], 8)
        self.assertEqual(grouped['groups'][0]['runs'], sequence.mask_union(list(paths.values())))

    def test_batch_failure_never_counts_as_a_matching_page(self):
        completed = dict(pageId='page001', summary=dict(subwords=2, matchingSubwords=1, differingSubwords=1,
                         missing={'fatha': 1}, extra={'kasra': 1}),
                         conservation=dict(inkPixels=30, everyPathAccounted=True))
        result = aggregate(['page001', 'page002'], [completed], [dict(pageId='page002', error='Missing input')])
        self.assertEqual(result['summary']['completedPages'], 1)
        self.assertEqual(result['summary']['failedPages'], 1)
        self.assertEqual(result['summary']['matchingSubwords'], 1)
        self.assertEqual(result['summary']['missing'], {'fatha': 1})

    def test_batch_requires_explicit_accounting_for_every_requested_page(self):
        with self.assertRaisesRegex(ValueError, 'every requested page'):
            aggregate(['page001', 'page002'], [], [dict(pageId='page002', error='bad')])

    def test_unused_ink_is_not_reported_as_a_letter_identity_swap(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/mark-sequence-page003.json').read_text())
        bank = json.loads((Path(__file__).resolve().parents[2]/'templates/mark-shapes.json').read_bytes())
        assignment = sequence.assign_page(fixture['native'], fixture['transcript'], shape_bank=bank['prototypes'])
        assignment['subwords'][0]['unusedPathIds'] = ['unused']
        predictions = []
        for group in assignment['groups']:
            expected = group['transcriptLink']['expectedType']
            predictions.append(dict(id=group['id'], kind='body' if expected == 'small-kaf-body' else 'mark',
                                    type=expected))
        comparisons = sequence.compare_predictions(assignment, predictions)
        summary = summarize(assignment, comparisons)
        self.assertEqual(summary['subwordsWithMatchingAggregateButLetterDifferences'], 0)
        self.assertEqual(summary['structureOnlyDifferingSubwords'], 1)
        self.assertEqual(summary['identityDifferingSubwords'], 0)


if __name__ == '__main__':
    unittest.main()
