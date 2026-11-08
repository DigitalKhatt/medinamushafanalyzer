"""Locate the pinned mark validator or prepare a project-local virtual environment."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

from store import HERE, ROOT


def validator(install=False):
    requirements = HERE/'requirements-validation.txt'
    local = ROOT/'build/mushafcatalog-venv'
    configured = os.environ.get('MUSHAF_CATALOG_PYTHON')
    candidates = ([Path(configured)] if configured else
                  [local/'bin/python',ROOT.parent/'mushafanalyzer/.venv/bin/python',Path(sys.executable)])
    probe = ("import importlib.metadata as m, sys; "
             "sys.exit(not all(m.version(k)==v for k,v in (s.split('==') for s in sys.argv[1:])))")
    pins = requirements.read_text().splitlines()
    for python in candidates:
        if python.is_file() and subprocess.run([str(python),'-c',probe,*pins],capture_output=True).returncode==0:
            return python.absolute()
    if configured or not install:
        raise ValueError('The pinned NumPy/OpenCV environment is unavailable. Run the Set up validation task '
                         'or point MUSHAF_CATALOG_PYTHON to an environment matching requirements-validation.txt.')
    python = shutil.which('python3.14') or sys.executable
    subprocess.run([python,'-m','venv',str(local)],check=True)
    subprocess.run([str(local/'bin/python'),'-m','pip','install','-r',str(requirements)],check=True)
    return validator()
