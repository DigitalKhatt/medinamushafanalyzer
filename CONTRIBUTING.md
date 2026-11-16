# Contributing to Medina Mushaf Catalog

You can help by reporting a cut or search problem, reviewing letter shapes,
improving a cutter template, or fixing the catalog and viewer.

## Run the Python catalog tests

Clone the repository and run commands from its root. These tests check only
the Python catalog code, using small fixtures for search, storage, and page
rendering. You can run them with Python 3.11+ before downloading the artwork
or building the native analyzer.

```sh
python3 -m unittest discover -s tools/mushafcatalog -p 'test_*.py'
```

In VS Code, use **Mushaf Catalog: Run catalog tests**.

## Verify the full catalog installation

The page 324 example checks the full catalog pipeline. It builds the C++
analyzer, reads a real artwork page, validates marks, segments letters,
verifies the SQLite catalog, and opens the page in the viewer.

Follow the [build prerequisites and artwork setup](README.md#build-and-serve)
and install uv on your PATH. The checked-in CMake preset uses Apple Silicon
Homebrew paths. The [tools guide](tools/README.md) covers native setup,
validation, and the separate Mask2Former environment.

The checkout includes the [mark validation models](input/mark-validation).
Catalog builds verify their checksums against
[`inputs.lock.json`](tools/mushafcatalog/inputs.lock.json).

To try one page with its own report and database, run the following commands.
Use new output paths when repeating the example with changed inputs.

```sh
cmake --preset ninja-nmc
cmake --build --preset release --target medinamushafanalyzer
python3 tools/mushafcatalog/catalog.py setup
python3 tools/mushafcatalog/catalog.py fresh --pages 324 --report output/contributor-page324
python3 tools/mushafcatalog/catalog.py --db output/contributor-page324.sqlite build --report output/contributor-page324
python3 tools/mushafcatalog/catalog.py --db output/contributor-page324.sqlite segment-letters --pages 324
python3 tools/mushafcatalog/catalog.py --db output/contributor-page324.sqlite compact-vectors
python3 tools/mushafcatalog/catalog.py --db output/contributor-page324.sqlite verify
python3 tools/mushafcatalog/catalog.py --db output/contributor-page324.sqlite serve --port 8771
```

Open [page 324 in the viewer](http://127.0.0.1:8771/viewer.html#324). For the
full corpus, use **Mushaf Catalog: Full pipeline and serve** in VS Code.

## Report or change a cut

Include the page, line, word, and subword ID, such as `page324:l4-w7-s1`.
For a letter or mark, include its catalog ID if you have it. Describe the
expected result and which letter should own the stroke at the join. A
screenshot with the intended cut or an unambiguous contour landmark helps
someone reproduce it. For a search or viewer issue, include the query or
viewer URL and describe what you expected to see.

The [cutter guide](cutter/README.md) explains body recognition, entry and exit
anchors, and join policies. Letter forms live in
[`templates/letter-forms.json`](templates/letter-forms.json). For a cut change,
verify the reported case and a few other instances of the same form. Use
`check-letter-cuts` for specific IDs or **Mushaf Catalog: Check all letter
cuts** for a before-and-after grid against an existing catalog.
[See the comparison commands](tools/README.md#catalog).

## Open a pull request

Work on a branch and keep each pull request focused. Explain what changed,
which cases it affects, and which checks you ran. For a geometry change, include
representative before-and-after crops and call out any intentional cut
differences. Keep downloaded artwork, experiment checkpoints, generated
catalogs, caches, and full review exports out of the commit.

If you change the catalog’s search result cards, try a search in your local
catalog and run the [result-grid test](tools/README.md#native-regressions).
