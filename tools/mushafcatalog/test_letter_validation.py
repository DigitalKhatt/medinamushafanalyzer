import unittest

from letter_validation import outline_height


class OutlineHeightTests(unittest.TestCase):
    def test_cubic_height_uses_the_curve_extremum(self):
        self.assertAlmostEqual(
            outline_height('M10 20C10 60 30 60 30 20', [2, 0, 0, 6, 10, 20]), 5
        )

    def test_height_is_measured_after_inverting_a_skewed_transform(self):
        # The raster outline has height 35, but the page outline has height 5.
        self.assertAlmostEqual(
            outline_height('M10 20L20 30L20 55L10 45', [2, 2, 0, 5, 10, 20]), 5
        )
