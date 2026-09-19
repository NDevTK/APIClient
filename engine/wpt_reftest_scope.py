"""THE REFERENCE-IMAGE POPULATION, ASKED OF WPT — the second instrument beside engine/wpt_classify.py, and the
one that can see what that one drops on its own second line.

wpt_classify.py exists so engine/wpt.mjs's collector cannot drift from tools/manifest/sourcefile.py, the
corpus's own authority. It asks that authority a question and then keeps ONE of its answers: `if kind !=
"testharness": continue`. Every other kind the corpus declares leaves through that line, and the largest of
them by far in a CSS area is the REFTEST — a pair of documents and a `<link rel=match>`, asserting that two
RENDERINGS are identical. So a gate built on that collector is not merely silent about reftests; nothing
anywhere in this tree COUNTS them, and a count nobody makes is the excluded-test failure with no number to
notice.

This walks the same tree with the same classifier and reports the WHOLE breakdown, then the reftest
population's own structure. It decides nothing and runs nothing: what it produces is the scope a reftest
instrument would have to cover, derived rather than estimated.

THE BANDS AND WHY EACH IS SEPARATE:

  KINDS          every item type sourcefile.py returns, so the testharness fifth is reported as a fraction of
                 something rather than as a total.

  REFERENCES     how the graph is actually shaped — `==` against `!=`, how many references a test names, and
                 how many of those references are THEMSELVES reftest nodes. That last one is the difference
                 between a pair and a GRAPH, and a driver written for pairs answers the wrong question about
                 every chain in it.

  UNRESOLVABLE   the references this checkout does not hold. It is its own band and not a footnote, because a
                 reftest whose reference is absent does not report a missing file — it reports a FAILING
                 TEST, and the component it names is the engine. That is the same shape as the family whose
                 members each reported two subtests because an `.idl` directory was outside the cone: the
                 numbers were about the CHECKOUT and read as being about the engine. A reference that is not
                 a rooted path at all (`about:blank`) is counted apart from one that is a path this tree has
                 no file for, because those take opposite work — the first is a thing to MODEL and the second
                 is a directory to check out.

  FONT           how many pairs name a font on ONE side only. A reftest compares this engine against ITSELF,
                 so a font both sides load identically cancels however it rasterizes — that is the whole
                 reason reference-image testing does not need font fidelity. It cancels only where both sides
                 CONSUME it the same way, and a test that builds a length out of glyph advances against a
                 reference that states the same length as a CSS width is consuming it two ways. This band is
                 a SUBSTRING TEST over the files' bytes and nothing more: it bounds the population where the
                 cancellation argument needs checking, and establishes about no single member of that
                 population that a glyph is painted at all. Three read by hand carried the declaration on an
                 EMPTY box, where it reaches no glyph and costs nothing.

  SCRIPT         how many pairs contain no script anywhere. This engine is a forced multi-path solver, so one
                 document has as many appearances as it has flows and `which rendering` is a question with N
                 answers rather than one. A branch is what forks, and a document with no script has none to
                 take — so this band is what says whether that question arises for a given area or is a
                 property of a minority of it. It is a substring test for a start tag, so it counts a document
                 that HOLDS a script rather than one that RUNS a branch, which is a smaller set it does not
                 separate.

CALIBRATION IS THE READER'S AND IT IS ONE COMMAND. `--testharness` emits wpt_classify.py's exact line format
for the subtrees named here, so the two instruments can be diffed instead of believed:

    diff <(python3 engine/wpt_reftest_scope.py <root> <area> --testharness) \
         <(python3 engine/wpt_classify.py <root> | grep '^<area>/')

A probe that cannot reproduce the figure its subject already publishes is a second implementation of that
subject's selector answering a wider question, and its breakdown is worth nothing until it does.

NO TOTAL IS WRITTEN INTO THIS FILE. Every number it prints is one it derived on the run that printed it, and
a figure recorded here would be a claim about a corpus revision rather than about the one in the checkout.

Usage:  python3 engine/wpt_reftest_scope.py <corpus-root> [<subtree> ...] [--testharness]
        (no subtree names the whole root)
"""
import collections
import json
import os
import sys

argv = [a for a in sys.argv[1:] if a != "--testharness"]
emit_testharness = "--testharness" in sys.argv
if not argv:
    sys.exit(__doc__)
root = os.path.abspath(argv[0])
areas = argv[1:] or [""]

# The manifest's own vendored dependencies, from the checkout rather than from the machine — wpt_classify.py's
# reason exactly: the corpus is pinned, so its tools are, and a system install answers a pinned question with a
# different version.
sys.path.insert(0, os.path.join(root, "tools"))
for _p in ("html5lib", "webencodings", "atomicwrites", "six", "attrs", "zipp", "packaging",
           "more-itertools", "pathlib2"):
    sys.path.insert(0, os.path.join(root, "tools", "third_party", _p))

from manifest.sourcefile import SourceFile   # noqa: E402

# A NAMED SUBTREE THAT IS NOT THERE IS NOT AN EMPTY ONE. A walk over an absent directory yields nothing and
# every band below then reports zero, which reads exactly like an area with nothing in it — so the two are
# separated here, where the name is still in hand, rather than downstream where only the zero is.
for a in areas:
    if not os.path.isdir(os.path.join(root, a)):
        raise SystemExit("no such subtree: %s (under %s)" % (a or "<root>", root))

kinds = collections.Counter()
refitems = []        # one per reftest manifest item
harness = []         # wpt_classify.py's own line format, for the calibration above
for area in areas:
    for dirpath, dirnames, filenames in os.walk(os.path.join(root, area)):
        # A DOTTED DIRECTORY IS NOT PART OF THE CORPUS — wpt_classify.py's rule, restated because the two
        # walks must visit the same files for the calibration to mean anything.
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for fn in filenames:
            if fn.startswith("."):
                continue
            rel = os.path.relpath(os.path.join(dirpath, fn), root)
            # A FILE THAT CANNOT BE CLASSIFIED IS NOT SILENTLY DROPPED — sourcefile.py raises on a malformed
            # variant or fuzzy declaration, and that is a fact about the corpus rather than a file to skip.
            sf = SourceFile(root, rel, "/")
            kind, items = sf.manifest_items()
            kinds[kind] += 1
            if kind == "testharness":
                variants = []
                for i in items:
                    q, h = i.url.find("?"), i.url.find("#")
                    cut = h if q < 0 else (q if h < 0 else min(q, h))
                    v = "" if cut < 0 else i.url[cut:]
                    if v not in variants:
                        variants.append(v)
                harness.append("\t".join([rel.replace(os.sep, "/")] + [v for v in variants if v]))
            elif kind == "reftest":
                for it in items:
                    refitems.append((it.url, list(it.references), sf))

if emit_testharness:
    sys.stdout.write("\n".join(sorted(harness)) + "\n")
    raise SystemExit(0)

out = sys.stdout.write
out("KINDS   " + json.dumps(dict(kinds), sort_keys=True) + "\n")

# ─── REFERENCES ────────────────────────────────────────────────────────────────────────────────────────────
testurls = set(u for u, _, _ in refitems)
rel_count = collections.Counter()
per_test = collections.Counter()
use = collections.Counter()
fuzzy = viewport = dpi = 0
for url, refs, sf in refitems:
    per_test[len(refs)] += 1
    for r in refs:
        rel_count[r[1]] += 1
        use[r[0]] += 1
    if sf.fuzzy:
        fuzzy += 1
    if sf.viewport_size:
        viewport += 1
    if sf.dpi:
        dpi += 1
chain = sorted(set(use) & testurls)
out("REFS    tests=%d distinct-refs=%d relation=%s refs-per-test=%s\n"
    % (len(refitems), len(use), json.dumps(dict(rel_count), sort_keys=True),
       json.dumps(dict(sorted(per_test.items())))))
out("REFS    chain-links=%d (references that are themselves reftest nodes, so the graph is not a pair)\n"
    % len(chain))
out("REFS    fuzzy=%d viewport-size=%d device-pixel-ratio=%d\n" % (fuzzy, viewport, dpi))

# ─── UNRESOLVABLE ──────────────────────────────────────────────────────────────────────────────────────────
def on_disk(u):
    if not u.startswith("/"):
        return None                      # not a rooted path: `about:blank` and its kind
    p = u.split("?")[0].split("#")[0].lstrip("/")
    return os.path.exists(os.path.join(root, p))

absent = {u: c for u, c in use.items() if on_disk(u) is False}
unrooted = {u: c for u, c in use.items() if on_disk(u) is None}
blocked = set()
for url, refs, _ in refitems:
    if any(r[0] in absent for r in refs):
        blocked.add(url)
out("ABSENT  refs=%d blocking=%d tests (a reftest whose reference is not on disk reports a FAILING TEST and "
    "names the engine)\n" % (len(absent), len(blocked)))
for u, c in sorted(absent.items(), key=lambda kv: (-kv[1], kv[0])):
    out("          %6d  %s\n" % (c, u))
out("UNROOT  refs=%d (not a path in this tree: a thing to MODEL, never a directory to check out)\n"
    % len(unrooted))
for u, c in sorted(unrooted.items(), key=lambda kv: (-kv[1], kv[0])):
    out("          %6d  %s\n" % (c, u))

# ─── FONT and SCRIPT, over the pairs this checkout can actually read ────────────────────────────────────────
# BOTH BANDS ARE SUBSTRING TESTS AND THE REPORT SAYS SO IN ITS OWN LINE. What they bound is a population; what
# they establish about any member of it is nothing, which is why neither is phrased as a count of defects.
font = collections.Counter()
script = collections.Counter()
readable = 0
for url, refs, _ in refitems:
    paths = [u.split("?")[0].split("#")[0].lstrip("/") for u in [url] + [r[0] for r in refs]]
    if not all(p and os.path.exists(os.path.join(root, p)) for p in paths):
        continue
    readable += 1
    texts = []
    for p in paths:
        with open(os.path.join(root, p), "rb") as fh:
            texts.append(fh.read())
    t_font = b"hem" in texts[0]
    r_font = any(b"hem" in t for t in texts[1:])
    font[("test" if t_font else "-") + "/" + ("ref" if r_font else "-")] += 1
    script["with" if any(b"<script" in t.lower() for t in texts) else "none"] += 1
out("FONT    readable-pairs=%d by-side=%s  (substring `hem`; says nothing about whether a glyph is painted)\n"
    % (readable, json.dumps(dict(font), sort_keys=True)))
out("SCRIPT  readable-pairs=%d %s  (substring `<script`; a document that HOLDS one, not one that branches)\n"
    % (readable, json.dumps(dict(script), sort_keys=True)))
