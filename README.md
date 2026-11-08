# Medina Mushaf Catalog

Build and browse a searchable catalog of the 1441 Hafs edition. The native
analyzer supplies word, letter, dot, and mark assignments. The catalog serves
search results and regenerates pages with individual letter colors.

This project is sponsored by [@tarteelAI](https://github.com/TarteelAI) [<img src="https://assets-global.website-files.com/6167e862f6dfba5084eb5554/61680717c50ec79defcdb062_logo-group.svg">](https://www.tarteel.ai/)

## Build and serve

1. Install a C++20 compiler, CMake, Ninja, Qt 5 and Poppler with private
   headers, nlohmann/json, JPEG, PNG, OpenJPEG, pkg-config, and Python 3.11+.
2. Extract [the 1441 Madinah Hafs Mushaf](https://archive.org/download/1441-ai-hafs/1441-AI-hafs.zip) pages into `input/1441-AI-hafs/`.
3. In VS Code, run **Mushaf Catalog: Full pipeline and serve**. Once the
   database exists, **Mushaf Catalog: Serve catalog** opens it for browsing.

The catalog database lives at `output/catalog.sqlite`.

[Open the catalog](http://127.0.0.1:8770/) · [Open the page viewer](http://127.0.0.1:8770/viewer.html) · [Command-line and validation tools](tools/README.md)


