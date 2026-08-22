"""WPT'S OWN CLASSIFICATION, ASKED OF WPT.

engine/wpt.mjs decides which checked-out files are tests and how many runs each one is. That decision is a PORT
of tools/manifest/sourcefile.py — the corpus's own authority — and a port that nothing compares against the
original is a copy that drifts. It has drifted four times already, and every time the cost was invisible from
inside the port: `.any.js` only, then no documents at all (523 of 778 files), then `.html` alone, then the
harness path as a bare substring. Each of those looked complete and excluded real tests, which is the one
defect that file exists to prevent.

So the gate asks sourcefile.py directly, every run, and any disagreement is fatal. This prints one line per
TESTHARNESS file, tab-separated: the path, then its variants (a file with none prints just the path, which is
sourcefile.py's `[""]` — exactly one run at the bare address).

Usage:  python3 engine/wpt_classify.py <corpus-root>
"""
import os
import sys

root = os.path.abspath(sys.argv[1])
# The manifest's own vendored dependencies, from the checkout rather than from the machine: the corpus is
# pinned, so its tools are too, and a system install would be a different version answering a pinned question.
sys.path.insert(0, os.path.join(root, "tools"))
for _p in ("html5lib", "webencodings", "atomicwrites", "six", "attrs", "zipp", "packaging",
           "more-itertools", "pathlib2"):
    sys.path.insert(0, os.path.join(root, "tools", "third_party", _p))

from manifest.sourcefile import SourceFile   # noqa: E402

out = []
for dirpath, dirnames, filenames in os.walk(root):
    # A DOTTED DIRECTORY IS NOT PART OF THE CORPUS — and `.git` is most of the bytes on disk.
    dirnames[:] = [d for d in dirnames if not d.startswith(".")]
    for fn in filenames:
        if fn.startswith("."):
            continue
        rel = os.path.relpath(os.path.join(dirpath, fn), root)
        # A FILE THAT CANNOT BE CLASSIFIED IS NOT SILENTLY DROPPED. sourcefile.py raises on a malformed variant
        # declaration, and that is a fact about the corpus the gate must see rather than a file to skip.
        kind, items = SourceFile(root, rel, "/").manifest_items()
        if kind != "testharness":
            continue
        url = "/" + rel.replace(os.sep, "/")
        variants = []
        for i in items:
            # A multi-global `.any.js` yields one item per GLOBAL as well as per variant, and this gate runs one
            # global. The VARIANT is the query or fragment the item's url carries past the file's own path.
            q = i.url.find("?")
            h = i.url.find("#")
            cut = h if q < 0 else (q if h < 0 else min(q, h))
            v = "" if cut < 0 else i.url[cut:]
            if v not in variants:
                variants.append(v)
        out.append("\t".join([url[1:]] + [v for v in variants if v]))
sys.stdout.write("\n".join(sorted(out)) + "\n")
