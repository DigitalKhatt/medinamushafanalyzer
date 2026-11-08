import importlib.util
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from letter_training_review import Selection, digest, freeze
from letter_bases import base_character_count, check_training_records, has_training_boundary
from segment import atomic_json, read_json


class TrainingSelectionTests(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.folder = self.root / 'candidates'
        for name in ('inputs', 'masks', 'vector', 'code', 'pretrained'):
            (self.folder / name).mkdir(parents=True)
        self.ids = ['page001:l1-w1-s1', 'page001:l1-w2-s1', 'page002:l1-w1-s1', 'page003:l1-w1-s1']
        records = []
        for i, (key, group) in enumerate(zip(self.ids, ('weak_train', 'weak_train', 'human_train', 'weak_validation'))):
            path = 'masks/' + str(i) + '.npz'
            (self.folder / path).write_bytes(bytes([i]))
            records.append(dict(id=key, group=group, split=group.split('_')[1], page=key.split(':')[0],
                                file=path, maskSha256=digest(self.folder/path), pdfToRaster=[1, 0, 0, 1, 0, 0],
                                baseCharacterCount=1 if group == 'weak_validation' else 2,
                                templateCases=['tooth|medial|ب']))
        doc = self.folder / 'vector/page001.json'
        atomic_json(doc, {'examples': [{'id': key} for key in self.ids[:2]]})
        atomic_json(self.folder / 'inputs/corpus.json', {'pages': [
            dict(id='page001', split='train'), dict(id='page002', split='train'), dict(id='page003', split='validation')]})
        atomic_json(self.folder / 'dataset.json', dict(trainingReview={'status': 'open'}, records=records,
                    vectorSnapshotHashes={doc.name: digest(doc)}))
        atomic_json(self.folder / 'run.json', dict(datasetSha256=digest(self.folder / 'dataset.json')))
        self.items = records
        self.selection = Selection(self.folder)

    def set_excluded(self, key, excluded=True):
        return self.selection.update(dict(id=key, excluded=excluded, datasetSha256=self.selection.fingerprint))

    def test_count_base_letters_instead_of_paths_marks_or_connections(self):
        for text, expected in [('نُـۦ', 1), ('ۥٓۚ', 0), ('ءٌ', 1), ('للَّ', 2), ('لَا', 2)]:
            with self.subTest(text=text):
                self.assertEqual(base_character_count(text), expected)
                self.assertEqual(has_training_boundary(dict(textForDisplayOnly=text, bodyRegions=3, kashidaRegions=1)),
                                 expected >= 2)
        self.assertFalse(has_training_boundary(dict(baseCharacterCount=1, bodyRegions=3)))
        self.assertTrue(has_training_boundary(dict(baseCharacterCount=2, bodyRegions=3)))
        self.assertFalse(has_training_boundary(dict(bodyRegions=3)))
        self.assertFalse(has_training_boundary(dict(textForDisplayOnly='نُ', baseCharacterCount=2, bodyRegions=2)))

    def test_enforce_training_scope_on_older_manifests_and_allow_single_letter_controls(self):
        single = dict(id='one-base-with-a-mark', textForDisplayOnly='قٓ', bodyRegions=1)
        for group in ('weak_train', 'human_train'):
            with self.assertRaisesRegex(ValueError, 'two confirmed base characters'):
                check_training_records([dict(single, group=group)])
        check_training_records([dict(single, group='weak_validation'), dict(single, group='weak_test')])

    def test_persistence_concurrent_edits_validation_and_restore(self):
        with ThreadPoolExecutor(max_workers=2) as threads:
            list(threads.map(self.set_excluded, self.ids[:2]))
        restored = Selection(self.folder)
        self.assertEqual(restored.read()['excluded'], self.ids[:2])
        self.assertEqual(self.set_excluded(self.ids[0])['revision'], 2)
        self.assertEqual(self.set_excluded(self.ids[0], False)['excluded'], [self.ids[1]])
        with self.assertRaises(ValueError):
            self.set_excluded(self.ids[-1])
        with self.assertRaises(ValueError):
            self.selection.update(dict(id=self.ids[0], excluded=True, datasetSha256='older'))
        self.assertEqual(read_json(self.selection.ledger)['excluded'], [self.ids[1]])

    @unittest.skipUnless(importlib.util.find_spec('numpy') and importlib.util.find_spec('PIL'), 'Optional raster dependencies')
    def test_freeze_filters_only_training_and_its_statistics(self):
        self.set_excluded(self.ids[0])
        atomic_json(self.folder / 'tooth-area.json', dict(measurements=[
            dict(id=self.ids[0], analyzerArea=18), dict(id=self.ids[1], analyzerArea=2)]))
        atomic_json(self.folder / 'preparation.json', dict(rules=1, exclusions={}))
        atomic_json(self.folder / 'template-coverage.json', dict(cases=[
            dict(template='tooth', position='medial', character='ب', status='covered',
                 selected=dict(train=2, validation=1))]))
        for name in ('runtime-versions.json', 'unfinished-inventory.json'):
            atomic_json(self.folder / name, {})
        output = self.root / 'final'
        freeze(self.selection, output)
        freeze(self.selection, output)
        manifest = read_json(output / 'dataset.json')
        self.assertEqual(manifest['records'], self.items[1:])
        self.assertEqual(manifest['groups'], {'human_train': 1, 'weak_train': 1, 'weak_validation': 1})
        self.assertEqual(manifest['decoder']['meanToothAnalyzerArea'], 2)
        self.assertEqual(read_json(output / 'template-coverage.json')['cases'][0]['selected'], {'train': 1, 'validation': 1})
        self.assertEqual(read_json(output / 'vector/page001.json')['examples'], [{'id': self.ids[1]}])
        self.assertFalse((output / self.items[0]['file']).exists())
        self.assertTrue((self.folder / self.items[0]['file']).is_file())
        with self.assertRaisesRegex(ValueError, 'frozen'):
            self.set_excluded(self.ids[2])

    @unittest.skipUnless(all(importlib.util.find_spec(k) for k in ('torch', 'skimage', 'scipy')), 'Optional model dependencies')
    def test_trainer_rejects_an_open_selection_before_reading_masks(self):
        from letter_retest import load_dataset
        with self.assertRaisesRegex(ValueError, 'open for review'):
            load_dataset(self.folder, for_training=True)
