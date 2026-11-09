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
The `medialLamShortExit` settings use local stroke widths for
`minHorizontalRun` and `maxEntryGap`, and page units for `anchorTolerance`.
When the folded exit is shortened, `cutsAroundHollow` uses the stem's turn to place
each cut up to one local stroke width from the hollow. This applies when the
original entry cut is within `entryMaxTilt` degrees of vertical.
Set `retryOnUnresolved` to false to skip the original-exit retry, or set
`medialLamShortExit` to false to disable shortening.

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
repeat training, predict, and serve the comparison. Runs use the bundled
workbench code and saved pretrained weights and page splits under
`output/mark-classifier-data/`.

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
python3 -m unittest discover -s tools/tests -p 'test_*.py'
python3 -m unittest discover -s tools/mushafcatalog -p 'test_*.py'
node --test tools/mushafcatalog/test_result_grid.mjs
```

The native checks use the Release binary and edition data. Pass a C++ test
filename to `check_letter_forms.py` to run one case.
