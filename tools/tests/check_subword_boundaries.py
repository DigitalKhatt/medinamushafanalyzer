#!/usr/bin/env python3
"""Check native subword ownership against all cached pages and fresh PDF parsing."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/ninja-nmc'


def main():
    commands = subprocess.check_output(
        ['ninja', '-f', 'build-Release.ninja', '-t', 'commands', 'medinamushafanalyzer'],
        cwd=BUILD, text=True).splitlines()
    command = next(c for c in commands if ' -c ' in c and c.endswith('/proposalexport.cpp'))
    with tempfile.TemporaryDirectory(prefix='medina-subword-boundaries-') as temporary:
        obj = str(Path(temporary) / 'tests.o')
        executable = str(Path(temporary) / 'checks')
        subprocess.run(shlex.split(command.split(' -MD ')[0]) +
                       ['-I' + str(ROOT), '-o', obj, '-c', str(ROOT/'tools/tests/subword_boundaries_test.cpp')],
                       cwd=BUILD, check=True)
        link = shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'))
        main_objects = [i for i, value in enumerate(link) if value.endswith('/main.cpp.o')]
        if len(main_objects) != 1:
            raise RuntimeError('Expected one application main object')
        link[main_objects[0]] = obj
        link[link.index('-o')+1] = executable
        subprocess.run(link, cwd=BUILD, check=True)
        subprocess.run([executable], cwd=ROOT, check=True,
                       env={**os.environ, 'QT_QPA_PLATFORM': 'offscreen'})


if __name__ == '__main__':
    main()
