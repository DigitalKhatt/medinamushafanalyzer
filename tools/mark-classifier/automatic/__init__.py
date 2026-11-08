"""Explicitly machine-labelled corpus and learning, separate from human annotations."""
import sys

from workbench_paths import NATIVE

# The shared feature and training modules retain their recorded code hashes.
# The transcript-slot helper now comes from this analyzer's tools directory.
tools = str(NATIVE/'tools')
if tools not in sys.path:
    sys.path.insert(0, tools)
