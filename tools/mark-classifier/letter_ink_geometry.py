"""Spatial tooth and connector features computed from binary source ink."""
import numpy as np
from scipy.ndimage import distance_transform_edt, gaussian_filter1d, label
from scipy.signal import find_peaks
from skimage.morphology import skeletonize
import torch
from torch import nn


def ink_geometry(ink):
    """Return two image-aligned channels scaled in local pen widths."""
    maps = np.zeros((2, *ink.shape), dtype=np.float32)
    occupied = ink.any(axis=0)
    edges = np.flatnonzero(np.diff(np.r_[False, occupied, False]))
    for start, stop in edges.reshape(-1, 2):
        y = np.flatnonzero(ink[:, start:stop].any(axis=1))
        top_y, end_y = y[0], y[-1] + 1
        crop = np.ascontiguousarray(ink[top_y:end_y, start:stop])
        skeleton = skeletonize(crop)
        if not skeleton.any():
            continue
        width = max(1., float(2 * np.median(distance_transform_edt(crop)[skeleton])))
        top = gaussian_filter1d(crop.argmax(axis=0).astype(float), sigma=max(.65, .3 * width))
        pad = float(crop.shape[0] - 1)
        peaks, _ = find_peaks(-np.r_[pad, top, pad], prominence=.65 * width,
                              distance=max(2, round(width)))
        peaks -= 1
        if len(peaks) < 2:
            continue
        valleys = [a + int(np.argmax(top[a:b + 1])) for a, b in zip(peaks[:-1], peaks[1:], strict=True)]
        floors = np.interp(peaks, valleys, top[valleys])
        rows = np.arange(crop.shape[0])[:, None]
        height_map, length_map = maps[:, top_y:end_y, start:stop]
        for peak, floor in zip(peaks, floors, strict=True):
            height = (floor - top[peak]) / width
            if height >= .65:
                radius = max(1, round(.5 * width))
                a, b = max(0, peak - radius), min(crop.shape[1], peak + radius + 1)
                support = crop[:, a:b] & (rows <= floor + width)
                height_map[:, a:b][support] = min(1., height / 8)
        for a, valley, b in zip(peaks[:-1], valleys, peaks[1:], strict=True):
            if min(top[valley] - top[a], top[valley] - top[b]) < .65 * width:
                continue
            floor = top[valley]
            left = right = valley
            while left > a and top[left - 1] >= floor - .5 * width:
                left -= 1
            while right < b and top[right + 1] >= floor - .5 * width:
                right += 1
            support = crop[:, left:right + 1] & (rows <= floor + width)
            length_map[:, left:right + 1][support] = min(1., (right - left + 1) / (12 * width))
    return maps


def stroke_geometry(ink):
    """Measure connector spans along the stroke shared by neighboring teeth."""
    maps = np.zeros((2, *ink.shape), dtype=np.float32)
    edges = np.flatnonzero(np.diff(np.r_[False, ink.any(axis=0), False]))
    for start, stop in edges.reshape(-1, 2):
        occupied = np.flatnonzero(ink[:, start:stop].any(axis=1))
        y, end = occupied[0], occupied[-1] + 1
        crop = np.ascontiguousarray(ink[y:end, start:stop])
        skeleton = skeletonize(crop)
        if not skeleton.any():
            continue
        distance = distance_transform_edt(np.pad(crop, 1))[1:-1, 1:-1]
        width = max(1., float(2 * np.median(distance[skeleton])))
        raw_top = crop.argmax(axis=0)
        top = gaussian_filter1d(raw_top.astype(float), sigma=max(.65, .3 * width))
        peaks, _ = find_peaks(-np.r_[crop.shape[0] - 1, top, crop.shape[0] - 1],
                              prominence=.65 * width, distance=max(2, round(width)))
        peaks -= 1
        if len(peaks) < 3:
            continue
        rows = np.arange(crop.shape[0])[:, None]
        # Follow the upper stroke. A lower stroke under a bowl, or an
        # unrelated stroke in the same columns, cannot bridge a gap here.
        near_top = rows <= raw_top + 1.6 * width
        upper_skeleton = skeleton & near_top
        links = {}
        for i, (a, b) in enumerate(zip(peaks[:-1], peaks[1:], strict=True)):
            valley = a + int(np.argmax(top[a:b + 1]))
            floor = top[valley]
            heights = np.array([floor - top[a], floor - top[b]]) / width
            if heights.min() < .65 or heights.max() > 5.5:
                continue
            labels, _ = label(upper_skeleton[:, a:b + 1], structure=np.ones((3, 3)))
            tolerance = min((b - a) // 3, max(1, round(width)))
            first = np.unique(labels[:, :tolerance + 1])
            last = np.unique(labels[:, b - a - tolerance:])
            crossing = np.intersect1d(first[first > 0], last[last > 0])
            if not len(crossing):
                continue
            # Isolate the low part of the stroke bounded by the two flanks.
            left = right = valley
            while left > a and top[left - 1] >= floor - .5 * width:
                left -= 1
            while right < b and top[right + 1] >= floor - .5 * width:
                right += 1
            path = labels[:, left-a:right-a+1] == crossing[0]
            count = path.sum(axis=0)
            if not count.all():
                continue
            local_width = max(1., float(2 * np.median(distance[:, left:right + 1][path])))
            if not .5 * width <= local_width <= 1.6 * width:
                continue
            center = (path * rows).sum(axis=0) / count
            length = float(1 + np.hypot(1, np.diff(center)).sum()) / local_width
            support = crop[:, left:right + 1] & (np.abs(rows - center) <= .65 * local_width)
            links[i] = (left, right, support, length, heights, floor)
        height_map, length_map = maps[:, y:end, start:stop]
        for i, (left, right, support, length, heights, floor) in links.items():
            if i - 1 not in links and i + 1 not in links:
                continue
            length_map[:, left:right + 1][support] = min(1., length / 12)
            radius = max(1, round(.5 * width))
            for peak, height in zip(peaks[i:i + 2], heights, strict=True):
                a, b = max(0, peak - radius), min(crop.shape[1], peak + radius + 1)
                patch = height_map[:, a:b]
                pixels = crop[:, a:b] & (rows <= floor + width)
                patch[pixels] = np.maximum(patch[pixels], min(1., height / 8))
    return maps


class GeometryProjection(nn.Conv2d):
    """Add geometry channels while preserving the original RGB projection."""
    def __init__(self, projection, extractor=True):
        # The new coefficients start at zero and preserve the caller's RNG.
        with torch.random.fork_rng(devices=[]):
            super().__init__(projection.in_channels + 2, projection.out_channels,
                             projection.kernel_size, projection.stride, projection.padding,
                             projection.dilation, projection.groups, projection.bias is not None,
                             projection.padding_mode, device=projection.weight.device,
                             dtype=projection.weight.dtype)
        with torch.no_grad():
            self.weight.zero_()
            self.weight[:, :projection.in_channels].copy_(projection.weight)
            if self.bias is not None:
                self.bias.copy_(projection.bias)
        if extractor is True or extractor == 'contour-v1':
            self.measure = ink_geometry
        elif extractor == 'stroke-v2':
            self.measure = stroke_geometry
        else:
            raise ValueError(f'Unknown geometry extractor {extractor!r}')

    def forward(self, image):
        # Channel zero is negative on black ink and positive on white paper
        # under the image normalization used by letter_retest.inputs.
        ink = (image[:, 0].detach() < 0).cpu().numpy()
        geometry = torch.from_numpy(np.stack([self.measure(crop) for crop in ink])).to(image)
        return super().forward(torch.cat((image, geometry), dim=1))
