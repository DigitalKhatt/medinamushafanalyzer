import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from native_marks import assign_page
from mark_assignment_collection import collect_page
from mark_validation import compare_predictions, digest_json


def fixture(name):
    return json.loads((ROOT / 'tools/tests/fixtures' / f'{name}.json').read_bytes())


def observations(assignment, overrides=None):
    """Controlled classifier labels to test validation independently of model weights."""
    overrides = overrides or {}
    result = []
    for group in assignment['groups']:
        kind = overrides.get(group['id'], group['transcriptLink'].get('expectedType'))
        if kind is None:
            raise ValueError('Specify a test observation for ambiguous ink')
        result.append(dict(id=group['id'], kind='body' if kind == 'small-kaf-body' else 'mark', type=kind))
    return result


def subword(records, sid):
    return next(s for s in records if s['id'] == sid)


class MarkCharacterLinksTests(unittest.TestCase):
    cases = fixture('mark-remaining-ownership')
    bank = json.loads((ROOT / 'templates/mark-shapes.json').read_bytes())['prototypes']
    sid = 'page579:l9-w9-s2'

    @classmethod
    def assign(cls, case):
        return assign_page(case['native'], case['transcript'], shape_bank=cls.bank)

    @classmethod
    def setUpClass(cls):
        cls.page579 = cls.assign(cls.cases[cls.sid])

    def test_kasra_and_intrinsic_dot_link_to_different_characters_of_the_same_letter(self):
        sub = subword(self.page579['subwords'], self.sid)
        owner = next(o for o in sub['owners'] if o['text'] == 'بِ')
        self.assertEqual([s['characterOffset'] for s in owner['expectedSlots']], [5, 6])
        groups = {g['id']: g for g in self.page579['groups']}
        paths_and_links = []
        for g in owner['groups']:
            self.assertEqual(g, groups[g['id']])
            link = g['transcriptLink']
            self.assertEqual((link['baseCharacter'], link['baseOffset']), ('ب', 5))
            paths_and_links.append((g['pathIds'][0], link['characterId'], link['sourceCharacter'],
                                    link['codePoint'], link['expectedType']))
        self.assertEqual(paths_and_links, [
            ('l9-w9-p6', 'page579:l9-w9:c6', 'ِ', 'U+0650', 'kasra'),
            ('l9-w9-p8', 'page579:l9-w9:c5', 'ب', 'U+0628', 'single-dot')])
        self.assertFalse(self.page579['policy']['classifierUsedForAssignment'])

    def test_permuted_classifier_labels_keep_inventory_but_fail_both_character_links(self):
        owner = next(o for o in subword(self.page579['subwords'], self.sid)['owners'] if o['text'] == 'بِ')
        g1, g2 = owner['groups']
        predictions = observations(self.page579, {g1['id']: 'single-dot', g2['id']: 'kasra'})
        before = digest_json([self.page579, predictions])
        comparisons = compare_predictions(self.page579, predictions)
        sub = subword(comparisons, self.sid)
        row = subword(sub['owners'], owner['id'])
        self.assertTrue(sub['inventoryAndStructureMatch'] and row['matches'])
        self.assertFalse(sub['matches'] or sub['characterLinksMatch'])
        self.assertEqual([p['characterMatch'] for p in row['predictions']], ['different', 'different'])
        self.assertEqual([p['transcriptCharacterId'] for p in row['predictions']],
                         ['page579:l9-w9:c6', 'page579:l9-w9:c5'])
        report = dict(self.page579, validation=comparisons, summary={}, assignmentSha256='test')
        diff = subword(collect_page(report, 'test')['differences'], self.sid)
        self.assertEqual(diff['missing'], {})
        self.assertEqual(diff['extra'], {})
        self.assertEqual([i['groupId'] for i in diff['owners'][0]['characterIssues']], [g1['id'], g2['id']])
        self.assertTrue(diff['characterLinkDifference'])
        self.assertEqual(digest_json([self.page579, predictions]), before)

    def test_ambiguous_shapes_keep_candidate_offsets_even_when_classifier_labels_fit(self):
        case = copy.deepcopy(self.cases['page536:l14-w8-s1'])
        for path in case['native']['paths']:
            path['templateScores'] = {}
        a = assign_page(case['native'], case['transcript'], shape_bank={})
        owner = subword(a['subwords'], 'page536:l14-w8-s1')['owners'][0]
        first, second = owner['groups']
        for group in (first, second):
            link = group['transcriptLink']
            self.assertEqual(link['status'], 'ambiguous')
            self.assertEqual(link['candidateOffsets'], [0, 1])
            self.assertNotIn('characterId', link)
        labels = observations(a, {first['id']: 'fatha', second['id']: 'single-dot'})
        row = subword(compare_predictions(a, labels), 'page536:l14-w8-s1')['owners'][0]
        self.assertTrue(row['matches'])
        self.assertFalse(row['characterLinksMatch'])
        self.assertEqual(row['charactersWithoutInk'], [])
        self.assertTrue(all(p['characterMatch'] == 'ambiguous' and 'transcriptCharacterId' not in p
                            for p in row['predictions']))

    def test_source_maddah_keeps_its_own_offset_when_its_expected_ink_is_fatha(self):
        a = self.assign(fixture('mark-sequence-page003'))
        group = next(g for g in a['groups'] if g['pathIds'] == ['l4-w6-p5'])
        link = group['transcriptLink']
        self.assertEqual((link['characterId'], link['sourceCharacter'], link['expectedType'], link['equivalence']),
                         ('page003:l4-w6:c4', 'ٓ', 'fatha', 'أٓ ≡ ـَٔا'))

    def test_word_sign_is_its_own_character_and_has_no_letter_owner(self):
        a = self.assign(fixture('mark-waqf-page002'))
        sign = next(o for sub in a['subwords'] for o in sub['owners'] if o['role'] == 'word-sign')
        link = sign['groups'][0]['transcriptLink']
        self.assertEqual((link['sourceCharacter'], link['characterId'], link['expectedType'],
                          link['baseCharacter'], link['baseOffset']),
                         ('ۖ', 'page002:l7-w5:c10', 'waqf-sad', None, None))

    def test_contextual_hamza_realization_still_points_to_the_original_yeh(self):
        sid = 'page560:l11-w4-s1'
        a = self.assign(self.cases[sid])
        owner = next(o for o in subword(a['subwords'], sid)['owners'] if o['text'] == 'ئِ')
        hamza = next(g for g in owner['groups'] if g['transcriptLink']['expectedType'] == 'hamza-above')
        predictions = observations(a, {hamza['id']: 'hamza-below'})
        row = subword(subword(compare_predictions(a, predictions), sid)['owners'], owner['id'])
        p = subword(row['predictions'], hamza['id'])
        self.assertTrue(row['characterLinksMatch'])
        self.assertEqual((p['type'], p['characterMatch'], p['transcriptCharacterId']),
                         ('hamza-below', 'accepted-form', 'page560:l11-w4:c4'))

    def test_fused_alef_hamza_records_the_character_without_a_separate_mask(self):
        a = self.assign(fixture('mark-fused-hamza-page324'))
        sid = 'page324:l4-w7-s1'
        group, = subword(a['subwords'], sid)['owners'][0]['groups']
        self.assertEqual((group['pathIds'], group['transcriptLink']['characterId']), (['l4-w7-p1'], 'page324:l4-w7:c1'))
        row = subword(compare_predictions(a, observations(a)), sid)['owners'][0]
        self.assertEqual(row['charactersWithoutInk'], [0])
        self.assertEqual(row['missing'], {'hamza-above': 1})

    def test_validation_rejects_duplicate_links_and_bad_source_text(self):
        predictions = observations(self.page579)
        a = copy.deepcopy(self.page579)
        owner = next(o for o in subword(a['subwords'], self.sid)['owners'] if o['text'] == 'بِ')
        first, second = owner['groups']
        second['transcriptLink'] = first['transcriptLink']
        with self.assertRaisesRegex(ValueError, 'duplicate transcript character'):
            compare_predictions(a, predictions)
        a = copy.deepcopy(self.page579)
        word = next(w for w in a['words'] if w['id'] == 'page579:l9-w9')
        word['text'] = word['text'].replace('ِ', 'َ')
        with self.assertRaisesRegex(ValueError, 'differs from transcript text'):
            compare_predictions(a, predictions)

    def test_legacy_exports_still_provide_the_original_inventory_comparison(self):
        predictions = observations(self.page579)
        a = copy.deepcopy(self.page579)
        a['schemaVersion'] = 4
        for g in a['groups']:
            del g['transcriptLink']
        for s in a['subwords']:
            for o in s['owners']:
                for g in o['groups']:
                    del g['transcriptLink']
        rows = compare_predictions(a, predictions)
        self.assertTrue(all(s['matches'] for s in rows))
        self.assertNotIn('characterLinksMatch', rows[0])


if __name__ == '__main__':
    unittest.main()
