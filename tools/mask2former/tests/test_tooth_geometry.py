from pathlib import Path
import sys
import unittest

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tooth_geometry import measure_teeth
from letter_ink_geometry import GeometryProjection, ink_geometry, stroke_geometry
from letter_retest import inputs


def teeth(last):
    ink = np.zeros((25, last + 5), dtype=bool)
    ink[-4:] = True
    for x in (3, 19, last):
        ink[6:, x:x + 4] = True
    return ink


class ToothGeometryTest(unittest.TestCase):
    def test_stroke_connector_requires_connected_ink_under_the_peaks(self):
        intact = teeth(51)
        maps = stroke_geometry(intact)
        self.assertGreater(maps[1].max(), stroke_geometry(teeth(35))[1].max())
        self.assertFalse(maps[:, ~intact].any())

        broken = intact.copy()
        broken[-4:, 30:37] = False
        broken[:4, 30:37] = True
        self.assertTrue(broken.any(axis=0).all())
        self.assertFalse(stroke_geometry(broken)[1].any())

    def test_longer_connection_does_not_require_a_taller_tooth(self):
        ordinary, extended = measure_teeth(teeth(35)), measure_teeth(teeth(51))
        self.assertIsNotNone(ordinary)
        self.assertIsNotNone(extended)
        self.assertLess(ordinary['highTooth'], .5)
        self.assertLess(extended['highTooth'], .5)
        self.assertGreater(extended['longestGap'], ordinary['longestGap'] + 1)

    def test_input_maps_follow_the_ink_and_reach_trainable_weights(self):
        ink = teeth(51)
        maps = ink_geometry(ink)
        self.assertFalse(maps[:, ~ink].any())
        self.assertGreater(float(maps[1].max()), float(ink_geometry(teeth(35))[1].max()))
        projection = torch.nn.Conv2d(3, 4, 4, stride=4)
        augmented = GeometryProjection(projection)
        image = inputs(torch.from_numpy(ink)[None], 'cpu')
        torch.testing.assert_close(augmented(image), projection(image))
        augmented(image).sum().backward()
        self.assertTrue((augmented.weight.grad[:, 3:].abs().sum(dim=(0, 2, 3)) > 0).all())


if __name__ == '__main__':
    unittest.main()
