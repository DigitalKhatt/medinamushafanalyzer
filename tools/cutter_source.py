"""Fingerprint the cutter entry point and its private implementation files."""

import hashlib
from pathlib import Path


def cutter_source_digest(root: Path) -> str:
    entry = (root / 'segmentsubword.cpp').read_bytes()
    files = sorted(path for path in (root / 'cutter').rglob('*')
                   if path.suffix in {'.cpp', '.h', '.inc'})
    # Keep the old fingerprint for checkouts with a single source file.
    digest = hashlib.sha256(entry)
    for path in files:
        digest.update(b'\0')
        digest.update(path.relative_to(root).as_posix().encode('utf-8'))
        digest.update(b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()
