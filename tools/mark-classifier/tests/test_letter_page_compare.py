"""Whole-page review must preserve unknowns and ignore arbitrary instance IDs."""
import importlib.util
import unittest

AVAILABLE = all(importlib.util.find_spec(x) is not None for x in ('torch', 'scipy', 'skimage', 'numpy'))
if AVAILABLE:
    import numpy as np
    from letter_page_compare import compare_masks, input_fingerprint, priority, reference_crop, source_crop


@unittest.skipUnless(AVAILABLE, 'Optional letter-model dependencies are not installed')
class PageComparisonTests(unittest.TestCase):
    def test_unmatched_letter_is_not_a_valid_scoring_target(self):
        row = {'id': 'test', 'sourceRuns': [22, 6, 32, 6, 42, 6, 52, 6, 62, 6, 72, 6, 82, 6],
               'regions': [{'role': 'body', 'runs': [22, 6, 32, 6, 42, 6, 52, 6, 62, 6, 72, 6, 82, 6]}],
               'complete': True, 'letters': [{'status': 'no-match'}]}
        ink, bbox = source_crop(row, 10)
        _, _, status = reference_crop(row, 10, ink, bbox)
        self.assertFalse(status['valid'])

    def test_prediction_reuse_depends_on_source_not_template_labels(self):
        import copy
        source = {'raster': {'width': 10, 'height': 10}, 'sourceSha256': 'source',
                  'examples': [{'id': 'one', 'sourceRuns': [22, 6], 'complete': False, 'regions': []}]}
        revised = copy.deepcopy(source)
        revised['examples'][0].update(complete=True, regions=[{'role': 'body', 'runs': [22, 6]}])
        self.assertEqual(input_fingerprint(source), input_fingerprint(revised))
        revised['examples'][0]['sourceRuns'] = [23, 6]
        self.assertNotEqual(input_fingerprint(source), input_fingerprint(revised))
        revised = copy.deepcopy(source)
        revised['raster']['width'] = 12
        self.assertNotEqual(input_fingerprint(source), input_fingerprint(revised))

    def test_permuting_instance_ids_is_not_a_difference(self):
        truth = np.array([[0, 1, 1, 2], [0, 1, 1, 2]], dtype='uint16')
        pred = np.array([[0, 5, 5, 3], [0, 5, 5, 3]], dtype='uint16')
        roles = np.array([0, 1, 2]); pr = np.array([0, 0, 0, 2, 0, 1])
        stats, *_ = compare_masks(truth, pred, roles, pr, truth > 0)
        self.assertEqual(stats['changedPixels'], 0)
        self.assertEqual(priority(stats, {'valid': True}), 'same')

    def test_role_change_is_visible_with_identical_geometry(self):
        truth = np.array([[0, 1, 1], [0, 1, 1]], dtype='uint16')
        stats, *_ = compare_masks(truth, truth, np.array([0, 1]), np.array([0, 2]), truth > 0)
        self.assertEqual(stats['roleChangedPixels'], 4)
        self.assertEqual(priority(stats, {'valid': True}), 'count')

    def test_unassigned_source_is_not_called_agreement(self):
        truth = np.array([[0, 1, 1], [0, 1, 1]], dtype='uint16')
        pred = truth.copy(); pred[1] = 0
        stats, *_ = compare_masks(truth, pred, np.array([0, 1]), np.array([0, 1]), truth > 0)
        self.assertEqual(stats['rawUnassignedPixels'], 2)
        self.assertEqual(priority(stats, {'valid': True}), 'unresolved')

    def test_failed_reference_keeps_source_and_holes_explicit(self):
        row = {'id': 'test', 'sourceRuns': [22, 6, 32, 6, 42, 6, 52, 6, 62, 6, 72, 6, 82, 6],
               'regions': [{'role': 'body', 'runs': [22, 6]}], 'complete': False}
        ink, bbox = source_crop(row, 10)
        ref, roles, status = reference_crop(row, 10, ink, bbox)
        self.assertEqual(int(ink.sum()), 42)
        self.assertEqual(int((ref > 0).sum()), 6)
        self.assertFalse(status['valid'])
        self.assertEqual(status['unknownPixels'], 36)
        self.assertFalse(((ref > 0) & ~ink).any())

    def test_partial_template_is_reviewable_even_when_masks_agree(self):
        a = np.array([[1, 1], [1, 1]], dtype='uint16')
        stats, *_ = compare_masks(a, a, np.array([0, 1]), np.array([0, 1]), a > 0)
        self.assertEqual(priority(stats, {'valid': False}), 'unresolved')


if __name__ == '__main__':
    unittest.main()
