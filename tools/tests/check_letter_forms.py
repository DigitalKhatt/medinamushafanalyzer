#!/usr/bin/env python3
"""Run native letter-form and compound-body regressions with the Ninja release build."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/ninja-nmc"

# These tests exercise the cutter’s private geometry.
CUTTER_TESTS = (
    "tail_shoulder_test.cpp",
    "candidate_search_test.cpp",
    "medial_heh_test.cpp", "medial_tooth_test.cpp",
    "sad_reh_test.cpp", "beh_hah_ligature_test.cpp", "seen_teeth_test.cpp",
    "lam_alef_test.cpp", "lam_body_independence_test.cpp", "shared_body_independence_test.cpp", "medial_heads_test.cpp", "final_meem_test.cpp",
    "counter_ligature_test.cpp", "body_shape_test.cpp", "body_cuts_test.cpp", "hah_shoulder_test.cpp",
    "alef_stem_test.cpp", "upright_tooth_test.cpp", "hah_counter_test.cpp",
    "hah_compact_test.cpp", "tooth_ownership_test.cpp", "final_feh_test.cpp",
    "medial_hah_ownership_test.cpp", "medial_kaf_flat_test.cpp", "medial_meem_test.cpp",
    "medial_kaf_zigzag_test.cpp", "additional_shape_forms_test.cpp",
    "final_hah_test.cpp", "cutter_disagreement_test.cpp", "feh_fold_test.cpp",
)
TESTS = (
    "segmentation_dialog_test.cpp", "form_inspection_test.cpp", "letter_forms_test.cpp", "initial_heads_test.cpp", "body_letters_test.cpp", "compound_body_test.cpp",
    "page3_corpus_test.cpp", "recovered_letter_masks_test.cpp",
    *CUTTER_TESTS,
)


def main():
    selected = sys.argv[1:] or TESTS
    if any(source not in TESTS for source in selected):
        raise SystemExit("Unknown test source; pass names from the test list")
    commands = subprocess.check_output(
        ["ninja", "-f", "build-Release.ninja", "-t", "commands", "medinamushafanalyzer"],
        cwd=BUILD, text=True).splitlines()
    compile_command = next(line for line in commands if " -c " in line and line.endswith("/proposalexport.cpp"))
    for source in selected:
        with tempfile.TemporaryDirectory(prefix="medina-letter-form-checks-") as temporary:
            obj = str(Path(temporary) / "tests.o")
            executable = str(Path(temporary) / "letter-form-tests")
            compile_args = shlex.split(compile_command.split(" -MD ")[0])
            compile_args += ["-I" + str(ROOT), "-o", obj, "-c", str(ROOT / "tools/tests" / source)]
            subprocess.run(compile_args, cwd=BUILD, check=True)
            link = shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
            main_objects = [i for i, arg in enumerate(link) if arg.endswith("/main.cpp.o")]
            if len(main_objects) != 1:
                raise RuntimeError("Expected one main object in the application's Ninja link command")
            link[main_objects[0]] = obj
            link[link.index("-o") + 1] = executable
            subprocess.run(link, cwd=BUILD, check=True)
            subprocess.run([executable], cwd=ROOT, check=True,
                           env={**os.environ, "QT_QPA_PLATFORM": "offscreen",
                                "MEDINA_LETTER_FORMS": str(ROOT / "templates/letter-forms.json")})


if __name__ == "__main__":
    main()
