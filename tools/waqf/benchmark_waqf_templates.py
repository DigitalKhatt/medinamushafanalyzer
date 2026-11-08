#!/usr/bin/env python3
"""Compile and run the read-only Qt waqf matching microbenchmark."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main(output):
    build = ROOT/'build/ninja-nmc'
    commands = subprocess.check_output(['ninja','-f','build-Release.ninja','-t','commands',
                                       'medinamushafanalyzer'], cwd=build, text=True).splitlines()
    command = next(c for c in commands if ' -c ' in c and c.endswith('/proposalexport.cpp'))
    with tempfile.TemporaryDirectory(prefix='medina-waqf-benchmark-') as temporary:
        obj = str(Path(temporary)/'benchmark.o'); executable = str(Path(temporary)/'benchmark')
        subprocess.run(shlex.split(command.split(' -MD ')[0])+['-I'+str(ROOT),'-o',obj,'-c',
                       str(Path(__file__).with_name('benchmark_waqf_templates.cpp'))], cwd=build, check=True)
        link = shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'))
        indices = [i for i,v in enumerate(link) if v.endswith('/main.cpp.o')]
        if len(indices) != 1:
            raise RuntimeError('Expected one application main')
        link[indices[0]]=obj;link[link.index('-o')+1]=executable
        subprocess.run(link,cwd=build,check=True)
        result = subprocess.check_output([executable], cwd=ROOT, text=True,
            env={**os.environ,'QT_QPA_PLATFORM':'offscreen'})
    data = json.loads(result)
    output.write_text(json.dumps(data,indent=2)+'\n')
    print(json.dumps(data,indent=2))


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    main(parser.parse_args().output)
