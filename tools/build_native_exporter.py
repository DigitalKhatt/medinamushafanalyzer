"""Build a standalone native exporter using the existing Ninja Release build."""
import argparse
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def build(destination, source):
    destination = Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    build_dir = ROOT / 'build/ninja-nmc'
    commands = subprocess.check_output(['ninja', '-f', 'build-Release.ninja', '-t', 'commands',
                                        'medinamushafanalyzer'], cwd=build_dir, text=True).splitlines()
    command = next(s for s in commands if ' -c ' in s and s.endswith('/proposalexport.cpp'))
    obj = str(destination) + '.o'
    subprocess.run(shlex.split(command.split(' -MD ')[0]) + ['-I'+str(ROOT), '-o', obj, '-c',
                   str(ROOT/'tools'/source)], cwd=build_dir, check=True)
    link = shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'))
    link[next(i for i,s in enumerate(link) if s.endswith('/main.cpp.o'))] = obj
    link[link.index('-o')+1] = str(destination)
    subprocess.run(link, cwd=build_dir, check=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True, help='C++ source relative to tools/, or an absolute path')
    args = parser.parse_args()
    build(args.output, args.source)
