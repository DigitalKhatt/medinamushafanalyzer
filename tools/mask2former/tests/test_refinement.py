from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import workspace  # Set up the shared segmentation module path.
from letter_retest import canvas_example, mask_decode, native, query_ownership_loss, resize_example
from refine import calibrate
from train import validate


class RefinementTest(unittest.TestCase):
    def test_grouping_penalizes_splits_and_merges(self):
        labels = torch.tensor([[1, 1, 1, 1, 2, 2], [1, 1, 1, 1, 2, 2]])
        targets = [(labels == torch.tensor([1, 2])[:, None, None]).float()]
        matches = [(torch.tensor([1, 0]), torch.tensor([0, 1]))]
        classes = torch.zeros((1, 3, 4))
        correct = torch.full((1, 3, 2, 6), -8.)
        correct[0, 1, :, :4] = 8
        correct[0, 0, :, 4:] = 8
        split = correct.clone()
        split[0, 1, :, 2:4] = -8
        split[0, 2, :, 2:4] = 8
        merged = correct.clone()
        merged[0, 1, :, 4:] = 8
        merged[0, 0, :, 4:] = -8

        good = query_ownership_loss(correct, classes, targets, matches)
        for alternative in (split, merged):
            alternative.requires_grad_()
            loss = query_ownership_loss(alternative, classes, targets, matches)
            self.assertGreater(loss.item(), good.item() + 1)
            loss.backward()
            self.assertTrue(torch.isfinite(alternative.grad).all().item())

    def test_larger_canvas_maps_masks_back_to_source_coordinates(self):
        labels = np.zeros((72, 104), dtype='uint16')
        labels[12:62, 17:68] = 1
        labels[26:48, 68:92] = 2
        example = canvas_example({}, labels, np.array([0, 1, 2]))
        resized = resize_example(example, (320, 448))
        self.assertEqual(tuple(resized['canvas'].shape), (320, 448))
        self.assertEqual(resized['box'], tuple(2 * value for value in example['box']))
        np.testing.assert_array_equal(native(resized['canvas'][None].float(), resized)[0].numpy(), labels)

    def test_overlap_prunes_duplicate_but_preserves_letters_and_thin_connection(self):
        ink = np.ones((4, 14), dtype=bool)
        masks = np.zeros((4, 4, 14), dtype='float32')
        masks[0, :, :6] = .95
        masks[1, :, 8:] = .95
        masks[2, :, :9] = .7
        masks[2, :, 5] = .99
        masks[3, :, 6:8] = .95
        classes = np.array([[.01, .01, .95, .03], [.95, .01, .01, .03],
                            [.95, .01, .01, .03], [.01, .95, .01, .03]])
        old, _, _ = mask_decode(masks, classes, ink, min_region_area=1)
        self.assertEqual(len(np.unique(old)), 4)
        new, roles, raw = mask_decode(masks, classes, ink, min_region_area=1,
                                      minimum_owned_fraction=.5)
        np.testing.assert_array_equal(new, np.tile([1] * 6 + [3] * 2 + [2] * 6, (4, 1)))
        np.testing.assert_array_equal(roles, [0, 1, 1, 2])
        np.testing.assert_array_equal(new > 0, ink)
        np.testing.assert_array_equal(raw > 0, ink)
        # Background pixels must not affect the overlap calculation.
        padded = np.pad(masks, ((0, 0), (2, 2), (3, 3)), constant_values=.9)
        padded_ink = np.pad(ink, ((2, 2), (3, 3)))
        result, _, _ = mask_decode(padded, classes, padded_ink, min_region_area=1,
                                    minimum_owned_fraction=.5)
        np.testing.assert_array_equal(result[2:-2, 3:-3], new)
        self.assertFalse(result[~padded_ink].any())

    def test_shared_inference_calibration_matches_separate_validation(self):
        labels = np.tile([1, 1, 1, 1, 2, 2], (3, 1)).astype('uint16')
        sample = dict(record={}, labels=labels, roles=np.array([0, 1, 1]))
        items = [dict(example=sample, ink=labels > 0, general=sample, letters=None)]
        masks = np.stack([labels == 1, labels == 2]).astype('float32')
        # At the last threshold one uncertain body disappears; the decoder
        # assigns its source ink to the surviving seen-body instance.
        classes = np.array([[.59, .01, .01, .39], [.01, .01, .95, .03]])
        probabilities = [(masks, classes)]
        settings = dict(threshold=.55, decoder=1)
        with patch('refine.batch_probabilities', return_value=probabilities) as infer:
            results = calibrate(Mock(), items, settings, 'cpu')
        infer.assert_called_once()
        self.assertEqual([row['threshold'] for row in results], [.35, .5, .55, .65])
        with patch('train.batch_probabilities', return_value=probabilities):
            for row in results:
                expected = validate(Mock(), items, dict(settings, threshold=row['threshold']), 'cpu')
                self.assertEqual(row['general'], expected['general'])
                self.assertEqual(row['focus'], expected['focus'])
                self.assertEqual(row['general']['inkCoverage'], 1)
        self.assertGreater(results[0]['general']['roleAwareInstanceF1AtIoU50'],
                           results[-1]['general']['roleAwareInstanceF1AtIoU50'])


if __name__ == '__main__':
    unittest.main()
