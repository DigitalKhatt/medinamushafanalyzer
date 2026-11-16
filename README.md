# Medina Mushaf Catalog

Build and browse a searchable catalog of the 1441 Hafs edition. The native
analyzer supplies word, letter, dot, and mark assignments. The catalog serves
search results and regenerates pages with individual letter colors.

This project is sponsored by [@tarteelAI](https://github.com/TarteelAI) [<img src="https://assets-global.website-files.com/6167e862f6dfba5084eb5554/61680717c50ec79defcdb062_logo-group.svg">](https://www.tarteel.ai/)

## Build and serve

1. Install a C++20 compiler, CMake 3.20+, Ninja, Qt 5 and Poppler with private
   headers, nlohmann/json 3.11+, JPEG, PNG, OpenJPEG, pkg-config, Python 3.11+, and
   [uv](https://docs.astral.sh/uv/getting-started/installation/).
   The default CMake preset uses Apple Silicon Homebrew paths.
2. Download [the 1441 Madinah Hafs Mushaf ZIP](https://archive.org/download/1441-ai-hafs/1441-AI-hafs.zip).
   Extract it and move or rename the enclosing artwork folder so that the 604
   `.ai` files are directly inside `input/1441-AI-hafs/`. The first file should
   be `input/1441-AI-hafs/001___Hafs39__DM.ai`.
3. Open the `medinamushafanalyzer` folder in VS Code and use
   **Terminal → Run Task → Mushaf Catalog: Full pipeline and serve**. To reopen
   a catalog you already built, use **Mushaf Catalog: Serve catalog**.
   Use **Mushaf Catalog: Serve local and LAN** and enter this computer's LAN
   IPv4 address to open the catalog from another device on the same network.

The checkout includes the mark validation weights. The pipeline reads the
AI artwork directly and writes the catalog to `output/catalog.sqlite`.

[Open the catalog](http://127.0.0.1:8770/) · [Open the page viewer](http://127.0.0.1:8770/viewer.html) · [Command-line setup](tools/README.md#catalog) · [Contributing](CONTRIBUTING.md)


