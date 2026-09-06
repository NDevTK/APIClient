#!/bin/bash
# FREEZE A REVISION SO A GATE'S NUMBER BELONGS TO ONE.
#
# CLAUDE.md §Testing: "a gate run from the WORKING TREE measures a tree that no longer exists." This checkout
# is edited continuously by several agents, so a build reads its inputs at different instants and can assemble
# a program NO REVISION EVER CONTAINED — measured once as a struct read one slot early, segfaulting inside
# strcmp in a DFAIL's own order check. `engine/gate_revision.mjs` reports WHICH revision a number belongs to;
# this script is the other half, which makes the answer be one revision at all.
#
#   engine/frozen_snapshot.sh <revision> <lane-name> [command...]
#
# With no command it prints the snapshot path and exits, so a caller can run whatever it likes inside.
#
# NAMED AFTER THE LANE AND THE REVISION, NEVER AFTER ITS ROLE. The scratch directory is shared state too: a
# second agent that picks the same obvious name (`frozen/`, `base/`, `before/`) reaches in and moves the bytes
# the snapshot exists to hold still, which is the working-tree defect arriving through the place chosen to
# escape it. It has happened — one lane's clone was clobbered mid-run and it noticed only because its numbers
# stopped reproducing.
set -e
REV="$1"; LANE="$2"; shift 2 || true
SRC=$(git rev-parse --show-toplevel)
ROOT="${FROZEN_SNAPSHOT_ROOT:-${TMPDIR:-/tmp}/apiclient-frozen}"
mkdir -p "$ROOT"

# RESOLVE EVERY REVISION IN THE PARENT, BEFORE CLONING, AND PASS THE SHA.
# A clone's `origin/*` is built from the source repository's LOCAL branches, not from its remote-tracking ones,
# so inside a snapshot `origin/main` means "the parent's local main" — which in a shared checkout is whatever a
# lane committed and has not pushed. A baseline frozen "at origin/main" can therefore be the very commit you
# are measuring against, and the before/after is then one revision compared with itself: both sides identical,
# every column unchanged, and nothing saying the comparison was empty.
SHA=$(git -C "$SRC" rev-parse --verify "$REV^{commit}")
QSHA=$(git -C "$SRC" rev-parse --verify "$SHA:engine/qjs")
DIR="$ROOT/snap-$LANE-$(echo "$SHA" | cut -c1-8)"

# A LOG IS EVIDENCE AND THE SNAPSHOT IS NOT, so logs are copied out BEFORE anything is deleted — a destroyed
# snapshot once took the only record of a revision's stage output with it, after the numbers had been quoted.
#
# AND WHICH LOGS ARE PRESERVED IS DERIVED, NEVER HAND-NAMED. An earlier version of this cleanup copied the
# build log and the smoke log BY NAME, and a build writes one log PER STAGE — so every other stage's detail
# was destroyed on the next freeze, silently, while the summary line that quoted it survived. That cost the
# per-stage body of an audit whose category counts had already been reported and could no longer be re-read,
# and it is the hand-picked-list defect this project keeps paying for: a stage added later is preserved by
# nobody and nothing says so. Copy the whole directory of logs the build wrote.
for old in "$ROOT"/snap-*; do
  [ -d "$old" ] || continue
  [ "$old" = "$DIR" ] && continue
  # A SNAPSHOT WITH A LIVE READER IS NOT STALE, IT IS IN USE. This loop once deleted a snapshot while that
  # snapshot's OWN gate was still reading it: the run did not fail, it reported hundreds of "No such file or
  # directory" lines and a destroyed measurement arrived looking like a compile result for a corpus. Ask the
  # KERNEL which paths are open rather than guessing from mtime — a corpus run reads its tree in bursts and
  # can look idle for minutes. `fuser -m` is the wrong question: it asks about the FILESYSTEM the directory
  # sits on, so it answers "in use" for every snapshot and frees nothing.
  if { ls -l /proc/*/cwd 2>/dev/null; ls -l /proc/*/fd/* 2>/dev/null; } \
       | sed 's/.*-> //' | grep -qF "$old"; then
    echo "keeping $(basename "$old") — a live process has it open or is cwd'd into it"; continue
  fi
  tag=$(basename "$old" | sed 's/^snap-//')
  find "$old" -name '*.log' -type f 2>/dev/null | while read -r f; do
    cp -n "$f" "$ROOT/EVIDENCE-$tag-$(basename "$f")" 2>/dev/null || true
  done
  rm -rf "$old"
done

rm -rf "$DIR"
git clone --shared -n "$SRC" "$DIR" >/dev/null 2>&1
git -C "$DIR" checkout --detach "$SHA" >/dev/null 2>&1
# A SUBMODULE IS TRACKED AND STILL ARRIVES EMPTY — a clone records the gitlink and populates nothing — and the
# tools GUARD their paths rather than requiring them, so the hole is silent and a resolved-of-total prints as a
# fraction of a population missing its largest member. Provision it at the commit the SUPERPROJECT records,
# never at whatever the working checkout happens to hold.
git clone --shared -n "$SRC/engine/qjs" "$DIR/engine/qjs" >/dev/null 2>&1
git -C "$DIR/engine/qjs" checkout --detach "$QSHA" >/dev/null 2>&1

mkdir -p "$DIR/engine/.work"
rm -rf "$DIR/engine/.work/emsdk" "$DIR/engine/.work/obj" "$DIR/engine/.work/wpt"
# SYMLINK A PURE TOOLCHAIN; give a PRIVATE, EMPTY directory to anything the build WRITES TO.
# The object cache reads like part of the toolchain and is part of the MEASUREMENT: sharing it makes every
# "frozen" snapshot a lie in both directions at once — the build reads objects compiled from other revisions
# and writes its own back for the next snapshot to read, which is the one input still moving under a gate whose
# entire product is a number belonging to a revision. A copy is not a fix either, because a copy carries
# exactly the stale objects that cause it. The price is a full compile per gate run and it is the right price.
[ -d "$SRC/engine/.work/emsdk" ] && ln -s "$SRC/engine/.work/emsdk" "$DIR/engine/.work/emsdk"
[ -d "$SRC/engine/.work/wpt" ]   && ln -s "$SRC/engine/.work/wpt"   "$DIR/engine/.work/wpt"
mkdir -p "$DIR/engine/.work/obj"

# GATE ON THE COUNT — a check whose result nothing branches on is a comment with a pipeline in it.
QN=$(ls -A "$DIR/engine/qjs" 2>/dev/null | wc -l)
if [ "$QN" -lt 50 ]; then
  echo "REFUSING: engine/qjs has $QN entries — the snapshot has a hole where the engine's own fork lives." >&2
  exit 1
fi

# PRINT WHERE THE EVIDENCE GOES, because a reader who wants a cross-revision reading globs for it and a
# glob that misses half the runs UNDER-SAMPLES SILENTLY — the same shape as a truncated search feeding a scope
# list, arriving in the data instead of the query. Runs made under different roots accumulate in different
# directories and nothing in a per-run log says so, so the path is stated rather than assumed.
echo "evidence   $ROOT/EVIDENCE-*.log  (per-revision logs kept when a snapshot is reclaimed)"
echo "snapshot   $DIR"
echo "revision   $SHA"
echo "engine/qjs $QSHA  ($QN entries)"
echo "load       $(cat /proc/loadavg)"
[ $# -eq 0 ] && exit 0
cd "$DIR"
"$@"
