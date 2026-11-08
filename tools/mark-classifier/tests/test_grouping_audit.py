import copy
import json
from pathlib import Path
import tempfile
import unittest

from audit_grouping_changes import compare


class GroupingAuditTests(unittest.TestCase):
    def documents(self):
        paths = [{'id': name, 'runs': [101 + i*100, 3],
                  'bbox': [1, i+1, 3, 1], 'contourCount': 1}
                 for i, name in enumerate(('a', 'b', 'c'))]
        def candidate(name, ids, mark='unknown'):
            return {'id': name, 'pathIds': ids, 'kind': 'mark', 'markType': mark,
                    'wordId': 'w1', 'subwordId': 's1', 'line': 1}
        before = {'pageId': 'page001', 'sourceSha256': 'source',
                  'raster': {'width': 100}, 'paths': paths,
                  'candidates': [candidate('one', ['a']), candidate('two', ['b']),
                                 candidate('three', ['c'], 'kasra')]}
        after = copy.deepcopy(before)
        after['candidates'] = [candidate('merged', ['a', 'b'], 'tanwin-kasr'),
                               candidate('renumbered', ['c'], 'kasra')]
        return before, after

    def audit(self, before, after):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for folder, doc in (('before', before), ('after', after)):
                (root/folder).mkdir()
                (root/folder/'page001.json').write_text(json.dumps(doc))
            return compare(root/'before', root/'after')

    def test_reports_only_conserved_merge_and_ignores_candidate_numbering(self):
        before, after = self.documents()
        report = self.audit(before, after)
        self.assertEqual(report['unchangedGroups'], 1)
        self.assertEqual(len(report['changes']), 1)
        self.assertEqual(len(report['changes'][0]['removed']), 2)
        self.assertEqual(len(report['changes'][0]['added']), 1)
        same = copy.deepcopy(before)
        same['candidates'][0]['id'] = 'another-number'
        self.assertEqual(self.audit(before, same)['changes'], [])

    def test_rejects_changed_pixels_or_ownership(self):
        for mutation in ('pixels', 'owner', 'label', 'drop'):
            with self.subTest(mutation=mutation):
                before, after = self.documents()
                if mutation == 'pixels': after['paths'][0]['runs'][1] += 1
                elif mutation == 'owner': after['candidates'][1]['wordId'] = 'w2'
                elif mutation == 'label': after['candidates'][1]['markType'] = 'fatha'
                else: after['candidates'][0]['pathIds'] = ['a']
                with self.assertRaises(ValueError): self.audit(before, after)
