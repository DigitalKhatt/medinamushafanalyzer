# Catalog and validation tools

Run the commands below from the `medinamushafanalyzer` root. The native
CMake preset uses Apple Silicon Homebrew paths. For other installations,
configure the same Ninja Multi-Config build with your dependency paths.

## Catalog

`mushafcatalog/catalog.py` generates, verifies, and serves the SQLite catalog.
The build uses the 1441 Hafs source artwork, the embedded native templates,
and the classifier and workbench code in `tools/mark-classifier/`. The
default database is `output/catalog.sqlite`.

Use **Mushaf Catalog: Full pipeline and serve** in VS Code, or run:

```sh
cmake --preset ninja-nmc
cmake --build --preset release --target medinamushafanalyzer
python3 tools/mushafcatalog/catalog.py setup
python3 tools/mushafcatalog/catalog.py fresh --pages all
python3 tools/mushafcatalog/catalog.py build
python3 tools/mushafcatalog/catalog.py segment-letters --pages all
python3 tools/mushafcatalog/catalog.py compact-vectors
python3 tools/mushafcatalog/catalog.py verify
python3 tools/mushafcatalog/catalog.py serve --port 8770
```

`fresh` reads the AI/PDF pages in `input/1441-AI-hafs/` and writes its report
to `output/mark-assignment-character-links-vector/`. Letter segmentation
stores the native page contours, grouped surah titles, basmalas and ayah
markers, and frame openings in the catalog. The viewer draws them directly.
Each letter-segmentation run writes `output/mushafcatalog/letter-segmentation.log`
with page totals and the subword IDs, letter diagnostics, and cutter traces
for provisional or unresolved cases. The log also records whether the catalog
update committed. Use `segment-letters --log PATH` to keep a separate log.

Check individual template edits against the saved cuts with
`python3 tools/mushafcatalog/catalog.py check-letter-cuts page069:l15-w6-s1 page435:l11-w6-s2`.
It reads the current template JSON directly and reports whether the cuts match
the saved outlines, endpoints, and raster masks. The catalog stays read-only.
Use `--templates PATH` to check a separate draft.

For a full regression check and a grid of changed cuts, use **Mushaf Catalog:
Check all letter cuts**, or:

```sh
python3 tools/mushafcatalog/catalog.py check-letter-corpus --report output/cut-review
```

Choose a new report directory each time. It contains `index.html`, the frozen
reference catalog, all recomputed multi-letter cuts, `cutter.log`, and timings
by stage and subword. This command does not update the live catalog. Single
letters remain unchanged. Native runs cache resolved AI source geometry in
`output/native-input-cache/`; they always recompute letter cuts. Artwork, font,
transcript and upstream source changes invalidate this cache automatically.
Set `MEDINA_DISABLE_INPUT_CACHE=1` to verify a fresh parse, or delete the cache.

Run `python3 tools/mushafcatalog/catalog.py validate-medial-lam-height` to check
every medial lām body against 5 page units. The JSON report lists short bodies
and unmeasured letters by ID. Either result makes the check fail.

Declared forms use `body` for recognition and `cuts` for cut placement.
Both support `inherit` plus local overrides. For example, `lam-bowl-fina`
declares its stem/bowl measurements in `body.stem`, then uses
`cuts.boundaries: {"right": "fork-normal", "left": "free"}`. There is no
lām `method` or `legacyLamRoutes` adapter. All lām forms and `beh-tooth-medi`
use these declarations; other forms can migrate independently.

`body.shape` selects the intrinsic shape vocabulary: contour stem,
descending stem, forked stem, turning shaft, terminal bowl/fork/return, or an
attached/detached return. `route` and `exitRoutes` describe contour landmarks;
the corresponding entries in `cuts` specify projections and offsets.
`minFlankSpan`, `minRise`, and `maxWidthToHeight` describe the shaft/fork.

`fork-normal` walks `forkOffset` page units along a fork flank, then cuts
normally to that stroke. `tightJoin: "tip-normal"` permits a cut from the tip
when space is limited. `free` adds no boundary; `preserve` keeps the boundary
from the declared shape's attachment. `beforeFinal` supplies the lām–alef
policy: keep the lower return with lām and share one seam at the inward
corners. `counterJoin` and `preserveEntryAfter` preserve the kāf–lām attachments.

The folded-foot option is `cuts.shortExit`: `minHorizontalRun` and
`maxEntryGap` use local stroke widths; `anchorTolerance` uses page units.
`cutsAroundHollow` and `entryMaxTilt` control its placement. Set
`retryOnUnresolved` to false to skip the longer-exit retry, or `shortExit` to
false to disable shortening. The ordinary fork walk remains 0.5 page unit.

For `beh-tooth-medi`, `body.shape: "tooth-pair"` recognizes the outer tip,
opposing inner bend, both flanks and local stroke measurements from uncut ink.
`body` contains pair-distance/width limits, flat-bend acceptance and the baseline
entry's minimum rise. `cuts` holds the left/right normal offsets, turn angle,
minimum turn run and tight-join policy. Its offsets use `local-stroke` units;
they preserve the existing normal-projection and contour-walk alternatives,
not a new symmetric contour-walk rule. `shared-boundary` permits a shared seam
when necessary. Recognition takes no cut policy or neighboring join landmarks.
Changing a policy can reuse the recognized evidence; the assembler still checks
that the tooth core survives. Check this separation and inheritance with:

```sh
python3 tools/tests/check_letter_forms.py body_cuts_test.cpp medial_tooth_test.cpp lam_alef_test.cpp
```

Catalog builds use AI/PDF reports. Rerun `build` with the same report to
upgrade a native catalog created with the previous artwork schema.

Put the pinned mark model bundle in
`output/mark-classifier-data/model-bundles/dot-grouping-v9-20261104/` before building.
Run `catalog.py --help` for database selection, queries, and SVG export.
[Open the catalog](http://127.0.0.1:8770/)

## Mark classifier

`mark-classifier/` checks the types of the mark and dot masks assigned by C++.
Use **Mark Classifier: Compare native assignments** to create a page report.
The audit derives its mask grid from the AI/PDF page dimensions and runs
without SVG artwork or Inkscape. Use `fresh` for a catalog build that includes
viewer artwork.
Keep that report's `inputs/` directory. **Mark Classifier: Replay saved
assignments** runs the classifier again from those inputs. Open `index.html`
in the resulting report.

```sh
python3 tools/mushafcatalog/catalog.py audit-marks --pages 193 --report output/marks-193
python3 tools/mushafcatalog/catalog.py replay-marks \
  --inputs output/marks-193/inputs --report output/marks-193-replay
```

## Mask2Former

`mask2former/` trains on eligible native letter cuts and compares predictions
with the cutter on connected subwords. The VS Code tasks prepare cuts,
repeat training, predict, and serve the comparison. The repeat task sets up
its Python environment. `prepare` and `repeat` generate missing raster metadata
from the AI/PDF artwork and freeze it with the page inventory in the run.
They keep any existing page splits and use the default deterministic split
for a first run. For starting weights, they use `--pretrained`, reuse the
previous run's starting weights when available, or
download the pinned [Mask2Former Swin-Tiny COCO checkpoint](https://huggingface.co/facebook/mask2former-swin-tiny-coco-instance/commit/22c4a2f15dc88149b8b8d9f4d42c54431fbd66f6).
The download is cached under `output/mask2former-model-cache/`. Training
fine-tunes those weights on native letter cuts.
The default classes are `body` and `kashida`. Add `--previous none --seen-body`
to the `repeat` command to train a shared sīn/shīn class from the first run.

```sh
python3.14 tools/mask2former/run.py setup
python3 tools/mask2former/run.py snapshot
output/mask2former-env/bin/python tools/mask2former/run.py repeat --run cutter-review --device auto
output/mask2former-env/bin/python tools/mask2former/run.py predict --run cutter-review --device auto
output/mask2former-env/bin/python tools/mask2former/run.py serve --run cutter-review
```

## Letter audit

`letter_audit/` reports missing forms and compares template cuts. The
shared `build_native_exporter.py` compiles the standalone snapshot exporter
against the Release build. It can also build the `hah_*.cpp` contour probes
used by the workbench experiments.

```sh
python3 tools/build_native_exporter.py --source export_incomplete_corpus.cpp --output output/letter-exporter
python3 tools/letter_audit/regenerate_letter_audit.py --executable output/letter-exporter --output output/letter-audit
python3 tools/letter_audit/report_incomplete_corpus.py --folder output/letter-audit
```

Open the report through a local static server. For two saved page caches,
`tools/letter_audit/review_letter_forms.py --help` shows the before-and-after
comparison options.

The body/cut pipeline is in `segmentsubword.cpp`:
`bodyCutProgram` resolves per-form descriptions (cached until the config changes);
`recognizeSubwordBodies` gathers reusable features from uncut ink;
`makeSubwordCutPlan` reads the matched form's right/left and ligature policies;
`resolveSubwordCutPlan` resolves compatible cuts before `assembleBodyPartition`
builds the masks. The planner has no hardcoded lām template-name dispatch.

Lām recognition bypasses the legacy method dispatcher through
`declaredLamBoundary` and `declaredMedialLamCandidates`. Generic contour
primitives are shared: `recognizeContourRoute` finds landmarks, while
`projectContourRoute` applies a cut policy. Non-lām forms still use the
existing matchers during the incremental migration. A failed lām declaration
cannot fall back to a legacy lām rule.

Run `python3 tools/tests/check_letter_forms.py lam_alef_test.cpp
letter_forms_test.cpp body_shape_test.cpp` for recognition, inheritance,
schema, per-form overrides, shared seams, and ink ownership. Use the full
`check-letter-corpus` command above for exact geometry and mask parity with
the current catalog.

## Vector review

`vector_review/` turns native vector exports into draft proposals for the
workbench under `mark-classifier/`. Run
`python3 -m tools.vector_review.prepare_vector_review --help` for the SVG,
page, and destination options. Use `--skip-export` to reuse raw exports.

## Waqf

`waqf/` rebuilds the native stop-sign templates and benchmarks their matching.
After building Release, run
`python3 tools/waqf/benchmark_waqf_templates.py --output output/waqf-benchmark.json`.
Use `python3 tools/waqf/build_waqf_templates.py --help` for the template inputs.

## Native regressions

`tests/` contains the active cutter, assignment, and fixture checks.

```sh
python3 tools/tests/check_letter_forms.py
python3 tools/tests/check_native_input_cache.py
python3 -m unittest discover -s tools/tests -p 'test_*.py'
python3 -m unittest discover -s tools/mushafcatalog -p 'test_*.py'
node --test tools/mushafcatalog/test_result_grid.mjs
```

The native checks use the Release binary and edition data. Pass a C++ test
filename to `check_letter_forms.py` to run one case.
The input-cache check uses the catalog to verify identical cold/warm masks,
fresh cuts after a template edit, and recovery from a truncated cache file.
