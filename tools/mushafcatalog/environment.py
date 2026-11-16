"""Locate the pinned mark validator or prepare a project-local virtual environment."""
import os
from pathlib import Path
import subprocess
import sys

from store import HERE, ROOT

sys.path.insert(0, str(ROOT/'tools'))
from python_environment import install_requirements


def validator(install=False):
    requirements = HERE/'requirements-validation.txt'
    local = ROOT/'build/mushafcatalog-venv'
    configured = os.environ.get('MUSHAF_CATALOG_PYTHON')
    python = Path(configured).expanduser() if configured else local/'bin/python'
    if install and not configured:
        python = install_requirements(local, requirements)
    probe = ("import importlib.metadata as m, sys; "
             "sys.exit(not all(m.version(k)==v for k,v in (s.split('==') for s in sys.argv[1:])))")
    pins = requirements.read_text().splitlines()
    if python.is_file() and subprocess.run([str(python),'-c',probe,*pins],capture_output=True).returncode==0:
        return python.absolute()
    raise ValueError('The pinned NumPy/OpenCV environment is unavailable. Run the Set up validation task '
                     'or point MUSHAF_CATALOG_PYTHON to an environment matching requirements-validation.txt.')
