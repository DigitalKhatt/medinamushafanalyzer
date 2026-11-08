"""JSON transport for native regression tests. No assignment logic lives here."""
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run_native(operation, **arguments):
    # Frozen historical fixtures stored 256-bit signatures as JSON integers.
    # Encode them losslessly for C++; never round through a floating-point JSON value.
    def encode(value):
        if isinstance(value, int) and value > 2**64-1:
            return f'{value:064x}'
        if isinstance(value, float) and not math.isfinite(value):
            return None
        if isinstance(value, dict):
            return {k: encode(v) for k, v in value.items()}
        if isinstance(value, (list, tuple)):
            return [encode(v) for v in value]
        return value

    binary = Path(os.environ.get('MEDINA_ANALYZER_BINARY', ROOT/'build/ninja-nmc/Release/medinamushafanalyzer'))
    with tempfile.TemporaryDirectory(prefix='native-mark-test-') as directory:
        source, target = Path(directory)/'input.json', Path(directory)/'output.json'
        source.write_text(json.dumps([encode(dict(operation=operation, **arguments))], ensure_ascii=False))
        subprocess.run([str(binary), '--test-mark-assignment', str(source), '--test-mark-output', str(target)],
                       cwd=ROOT, env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'},
                       check=True, capture_output=True, text=True)
        result, = json.loads(target.read_text())
    if 'error' in result:
        raise ValueError(result['error'])
    return result['result']
