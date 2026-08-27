#!/usr/bin/env python3
"""TWO MECHANICAL ANSWERS TO "IS THIS FILE SOMEBODY ELSE'S TOO", because every other answer we have is a
judgement call and a judgement call produces a commit that LOOKS filtered.

The checkout is shared and edited continuously, so a file can hold your hunk and another lane's at the same
instant, and a `git add <path>` then publishes theirs under your message.  CLAUDE.md's rule for that is to
stage the HUNK and never the file.  Doing it by eye fails in the one direction that matters: you keep the
hunks you recognise, and the ones you do not recognise are exactly the ones that are not yours.

    stale <file.c> [-- <extra cc flags>]
        Compile the COMMITTED form of one translation unit against the WORKING TREE's headers.  A
        `conflicting types` / `implicit declaration` / `too many arguments` error CANNOT be true unless the
        two halves belong to different revisions -- the committed .c and the edited .h are one program only
        if nobody is mid-edit.  This is the whole check, and it is a compiler answering yes or no rather than
        anyone's reading of `git status`.  Measured: `url_encoded_list_append` was defined with five
        parameters in the committed url.c and declared with seven in the working tree's url.h, which is how a
        sibling lane's in-flight ABI change was found before it could be swept into someone else's commit.
        Exit 0 = the tree is consistent for this TU.  Exit 3 = it is not, and the error names the symbol.

    atrev <file>:<line> [rev ...]
        Print that source line at each revision named (default `origin/main`), because A CRASH'S file:line
        ONLY MEANS SOMETHING AT THE REVISION THAT COMPILED.  `JS_ToCString` and its siblings are macros that
        capture __FILE__/__LINE__ at the call site, so a `@WHY` naming `url.c:1878` is naming one exact line
        -- and if the artifact was built from a tree that was dirty, or from an older checkout than its head
        claim suggests, that line is a DIFFERENT statement than the one at head.  Measured: a rung reported
        "the coercion is back" against an artifact whose head claim contained the fix, and `url.c:1878` was
        `bs = JS_ToCStringLen(ctx, &bs_len, argv[1])` at the revision BEFORE the fix and an ordinary function
        call at every revision after it.  The crash was real and it was the OLD code; the artifact did not
        contain what its head claim said it did.  Reading a crash against head instead of against the built
        revision is the stale-DFAIL failure mode with the roles reversed -- the text is accurate about a tree,
        just not the one that ran.

    split <file> --mine <marker> [--mine ...] --theirs <marker> [--theirs ...]
        Write, to stdout, a patch of only the hunks of `git diff origin/main -- <file>` that carry one of
        your markers.  A marker is any substring that appears in your hunks and cannot appear in theirs -- a
        function name you added, a field you introduced.
        IT REFUSES RATHER THAN GUESSING.  A hunk carrying markers from BOTH lanes is not split here: the
        exit is non-zero and nothing is written, because a hunk that mixes two lanes has to be split by LINE
        by the person who knows which line is whose, and a tool that guessed would hand back a patch with
        somebody else's line in it and no way to see that it had.  A hunk carrying NEITHER marker is also
        refused, for the same reason in the other direction: an unclassified hunk is one you have not
        accounted for, and silently dropping it is how a lane loses its own work.

    Then, as ONE uninterrupted operation (CLAUDE.md: stage and commit together or do not stage at all):
        python3 testing/lane-hunks.py split <file> --mine foo --theirs bar > /tmp/mine.patch \\
          && git apply --cached /tmp/mine.patch && git commit -F <message>

DIFF AGAINST `origin/main`, NEVER `HEAD`: a shared checkout is usually BEHIND, and a diff against the wrong
base cannot tell "stale" from "ahead".  That is why `split` hard-codes the base rather than taking one.
"""
import argparse
import subprocess
import sys

BASE = "origin/main"


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def cmd_stale(args):
    """The committed TU against the working tree's headers -- see the module docstring."""
    r = run(["git", "show", f"{BASE}:{args.file}"])
    if r.returncode:
        print(f"lane-hunks: {args.file} is not in {BASE}: {r.stderr.strip()}", file=sys.stderr)
        return 2
    # THE COMMITTED BYTES IN A SCRATCH PATH, so the compiler reads the working tree's headers through -I and
    # this file's own committed form as the TU.  Writing it back into the tree would be the edit we are
    # trying to detect.
    tmp = "/tmp/lane-hunks-stale.c"
    with open(tmp, "w") as fh:
        fh.write(r.stdout)
    # THE CALLER'S FLAGS GO FIRST so an extra -I can SHADOW a header: that is how you ask "is this TU
    # consistent with the headers at some other revision" as well as with the working tree's, and it is how
    # this check is exercised at all without editing a shared header to break it.
    cc = ["gcc", "-fsyntax-only", "-Wall", "-DAPICLIENT_DEV=1"] + args.ccflags + [
          "-I", "engine/host", "-I", "engine/host/browser", "-I", "engine/qjs",
          "-I", "engine/host/browser/core", "-I", "engine/lexbor/source", tmp]
    out = run(cc)
    # ONLY THE CROSS-REVISION ERRORS COUNT.  A TU pulled out to /tmp legitimately fails on a quoted include
    # that resolved next to the original file, and reporting that as staleness would make this instrument
    # cry wolf until nobody ran it.  These three diagnostics are the ones a declaration/definition SPLIT
    # across two revisions produces and that nothing else here produces.
    SPLIT = ("conflicting types", "implicit declaration", "too many arguments",
             "too few arguments", "conflicting declaration")
    hits = [l for l in out.stderr.split("\n") if any(s in l for s in SPLIT)]
    if hits:
        print(f"lane-hunks: {args.file} and the working tree's headers are DIFFERENT REVISIONS:",
              file=sys.stderr)
        for l in hits:
            print("  " + l, file=sys.stderr)
        return 3
    print(f"lane-hunks: {args.file} is consistent with the working tree's headers")
    return 0


def cmd_atrev(args):
    """One source line, at each revision named -- see the module docstring for why this is the question."""
    where = args.target.rsplit(":", 1)
    if len(where) != 2 or not where[1].isdigit():
        print("lane-hunks: atrev wants <file>:<line>, not " + args.target, file=sys.stderr)
        return 2
    path, line = where[0], int(where[1])
    revs = args.revs or [BASE]
    seen = {}
    for rev in revs:
        r = run(["git", "show", f"{rev}:{path}"])
        if r.returncode:
            print(f"  {rev:<14} (not in this revision)")
            continue
        lines = r.stdout.split("\n")
        text = lines[line - 1].rstrip() if 0 < line <= len(lines) else "(past end of file)"
        seen.setdefault(text, []).append(rev)
        print(f"  {rev:<14} {text[:120]}")
    # THE COMPARISON IS THE POINT, so it is stated rather than left for the reader to eyeball two long lines.
    if len(revs) > 1:
        print(("lane-hunks: the line is THE SAME at every revision named"
               if len(seen) == 1 else
               f"lane-hunks: the line DIFFERS across the revisions named ({len(seen)} distinct) -- a crash at "
               "this address means a different statement depending on which one compiled"), file=sys.stderr)
    return 0


def hunks_of(file):
    """The diff's header lines, then one list per hunk."""
    d = run(["git", "diff", BASE, "--", file]).stdout.split("\n")
    head, out, cur = [], [], None
    for line in d:
        if line.startswith("@@"):
            if cur is not None:
                out.append(cur)
            cur = [line]
        elif cur is None:
            head.append(line)
        else:
            cur.append(line)
    if cur is not None:
        out.append(cur)
    return head, out


def cmd_split(args):
    head, hunks = hunks_of(args.file)
    if not hunks:
        print(f"lane-hunks: {args.file} does not differ from {BASE}", file=sys.stderr)
        return 1
    mine, refused = [], []
    for h in hunks:
        body = "\n".join(h)
        m = any(k in body for k in args.mine)
        t = any(k in body for k in args.theirs)
        if m and t:
            refused.append(("MIXES BOTH LANES", h[0]))
        elif m:
            mine.append(h)
        elif not t:
            refused.append(("MATCHES NEITHER MARKER SET", h[0]))
    if refused:
        print(f"lane-hunks: REFUSING to split {args.file} -- {len(refused)} hunk(s) this tool must not "
              f"classify. Split them by LINE yourself; a guess here produces a patch that looks filtered.",
              file=sys.stderr)
        for why, hdr in refused:
            print(f"  {why}: {hdr[:110]}", file=sys.stderr)
        return 4
    sys.stdout.write("\n".join(head + [l for h in mine for l in h]) + "\n")
    print(f"lane-hunks: {len(mine)} of {len(hunks)} hunk(s) are yours", file=sys.stderr)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("stale", help="compile the committed TU against the working tree's headers")
    s.add_argument("file")
    # REMAINDER, so a caller's own `-I` reaches gcc instead of being read as one of this tool's options.
    s.add_argument("ccflags", nargs=argparse.REMAINDER, default=[])
    s.set_defaults(fn=cmd_stale)

    s = sub.add_parser("atrev", help="print one source line at each revision named")
    s.add_argument("target", help="<file>:<line>, as a @WHY spells it")
    s.add_argument("revs", nargs="*", help="revisions to resolve it at (default origin/main)")
    s.set_defaults(fn=cmd_atrev)

    s = sub.add_parser("split", help="emit only your hunks; refuse rather than guess")
    s.add_argument("file")
    s.add_argument("--mine", action="append", required=True)
    s.add_argument("--theirs", action="append", default=[])
    s.set_defaults(fn=cmd_split)

    a = p.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
