"""A corpus audit must expose lost small bodies and preserve all inference inputs."""
import importlib.util
from contextlib import closing
import json
import sqlite3
import unittest

AVAILABLE = all(importlib.util.find_spec(name) for name in ('torch', 'numpy', 'scipy', 'skimage'))
if AVAILABLE:
    import numpy as np
    from letter_corpus_audit import SCHEMA, disagreement, search, source_records
    from letter_retest import mask_decode


@unittest.skipUnless(AVAILABLE, 'Optional model dependencies are absent')
class CorpusAuditTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec('PySide6'), 'Qt rasterizer is absent')
    def test_unowned_partial_body_does_not_block_inference(self):
        from letter_corpus_audit import prepare_example
        outline = {'commands': [['M', 2, 2], ['L', 12, 2], ['L', 12, 12], ['L', 2, 12]],
                   'fillRule': 'evenodd'}
        row = dict(line=1, word=1, subword=1, text='بم', complete=False,
                   bodies=[outline], joins=[], sourceInk=outline)
        source = dict(page='page001', raster=dict(width=100, height=100), pdfToRaster=[4, 0, 0, 4, 0, 0])
        item = prepare_example(row, source)
        self.assertTrue(item['ink'].any())
        self.assertFalse(item['reference'].any())
        self.assertFalse(item['status']['valid'])

    def test_comparison_seam_repair_preserves_ink_and_rejects_deeper_gaps(self):
        from letter_page_compare import repair_reference_seams
        ink = np.pad(np.ones((40, 40), bool), 3)
        labels = np.zeros(ink.shape, 'uint16')
        labels[3:43, 3:23] = 1
        labels[3:43, 24:43] = 2
        count = (labels > 0).astype('uint16')
        fixed, repaired, extra = repair_reference_seams(ink, labels, count, 2)
        np.testing.assert_array_equal(fixed > 0, ink)
        self.assertEqual((repaired, extra), (40, 0))
        labels[3:43, 22:25] = 0
        self.assertIsNone(repair_reference_seams(ink, labels, (labels > 0).astype('uint16'), 2))
        large = np.pad(np.ones((100, 100), bool), 3)
        labels = large.astype('uint16')
        labels[40:50, 40:44] = 0
        self.assertIsNone(repair_reference_seams(large, labels, (labels > 0).astype('uint16'), 1))

    def test_report_keeps_single_letters_available_and_filters_to_actual_cut_cases(self):
        with closing(sqlite3.connect(':memory:')) as db:
            db.executescript(SCHEMA)
            for sid, bases, difference in [('single', 1, .9), ('multi', 3, .6)]:
                detail = dict(id=sid, expectedBodies=bases)
                db.execute('INSERT INTO examples VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
                           (sid, 3, sid, sid, 'train', 0, 'complete', difference, difference,
                            difference, 0, 0, '[]', json.dumps(detail), b''))
            self.assertEqual(search(db, {})['total'], 2)
            result = search(db, {'bases':['multi']})
            self.assertEqual(result['total'], 1)
            self.assertEqual(result['rows'][0]['id'], 'multi')
            self.assertEqual(search(db, {'bases':['single']})['rows'][0]['id'], 'single')
            self.assertEqual(search(db, {}, ['multi'])['rows'][0]['id'], 'multi')
            self.assertEqual(search(db, {}, [])['total'], 0)

    def test_reference_refresh_preserves_frozen_inference_and_rejects_changed_ink(self):
        from letter_corpus_audit import save_example
        from letter_corpus_refresh import stage_reference, stage_metadata
        import io
        with closing(sqlite3.connect(':memory:')) as db:
            db.executescript(SCHEMA)
            ink = np.ones((4, 6), bool)
            reference = np.ones((4, 6), 'uint16')
            reference[:, 3:] = 2
            row = dict(text='بم', wordText='بم', line=1, word=1, subword=1, complete=True,
                       templates=['old'], letters=[])
            item = dict(vector=dict(id='page003:l1-w1-s1'), row=row, ink=ink,
                        reference=reference, roles=np.array([0, 1, 1], 'uint8'),
                        status=dict(valid=True, rasterError=None), bbox=[0, 0, 6, 4])
            document = dict(page='page003', pdfToRaster=[4, 0, 0, 4, 0, 0])
            save_example(db, item, reference, item['roles'], reference, document, 'validation', set())
            db.commit()
            before = db.execute('SELECT masks FROM examples').fetchone()[0]
            revised = reference.copy()
            revised[:, 2] = 2
            with db:
                _, updated = stage_reference(db, dict(item, reference=revised), document, 'fresh')
            self.assertEqual(updated['split'], 'validation')
            self.assertFalse(updated['referenceHumanReviewed'])
            after = db.execute('SELECT masks FROM examples').fetchone()[0]
            with np.load(io.BytesIO(before)) as old, np.load(io.BytesIO(after)) as new:
                self.assertFalse(np.array_equal(old['reference'], new['reference']))
                for key in ('ink', 'prediction', 'predicted_roles', 'raw'):
                    np.testing.assert_array_equal(old[key], new[key])
            with db:
                stage_metadata(db, 'page003:l1-w1-s1', dict(row, templates=['new']), 'metadata')
            self.assertEqual(db.execute('SELECT masks FROM examples').fetchone()[0], after)
            bad_ink = ink.copy()
            bad_ink[0, 0] = False
            with self.assertRaisesRegex(ValueError, 'Source ink or crop changed'):
                with db:
                    stage_reference(db, dict(item, ink=bad_ink), document, 'bad')
            self.assertEqual(db.execute('SELECT masks FROM examples').fetchone()[0], after)
            reviewed = dict(updated, referenceHumanReviewed=True)
            db.execute('UPDATE examples SET detail=?', (json.dumps(reviewed),))
            db.commit()
            with self.assertRaisesRegex(ValueError, 'human-reviewed'):
                stage_metadata(db, 'page003:l1-w1-s1', row, 'bad')
            with self.assertRaisesRegex(ValueError, 'human-reviewed'):
                stage_reference(db, item, document, 'bad')

    def test_lost_small_letter_is_visible_inside_a_large_subword(self):
        truth = np.ones((20, 20), 'uint16')
        truth[-1, -2:] = 2
        prediction = np.ones_like(truth)
        stats = disagreement(truth, prediction, np.array([0, 1, 1]), np.array([0, 1]),
                             truth > 0, pen_width=2)
        self.assertLess(stats['differenceFraction'], .01)
        self.assertEqual(stats['worstBodyDifference'], 1)
        self.assertEqual(stats['referenceBodies'], 2)
        self.assertEqual(stats['modelBodies'], 1)

    def test_unknown_model_ink_is_a_disagreement(self):
        truth = np.ones((5, 5), 'uint16')
        stats = disagreement(truth, np.zeros_like(truth), np.array([0, 1]), np.array([0]),
                             truth > 0, pen_width=2)
        self.assertEqual(stats['differenceFraction'], 1)
        self.assertEqual(stats['rawUnassignedPixels'], 25)

    def test_filtering_unused_queries_preserves_the_canonical_decoder(self):
        classes = np.array([[.8, .1, .1], [.1, .8, .1], [.1, .1, .8], [.4, .3, .3]])
        probabilities = np.zeros((4, 8, 10))
        probabilities[0, :, :5] = .95
        probabilities[1, :, 5:] = .95
        probabilities[2] = 1
        keep = (classes[:, :2].max(1) >= .5) & (classes.argmax(1) != 2)
        ink = np.ones((8, 10), bool)
        full = mask_decode(probabilities, classes, ink, .5)
        selected = mask_decode(probabilities[keep], classes[keep], ink, .5)
        for expected, actual in zip(full, selected):
            np.testing.assert_array_equal(expected, actual)

    def test_all_single_and_multiletter_subwords_are_inferred_within_text_scope(self):
        rows = [dict(line=1, word=1, text='م'), dict(line=1, word=2, text='بِهِ'),
                dict(line=1, word=3, text='عنوان'), dict(line=2, word=1, text='عنوان')]
        cache = dict(subwords=rows, sourceLines=[dict(line=1, transcriptWords=2),
                                                dict(line=2, transcriptWords=0)],
                     sourceWords=[dict(line=1, word=1, inTranscript=True),
                                  dict(line=1, word=2, inTranscript=True),
                                  dict(line=1, word=3, inTranscript=False),
                                  dict(line=2, word=1, inTranscript=False)])
        self.assertEqual(source_records(cache), rows[:2])


if __name__ == '__main__':
    unittest.main()
