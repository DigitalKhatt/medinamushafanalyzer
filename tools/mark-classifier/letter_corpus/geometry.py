"""Exact source-pixel masks; no contour tracing or reconstruction of hidden ink."""
import hashlib
import json


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                    separators=(',', ':'), allow_nan=False).encode()).hexdigest()


def pixels(runs):
    import numpy as np
    if not runs:
        return np.array([], dtype='int64')
    return np.concatenate([np.arange(s, s+n, dtype='int64') for s, n in zip(runs[::2], runs[1::2])])


def encode_pixels(indices):
    import numpy as np
    p = np.unique(np.asarray(indices, dtype='int64'))
    if not p.size:
        return []
    cuts = np.flatnonzero(np.diff(p) > 1) + 1
    starts, ends = np.r_[0, cuts], np.r_[cuts, len(p)]
    return [int(v) for a, b in zip(starts, ends) for v in (p[a], b-a)]


def mask_runs(mask, bbox, width):
    import numpy as np
    yy, xx = np.where(mask)
    return encode_pixels((yy+bbox[1])*width + xx+bbox[0])


def bounds(runs, width):
    import numpy as np
    yy, xx = np.divmod(pixels(runs), width)
    if not xx.size:
        return [0, 0, 0, 0]
    return [int(xx.min()), int(yy.min()), int(xx.max()-xx.min()+1), int(yy.max()-yy.min()+1)]


def crop(runs, width, height, pad=3):
    from automatic.common import local_mask
    x, y, w, h = bounds(runs, width)
    box = [max(0, x-pad), max(0, y-pad), min(width, x+w+pad), min(height, y+h+pad)]
    box[2] -= box[0]
    box[3] -= box[1]
    return local_mask(runs, box, width) > 0, box


def validate_partition(ink, labels, raw):
    import numpy as np
    if labels.shape != ink.shape or raw.shape != ink.shape:
        raise ValueError('Prediction dimensions differ from the source')
    if np.any((labels > 0) & ~ink) or np.any((raw > 0) & ~ink):
        raise ValueError('Prediction adds source ink')
    if np.any((raw > 0) & (raw != labels)):
        raise ValueError('Completed prediction changes supported raw instances')
