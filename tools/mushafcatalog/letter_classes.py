"""Shape classes for base letters.

Two letters that differ only by dots share one written body, so for shape
analysis they are the same instance class -- the dots are separate mark
components in the analyzer's output anyway. The class sets are the ones
already used for search in ``oldmadinaanalyzer/src/renderer.ts``; the joining
sets themselves live in ``tools/paw_segmentation/arabic_joining.py`` and are
not duplicated here.

The interesting case, also taken from the renderer: in initial and medial
position beh/teh/theh/noon/yeh all reduce to the same *senna* tooth, so a
query for "initial beh" that means "this shape" wants all of them, while a
query for final beh does not (final noon and final yeh are different bowls).
``shape_class`` encodes exactly that.
"""

from typing import Dict

# One body shape per entry; the members differ only in dots or hamza, which
# the analyzer segments as separate mark components.
SHAPE_CLASSES: Dict[str, str] = {
    "alef": "اآٱأإ",
    "beh": "بتث",
    "hah": "جحخ",
    "dal": "دذ",
    "reh": "رز",
    "seen": "سش",
    "sad": "صض",
    "tah": "طظ",
    "ain": "عغ",
    "feh": "ف",
    "qaf": "ق",
    "kaf": "ك",
    "lam": "ل",
    "meem": "م",
    "noon": "ن",
    "heh": "ةه",
    "waw": "وؤ",
    "yeh": "يىئ",
    "hamza": "ء",
    "tatweel": "ـ",
}

# The shared tooth. Only in initial and medial position: their final and
# isolated forms are genuinely different shapes.
SENNA = set("بتثنيئى")
SENNA_CLASS = "senna"

_CLASS_OF_CHAR = {ch: name for name, chars in SHAPE_CLASSES.items() for ch in chars}


def shape_class(char: str, form: str) -> str:
  """Return the body-shape class of one base letter in one joining form."""
  if not char:
    return ""
  if form in ("initial", "medial") and char in SENNA:
    return SENNA_CLASS
  return _CLASS_OF_CHAR.get(char, char)
