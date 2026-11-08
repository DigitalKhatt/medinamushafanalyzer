"""Compatibility names for tests that now exercise the compiled C++ stage."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/mark-classifier'))
from native_mark_assignment import run_native
from mark_validation import *


def expected_letters(word):
    return run_native('expectedLetters', word=word)


def consume(paths, buckets):
    return run_native('consume', paths=paths, owners=buckets)


def consume_with_vertical_quotas(paths, buckets, baseline, body_bbox=None, scale=4):
    return run_native('consumeVertical', paths=paths, owners=buckets, baseline=baseline,
                      bodyBox=body_bbox, scale=scale)


def select_alef_hamza_body(paths, buckets, native_body):
    return run_native('selectBody', paths=paths, owners=buckets, nativeBody=native_body)


def assign_page(doc, transcript, geometry=None, shape_bank=None):
    return run_native('assignPage', native=doc, transcript=transcript, geometry=geometry or {}, bank=shape_bank or {})


def segmentation_snapshot(subword):
    """Compare the full native segmentation to historical fixtures predating character links."""
    owners = []
    for owner in subword['owners']:
        groups = [{k: v for k, v in group.items() if k != 'transcriptLink'} for group in owner['groups']]
        owners.append(dict(owner, groups=groups))
    return dict(subword, owners=owners)


def normalize_detached_marks(subwords):
    return run_native('normalizeDetached', subwords=subwords)


def group_owned_ink(owner, paths, width, baseline, geometry=None, bank=None, boundary_conflicts=None):
    return run_native('groupOwnedInk', owner=owner, paths=paths, width=width, baseline=baseline,
                      geometry=geometry or {}, bank=bank or {}, conflicts=boundary_conflicts)


def decompose_dot_pairs(paths, width, bank=None):
    return run_native('decomposeDotPairs', paths=paths, width=width, bank=bank or {})


def owner_boundary_conflicts(owners, paths, width, baseline, geometry=None, bank=None):
    return run_native('boundaryConflicts', owners=owners, paths=paths, width=width, baseline=baseline,
                      geometry=geometry or {}, bank=bank or {})


def geometric_partition(owner, paths, width, baseline, geometry=None, bank=None):
    return run_native('geometricPartition', owner=owner, paths=paths, width=width, baseline=baseline,
                      geometry=geometry or {}, bank=bank or {})


def top_waqf_candidate(paths, contours):
    result = run_native('topWaqfCandidate', paths=paths, contours=contours)
    return result['paths'], result['reason']


def waqf_candidates(paths, geometry, width, baseline, scale):
    return run_native('waqfCandidates', paths=paths, geometry=geometry, width=width, baseline=baseline, scale=scale)


def template_match(path, name, scale):
    return run_native('templateMatch', path=path, name=name, scale=scale)


def dilated_signature(bits):
    return int(run_native('dilatedSignature', signature=f'{bits:064x}'), 16)


def reserve_top_waqf(paths, buckets, original, baseline, body_bbox=None, scale=4, geometry=None, width=0):
    return run_native('reserveWaqf', paths=paths, owners=buckets, original=original, baseline=baseline,
                      bodyBox=body_bbox, scale=scale, geometry=geometry or {}, width=width)
