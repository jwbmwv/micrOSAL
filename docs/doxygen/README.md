# MicrOSAL Documentation Generation

This directory holds generated Doxygen output for MicrOSAL.

## Prerequisites

Install Doxygen and Graphviz:

**Ubuntu/Debian:**

```bash
sudo apt install doxygen graphviz
```

**macOS:**

```bash
brew install doxygen graphviz
```

**Windows:**

Download installers from [Doxygen](https://www.doxygen.nl/download.html) and [Graphviz](https://graphviz.org/download/).

## Build Documentation

Run Doxygen from the repository root:

```bash
cd micrOSAL
rm -rf docs/doxygen/html docs/doxygen/latex docs/doxygen/xml
doxygen Doxyfile
```

The generated HTML is written to `docs/doxygen/html`.

## View Documentation

Open `docs/doxygen/html/index.html` in a browser:

```bash
# Linux
xdg-open docs/doxygen/html/index.html

# macOS
open docs/doxygen/html/index.html

# Windows
start docs/doxygen/html/index.html
```

## What Gets Documented

- Public C++ headers under `include/osal/`
- The C API surface in `include/osal/osal_c.h`
- Top-level guides under `docs/`
- Referenced architecture and sequence diagrams from `docs/diagrams/`

## Customization

Edit `Doxyfile` in the repository root to change:

- Project metadata
- Which files are included or excluded
- Warning behavior
- Diagram generation and image search paths
- HTML, LaTeX, or XML output options

## Troubleshooting

**Problem**: `doxygen: command not found`

Install the prerequisite packages above and re-run the command from the repository root.

```bash
doxygen --version
which doxygen  # Linux/macOS
where doxygen  # Windows
```

**Problem**: diagrams are missing from HTML output

Verify Graphviz is installed and that the files under `docs/diagrams/` are present.

```bash
dot -V
```

**Problem**: stale generated pages remain after config changes

Remove `docs/doxygen/html`, `docs/doxygen/latex`, and `docs/doxygen/xml` before the next Doxygen run.
