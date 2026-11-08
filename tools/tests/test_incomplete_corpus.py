import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from letter_audit.report_incomplete_corpus import analyse_page, body_letters, group_id, group_key


class IncompleteCorpusTests(unittest.TestCase):
    def cache(self):
        return {
            'page': 3,
            'templateConfig': {'version': 2, 'initial': [], 'medial': [], 'final': [],
                               'kashidaDefaults': {'trackingEnabled': False}},
            'sourceWords': [{'line': 1, 'word': 1, 'text': 'بِمْ', 'subwords': 1, 'inTranscript': True}],
            'sourceLines': [{'line': 1, 'sourceWords': 1, 'transcriptWords': 1}],
            'subwords': [{'line': 1, 'word': 1, 'subword': 1, 'text': 'بِمْ',
                         'complete': False, 'bodies': [{}, {}], 'letters': [
                             {'letterIndex': 1, 'character': 'ب', 'position': 'initial',
                              'status': 'no-match', 'candidates': ['beh-init']},
                             {'letterIndex': 2, 'character': 'م', 'position': 'final',
                              'status': 'blocked', 'candidates': ['meem-counter-fina']}]}],
        }

    def test_multiple_partial_subwords_count_as_one_word(self):
        cache = self.cache()
        second = copy.deepcopy(cache['subwords'][0])
        second['subword'] = 2
        cache['subwords'].append(second)
        result = analyse_page(cache)
        self.assertEqual(result['summary']['incompleteWords'], 1)
        self.assertEqual(result['summary']['incompleteConnectedSubwords'], 2)
        self.assertEqual(len({item['id'] for item in result['items']}), 2)

    def test_blocked_letter_is_separate_from_first_failure(self):
        cache = self.cache()
        item = analyse_page(cache)['items'][0]
        self.assertEqual(item['primaryGroup'], group_id(group_key(cache['subwords'][0]['letters'][0])))
        self.assertEqual([l['status'] for l in item['direct']], ['no-match'])
        self.assertEqual([l['status'] for l in item['blocked']], ['blocked'])

    def test_missing_diagnostics_are_visible_and_not_complete(self):
        cache = self.cache()
        cache['subwords'][0].update(complete=True, letters=[])
        result = analyse_page(cache)
        self.assertEqual(result['items'][0]['reason'], 'missing-diagnostics')
        self.assertEqual(result['summary']['reportedCompleteButInconsistent'], 1)
        self.assertEqual(result['summary']['incompleteWords'], 1)

    def test_provisional_remainder_is_still_incomplete(self):
        cache = self.cache()
        sub = cache['subwords'][0]
        sub.update(text='فِي', provisional=True)
        sub['letters'][0].update(character='ف', status='remainder', candidates=['feh-init'])
        sub['letters'][1].update(character='ي', status='matched', candidates=['yeh-fina'])
        result = analyse_page(cache)
        self.assertEqual(result['summary']['provisionalSubwords'], 1)
        self.assertEqual(result['items'][0]['reason'], 'provisional-remainder')

    def test_spacing_and_marks_do_not_add_letter_slots(self):
        cache = self.cache()
        cache['subwords'][0]['text'] = 'بِـٔــۧمْ'
        self.assertEqual(body_letters(cache['subwords'][0]['text']), 'بم')
        self.assertEqual(analyse_page(cache)['items'][0]['expectedBodyCount'], 2)

    def test_headings_and_symbols_are_excluded(self):
        cache = self.cache()
        cache['sourceLines'][0]['transcriptWords'] = 0
        self.assertEqual(analyse_page(cache)['summary']['words'], 0)
        cache = self.cache()
        cache['sourceWords'][0]['text'] = cache['subwords'][0]['text'] = '١٢'
        self.assertEqual(analyse_page(cache)['summary']['words'], 0)

    def test_word_without_decomposition_is_reported(self):
        cache = self.cache()
        cache['subwords'] = []
        cache['sourceWords'][0]['subwords'] = 0
        result = analyse_page(cache)
        self.assertEqual(result['summary']['incompleteWords'], 1)
        self.assertEqual(result['items'][0]['id'], 'page003:l1-w1-s0')
        self.assertEqual(result['items'][0]['reason'], 'word-without-subwords')

    def test_same_selectors_group_dotted_variants_but_not_missing_selectors(self):
        beh = self.cache()['subwords'][0]['letters'][0]
        teh = {**beh, 'character': 'ت'}
        self.assertEqual(group_key(beh), group_key(teh))
        beh.update(status='no-template', candidates=[])
        teh.update(status='no-template', candidates=[])
        self.assertNotEqual(group_key(beh), group_key(teh))

    def test_body_count_alone_does_not_certify_completion(self):
        cache = self.cache()
        cache['subwords'][0]['complete'] = True
        self.assertEqual(analyse_page(cache)['summary']['incompleteWords'], 1)
        for letter in cache['subwords'][0]['letters']:
            letter['status'] = 'matched'
        self.assertEqual(analyse_page(cache)['summary']['completeConnectedSubwords'], 1)

    def test_failed_ink_partition_requires_review_even_with_all_templates(self):
        cache = self.cache()
        sub = cache['subwords'][0]
        sub['complete'] = True
        for letter in sub['letters']:
            letter['status'] = 'matched'
        result = analyse_page(cache, {'page003:l1-w1-s1'})
        self.assertEqual(result['items'][0]['reason'], 'ink-partition')
        self.assertTrue(result['items'][0]['reportedComplete'])
        self.assertEqual(result['summary']['incompleteConnectedSubwords'], 1)


if __name__ == '__main__':
    unittest.main()
