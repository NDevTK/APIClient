#!/usr/bin/env python3
"""BOUND A `--paint-dir` OUT OF PROCESS — index every frame, keep one file per
distinct raster payload, delete the rest.

WHY THIS IS NOT A SETTING ON THE ENGINE. After @PERWORLD the engine owes an
image to every world a run MINTS, so a forking real page writes one PAM per
timeline that ends; at 1280x720xRGBA each is 3,686,400 bytes, and a few hundred
worlds is a full disk. CLAUDE.md's answer to that is fixed and this file is
built to it: the response to running out of room is NEVER to make the engine
paint less, because a run that paints fewer worlds is a run that measured fewer
worlds. So the bound lives OUTSIDE the engine, after the bytes are written, and
it is LOSSLESS ABOUT WHAT WAS SEEN: `INDEX.tsv` keeps a row for every complete
frame this ever observed, including the ones whose bytes it then removed, so
"how many worlds did this run paint" is still answerable once the files are
gone. A count that shrinks because an instrument stopped looking is the one
failure this must not have.

A FILE SHORTER THAN ITS OWN HEADER SAYS IT SHOULD BE IS MID-WRITE AND IS NEVER
TOUCHED, WHICH IS THE WHOLE OF THE RACE. `abi_paint` writes the PAM header, then
`fwrite`s the payload, so a file caught between those two calls is a real file
at the right name with a short body — and digesting one would hash a picture
that does not exist yet, while deleting one would remove a frame the engine is
still writing. The header states WIDTH, HEIGHT and DEPTH, so the expected length
is computed FROM THE FILE rather than from a viewport constant this script would
otherwise have to keep in step with the engine's. A short file is skipped
entirely: not indexed, not digested, not removed, and retried next pass.

DEDUPLICATION IS BY PAYLOAD AND NEVER BY WORLD NAME. Two worlds that render the
same picture are the interesting negative result — it is what "the fork did not
reach the pixels" looks like — so collapsing them must not lose the fact that
BOTH were painted. The index records both rows with the same digest and
`kept=0` on the second; only the bytes go.

POSITIVE CONTROL, because an instrument that has never been shown rejecting
anything has calibrated nothing. Build a directory holding two distinct
payloads, one exact duplicate, and one truncated file; a correct pass deletes
the duplicate, leaves the truncated file untouched AND unindexed, and writes
three index rows. Both directions of the check speak in that one run.

usage: python3 testing/pamprune.py <paint-dir> [poll-seconds]
"""
import sys, os, time, hashlib, pathlib

def parse(path):
    """(header_len, expected_payload, comments) or None if not yet a PAM."""
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
    except OSError:
        return None
    k = head.find(b"ENDHDR\n")
    if k < 0:
        return None                      # header itself still being written
    hlen = k + len(b"ENDHDR\n")
    meta, comments = {}, []
    for line in head[:k].decode("utf-8", "replace").splitlines():
        if line.startswith("#"):
            comments.append(line[1:].strip())
        elif " " in line:
            a, _, b = line.partition(" ")
            meta[a] = b.strip()
    try:
        exp = int(meta["WIDTH"]) * int(meta["HEIGHT"]) * int(meta["DEPTH"])
    except (KeyError, ValueError):
        return None
    return hlen, exp, comments

def digest(path, hlen, exp):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(hlen)
        got = 0
        while got < exp:
            b = f.read(1 << 20)
            if not b:
                return None, got
            h.update(b); got += len(b)
    return h.hexdigest(), got

def main():
    d = pathlib.Path(sys.argv[1])
    idx = d / "INDEX.tsv"
    period = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    exemplar = {}                        # payload digest -> path kept
    seen = set()                         # (name, digest) already indexed
    if not idx.exists():
        idx.write_text("# t\tfile\tdigest\tbytes\tkept\tworld\tforced\n")
    while True:
        for p in sorted(d.glob("*.pam")):
            try:
                st = p.stat()
            except OSError:
                continue
            pr = parse(p)
            if pr is None:
                continue                 # mid-write: leave it entirely alone
            hlen, exp, comments = pr
            if st.st_size < hlen + exp:
                continue                 # mid-write: payload short, never touch
            dg, got = digest(p, hlen, exp)
            if dg is None or got < exp:
                continue                 # raced the writer; try again next pass
            key = (p.name, dg)
            world = next((c[len("world "):] for c in comments
                          if c.startswith("world ")), "?")
            forced = next((c for c in comments if c.startswith("the walk ")), "?")
            if key in seen:
                # unchanged since last pass; still drop it if a twin is kept
                if exemplar.get(dg) not in (None, str(p)):
                    try: p.unlink()
                    except OSError: pass
                continue
            seen.add(key)
            keep = dg not in exemplar
            if keep:
                exemplar[dg] = str(p)
            with idx.open("a") as f:
                f.write("%.3f\t%s\t%s\t%d\t%d\t%s\t%s\n"
                        % (time.time(), p.name, dg[:16], st.st_size,
                           1 if keep else 0, world, forced[:120]))
            if not keep:
                try: p.unlink()
                except OSError: pass
        sv = os.statvfs(str(d))
        free_mb = sv.f_bavail * sv.f_frsize // (1 << 20)
        print("[prune] frames=%d distinct=%d kept=%d freeMB=%d"
              % (len(seen), len(exemplar), len(exemplar), free_mb), flush=True)
        if free_mb < 400:
            print("[prune] FREE SPACE BELOW 400MB — reporting, not softening", flush=True)
        time.sleep(period)

main()
