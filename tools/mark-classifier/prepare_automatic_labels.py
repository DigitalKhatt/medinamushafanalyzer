#!/usr/bin/env python3
"""Prepare automatic training labels offline; never train or approve them.

Use --reuse-corpus to freeze existing proposals/regions, or omit it to regenerate
proposals and regions first. Other options are those of automatic_pipeline.py.
"""
import sys
from automatic_pipeline import main

if __name__=='__main__':
    reuse='--reuse-corpus' in sys.argv
    if reuse:sys.argv.remove('--reuse-corpus')
    sys.argv.insert(1,'freeze-labels' if reuse else 'prepare-labels')
    main()
