"""Sphinx configuration for the frost user documentation.

The site documents the ``frost`` command-line tool. It has no API reference:
the library is for the rare advanced user, and its headers are the reference.
The developer documents in this directory (architecture, decisions, roadmap,
RFI internals) stay in the repository and are not published.
"""

from __future__ import annotations

import datetime
import re
from pathlib import Path

DOCS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = DOCS_DIR.parent

# -- Project information -----------------------------------------------------
project = "frost"
author = "Pravir Kumar"
year = datetime.datetime.now(tz=datetime.UTC).date().year
copyright = f"{year}, {author}"  # noqa: A001
# The version lives in one place, project(VERSION) in CMakeLists.txt.
_cmake_version = re.search(
    r"project\([^)]*VERSION\s+([0-9][^\s)]+)",
    (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
)
release = _cmake_version.group(1) if _cmake_version else "unknown"
version = release
repo_url = "https://github.com/pravirkr/frost"

# -- General configuration ---------------------------------------------------
extensions = [
    "myst_parser",
    "sphinx_design",
    "sphinx_copybutton",
]
source_suffix = {".md": "markdown"}
exclude_patterns = [
    "_build",
    "Thumbs.db",
    ".DS_Store",
    "requirements.txt",
    # Logo generator, its font cache and its exports; not pages.
    "logo-src",
    # Developer documents, read in the repository rather than on the site.
    "architecture.md",
    "decisions.md",
    "roadmap.md",
    "rfi.md",
]

myst_enable_extensions = [
    "colon_fence",
    "deflist",
    "dollarmath",
    "html_image",
    "substitution",
]
myst_heading_anchors = 3
myst_substitutions = {"version": release}

# -- HTML --------------------------------------------------------------------
html_theme = "sphinx_book_theme"
html_title = f"frost {release}"
html_last_updated_fmt = "%b %d, %Y"
html_static_path = ["_static"]
html_css_files = ["custom.css"]
html_theme_options = {
    "repository_url": repo_url,
    "use_repository_button": True,
    "use_issues_button": True,
    "use_edit_page_button": True,
    "path_to_docs": "docs",
    "repository_branch": "main",
    "show_toc_level": 2,
    "show_navbar_depth": 2,
    "navigation_with_keys": True,
    "toc_title": "On this page",
    # The lockup already carries the name, so no text beside it.
    # docs/logo-src/make_logo.py generates every file in _static/logo/.
    "logo": {
        "image_light": "_static/logo/frost-logo.svg",
        "image_dark": "_static/logo/frost-logo-dark.svg",
        "alt_text": "frost - FDMT-accelerated Optimal Search for Radio Transients",
    },
}
html_favicon = "_static/logo/favicon.svg"

# -- sphinx-copybutton -------------------------------------------------------
# Commands in these pages carry no shell prompt; skip "#" comment lines.
copybutton_exclude = ".linenos, .gp, .c1"
