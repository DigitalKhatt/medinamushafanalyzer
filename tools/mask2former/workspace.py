"""Paths and frozen inputs for the native letter-segmentation trial."""
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parents[1]
WORKBENCH = NATIVE / 'tools/mark-classifier'
sys.path.insert(0, str(WORKBENCH))
from workbench_paths import DATA

AUTOMATIC = DATA / 'automatic'
RUNS = AUTOMATIC / 'mask2former'
ENV = NATIVE / 'output' / 'mask2former-env'
EXPERIMENTS = AUTOMATIC / 'experiments'

def read(path):
    return json.loads(path.read_bytes())


def run_path(value):
    path = Path(value).expanduser()
    if not path.is_absolute():
        return (RUNS / path).resolve()
    path = path.resolve()
    old_data = NATIVE.parent/'mushaf-segmentation/data/automatic'
    if path.is_relative_to(old_data):
        return AUTOMATIC/path.relative_to(old_data)
    return path


def latest_report():
    candidates = [*RUNS.glob('*/corpus-audit/summary.json'),
                  *EXPERIMENTS.glob('mask2former-*/corpus-audit/summary.json'),
                  *EXPERIMENTS.glob('mask2former-*/focused-replay/corpus-audit/summary.json')]
    candidates = [p for p in candidates if (p.parent / 'comparison.sqlite').is_file()
                  and read(p).get('pages') == 604 and not read(p).get('failures')]
    return max(candidates, key=lambda path: path.stat().st_mtime).parents[1] if candidates else None
