"""Paths for the analyzer's bundled workbench and local corpus data."""
import json
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
NATIVE = PROJECT.parents[1]
DATA = NATIVE / 'output/mark-classifier-data'
CONFIG = PROJECT / 'project.json'


def svg_source(override=None):
    if override is not None:
        return Path(override).expanduser().resolve()
    source = Path(json.loads(CONFIG.read_bytes())['source_dir']).expanduser()
    return (CONFIG.parent / source).resolve()
