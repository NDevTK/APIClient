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
# A SNAPSHOT SURVIVES BETWEEN THE COMMANDS ITS OWNER RUNS AGAINST IT. That sentence is the contract, it is new,
# and every one of the three ways it used to be false cost a lane a measurement in a single session:
#   (1) A PEER'S FREEZE TOOK IT. The cleanup loop below reclaimed every snapshot no process was standing in,
#       on every freeze, whether or not this machine needed one byte of it — reproduced here with 2.5 GB free
#       and no output naming the deletion. A snapshot freshly handed to a caller has nobody standing in it.
#   (2) A SHORT COMMAND HOLDS NOTHING EITHER, so passing the command in — which the line above prescribes —
#       protected a lane only while that command ran, and a SEQUENCE of short commands was exposed in every
#       gap between them.
#   (3) ITS OWNER'S OWN NEXT FREEZE TOOK IT, which is the largest of the three and needed no peer at all: the
#       `rm -rf "$DIR"` before the clone asked nothing and carried nothing out, so a lane running two short
#       commands through the prescribed form re-cloned between them and lost the build the first one made.
#       Measured: artifact and object directory both gone on the second invocation, same lane, same revision.
# Two lanes independently invented workarounds — a private `FROZEN_SNAPSHOT_ROOT`, and folding every check
# into one long command — which is the sign that the CONTRACT was missing rather than that they were careless.
#
# WHAT A MISSING SNAPSHOT DEGRADES INTO IS THE WORST OF IT, AND IT IS NOT THIS SCRIPT'S `cd`. A third lane
# reported its commands running in `/home/user/APIClient` after a `cd` failed, printing `0 warnings` for the
# one tree a freeze exists to escape — a wrong measurement that renders exactly like a right one. The `cd`
# BELOW is not that path and was measured not to be: under `set -e` a failing `cd` exits 1 with the shell's own
# message and `"$@"` never runs (checked directly, and again in this script's own tail shape). The leak is a
# `cd` the CALLER does in its own shell, where a failure leaves the shell in the working tree and the next
# command reads plausibly — CLAUDE.md's `<check>; <act>` non-check, arriving in a tool-call shell whose working
# directory persists. No assert is added here for it, because an assert whose two sides cannot disagree is a
# non-check with a reassuring transcript: `set -e` already gates it. What is done instead is to remove the
# REASON a caller cd's itself, which was (1)-(3) above, and to print the invocation that needs no cd at all.
#
# NAMED AFTER THE LANE AND THE REVISION, NEVER AFTER ITS ROLE. The scratch directory is shared state too: a
# second agent that picks the same obvious name (`frozen/`, `base/`, `before/`) reaches in and moves the bytes
# the snapshot exists to hold still, which is the working-tree defect arriving through the place chosen to
# escape it. It has happened — one lane's clone was clobbered mid-run and it noticed only because its numbers
# stopped reproducing.
#
# AND THE SAME ARGUMENT DOES NOT REACH ONE LEVEL UP TO THE ROOT, WHICH IS THE OBVIOUS FIX AND IS REFUSED HERE.
# `FROZEN_SNAPSHOT_ROOT` already exists and a lane pointing it at its own scratch directory is isolated from
# every peer for free — which is the right ESCAPE HATCH and the wrong DEFAULT, because this file's own closing
# paragraph says why: runs made under different roots accumulate in different directories, nothing in a per-run
# log says so, and a reader globbing for a cross-revision reading UNDER-SAMPLES SILENTLY. A per-lane default
# makes that the normal case rather than the exception. It also breaks the two preservation rules below, which
# are keyed the way they are precisely because lanes SHARE a root: one revision has one artifact, so a second
# lane freezing the same SHA is a `cp -n` no-op instead of a duplicate, and the EVIDENCE logs beside it already
# run to tens of megabytes each. Isolating the lanes would buy safety by making the evidence pool per-lane,
# which is the cost this project is least able to afford. And it would not have closed (3) at all, since that
# one needs no peer. The sharing is deliberate; what was wrong is what the sharing LICENSED.
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
DIR="$ROOT/snap-$LANE-$(echo "$SHA" | cut -c1-8)"

# A SNAPSHOT WITH A LIVE READER IS NOT STALE, IT IS IN USE. This question once went unasked and the loop below
# deleted a snapshot while that snapshot's OWN gate was still reading it: the run did not fail, it reported
# hundreds of "No such file or directory" lines and a destroyed measurement arrived looking like a compile
# result for a corpus. Ask the KERNEL which paths are open rather than guessing from mtime — a corpus run reads
# its tree in bursts and can look idle for minutes. `fuser -m` is the wrong question: it asks about the
# FILESYSTEM the directory sits on, so it answers "in use" for every snapshot and frees nothing.
#
# IT IS A FUNCTION NOW BECAUSE THERE ARE TWO PLACES THAT DELETE A SNAPSHOT AND ONLY ONE OF THEM ASKED. The
# other is this script's own delete of its target before cloning, which destroyed whatever stood there without
# asking anything at all — the shape CLAUDE.md records as two concurrent freezes into one path whose
# `git clone`s destroyed each other's pack files and left a tree PRESENT ENOUGH TO BUILD FROM with its `.git`
# absent. One question, asked at every site that deletes, is the fix; a second correct answer written beside
# the first is how two sites drift.
snapshot_is_live() {
  { ls -l /proc/*/cwd 2>/dev/null; ls -l /proc/*/fd/* 2>/dev/null; } \
    | sed 's/.*-> //' | grep -qF "$1"
}

# LAST USE IS THE NEWEST MTIME ANYWHERE IN THE TREE, and it is the one thing a short command leaves behind.
# This is NOT the mtime question the liveness gate above rightly refuses: that one asks "is someone reading
# this RIGHT NOW", which mtime cannot answer because a reader writes nothing. This asks "when was this last
# touched at all", which is the only ordering key available and which a build, a checkout and a gate all move.
# The directory's own mtime is not it — a build writing into `engine/.work/obj` does not touch the top level.
# Measured on a live snapshot: top-level mtime 1789649354, deepest mtime 1789649435, eighty-one seconds apart
# and the deeper one the true answer. Costs ~15 ms on a populated snapshot, so it is affordable per candidate.
# A tree with no entries answers 0 and sorts first, which is the right answer for a directory holding nothing.
snapshot_last_use() {
  find "$1" -printf '%T@\n' 2>/dev/null | awk 'BEGIN{m=0} $1>m{m=$1} END{printf "%d\n", m}'
}

snapshot_size_mb() { du -sm "$1" 2>/dev/null | cut -f1 || echo 0; }
root_avail_mb()    { df -Pm "$ROOT" | awk 'NR==2{print $4}'; }

# A LOG IS EVIDENCE AND THE SNAPSHOT IS NOT, so logs are copied out BEFORE anything is deleted — a destroyed
# snapshot once took the only record of a revision's stage output with it, after the numbers had been quoted.
#
# AND WHICH LOGS ARE PRESERVED IS DERIVED, NEVER HAND-NAMED. An earlier version of this cleanup copied the
# build log and the smoke log BY NAME, and a build writes one log PER STAGE — so every other stage's detail
# was destroyed on the next freeze, silently, while the summary line that quoted it survived. That cost the
# per-stage body of an audit whose category counts had already been reported and could no longer be re-read,
# and it is the hand-picked-list defect this project keeps paying for: a stage added later is preserved by
# nobody and nothing says so. Copy the whole directory of logs the build wrote.
preserve_evidence() {
  local old="$1" tag arev
  tag=$(basename "$old" | sed 's/^snap-//')
  find "$old" -name '*.log' -type f 2>/dev/null | while read -r f; do
    cp -n "$f" "$ROOT/EVIDENCE-$tag-$(basename "$f")" 2>/dev/null || true
  done
  # A BUILT ARTIFACT IS EVIDENCE TOO, AND THE RULE ABOVE IS WRITTEN ABOUT LOGS ALONE.
  # The argument above — a log is evidence and the snapshot is not, so it is copied out BEFORE anything is
  # deleted — generalises to the artifact WORD FOR WORD and the implementation did not: the
  # `find -name '*.log'` preserves the record of a run and discards the PROGRAM that run was about. That
  # program is a fact about a revision, it is what every later measurement drives, and it costs a full
  # compile to reproduce. Measured, and it cost exactly that: a build was taken at one revision, its
  # artifact left in the snapshot, and the NEXT freeze — by the same agent, minutes later, for the corpus
  # pass that artifact was built for — reclaimed it. Nothing was live in it, so the liveness gate above was
  # right to let it go; what was missing is that the gate decides WHETHER to delete and nothing decided
  # what to CARRY OUT first.
  # KEYED BY REVISION AND NOT BY LANE, because one revision has one artifact: two lanes freezing the same
  # SHA preserve the same bytes, and `cp -n` then makes the second a no-op rather than a duplicate. A
  # lane-keyed name would accumulate one copy per lane for no added evidence, which is the cost this
  # project is least able to afford — the EVIDENCE logs beside it already run to tens of megabytes each.
  # THE STAMP TRAVELS WITH IT OR THE BYTES ARE WORTHLESS: an artifact whose revision a reader cannot state
  # is exactly the unstamped artifact §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES says produces no number at all.
  if [ -d "$old/extension/lib/qjs" ]; then
    arev=$(basename "$old" | sed 's/.*-//')
    if [ ! -d "$ROOT/ARTIFACT-$arev" ]; then
      mkdir -p "$ROOT/ARTIFACT-$arev"
      cp -pn "$old/extension/lib/qjs/"* "$ROOT/ARTIFACT-$arev/" 2>/dev/null || true
      echo "preserved artifact of $arev -> $ROOT/ARTIFACT-$arev ($(du -sh "$ROOT/ARTIFACT-$arev" 2>/dev/null | cut -f1))"
    fi
  fi
}

# A RECLAMATION LEAVES A NAMEABLE CAUSE BEHIND IT. What this destroys is a path this same script PRINTED to
# somebody, so its absence surfaces in their next command as a missing directory and arrives, per the incident
# above, looking like a compile result rather than like a deletion. One appended line costs nothing and turns
# that into a question with an answer: who reclaimed it, when, and under what pressure.
reclaim_snapshot() {
  local old="$1" why="$2"
  preserve_evidence "$old"
  rm -rf "$old"
  echo "$(date -Is) reclaimed $(basename "$old") by freeze of ${LANE} at ${SHA:0:8} — $why" >> "$ROOT/RECLAIMED.log"
  echo "reclaimed $(basename "$old") — $why"
}

# OUR OWN TARGET IS ANSWERED BY THE SAME QUESTIONS AS ANYBODY ELSE'S, AND IT USED TO BE ANSWERED BY NONE.
# A bare `rm -rf "$DIR"` stood here. It asked nothing about who was standing in that directory and carried
# nothing out of it, so the two rules above — keep what is in use, carry the evidence out first — held for a
# PEER's snapshot and not for the caller's own, which is one question answered two ways depending on who asks.
# Measured: a lane running two short commands through this script's own prescribed form lost the artifact and
# the object directory the first one built, on the second invocation, with no peer involved and nothing said.
#
# REUSE IS THE ANSWER AND IT IS A CONTENT QUESTION, NEVER A PATH ONE. A directory standing at this path is
# worth keeping exactly when it IS what we were about to make: HEAD at the SHA we resolved, no tracked file
# modified, and the engine's fork populated. All three are read from the tree rather than assumed, which is
# why a half-built or re-pointed directory still gets replaced. The build's outputs do not spoil the check —
# `extension/lib/qjs` is untracked and `engine/.work/obj` is ignored, both verified — so a fully built snapshot
# reads as clean and reuse is available precisely when it is worth the most.
#
# AND REUSING THE OBJECT DIRECTORY DOES NOT BREAK THE RULE BELOW THAT MAKES IT PRIVATE AND EMPTY, because that
# rule's argument is about objects compiled from OTHER REVISIONS leaking between snapshots. These are this
# snapshot's own objects, compiled from these sources, at this SHA, shared with nobody. Wiping them here would
# destroy the one thing reuse exists to keep and would put every lane back on the pattern that caused (3).
#
# LIVE IS A REFUSAL RATHER THAN A DELETE, because the only way this path is live is that a second freeze is
# already running into it, which is the pack-file-destroying incident exactly.
REUSE=""
if [ -e "$DIR" ]; then
  if snapshot_is_live "$DIR"; then
    echo "REFUSING: $DIR is open or cwd'd by a live process — a second freeze into a path a first is still" >&2
    echo "  using is how two clones destroy each other's pack files. Wait for it, or use another lane name." >&2
    exit 1
  fi
  HEAD_THERE=$(git -C "$DIR" rev-parse HEAD 2>/dev/null || echo none)
  DIRTY=$(git -C "$DIR" status --porcelain --untracked-files=no 2>/dev/null || echo dirty)
  POP=$(ls -A "$DIR/engine/qjs" 2>/dev/null | wc -l)
  if [ "$HEAD_THERE" = "$SHA" ] && [ -z "$DIRTY" ] && [ "$POP" -ge 50 ]; then
    REUSE=yes
    echo "reusing    $DIR — already at $SHA, no tracked file modified, $POP entries in engine/qjs"
  else
    echo "replacing  $DIR — HEAD $HEAD_THERE, $([ -n "$DIRTY" ] && echo "tracked files modified" || echo "tracked tree clean"), engine/qjs $POP entries"
    preserve_evidence "$DIR"
    rm -rf "$DIR"
  fi
fi

# RECLAIM BECAUSE THE DEVICE IS SHORT, NEVER BECAUSE A FREEZE HAPPENED.
# The loop that stood here deleted EVERY snapshot no process was standing in, on every freeze, whether or not
# this machine needed one byte of it — measured while reproducing the reported defect: a peer's snapshot was
# destroyed with 2.5 GB free, silently, for nothing. That is not a reclamation policy, it is a reflex, and the
# question it was really answering ("do I need this disk?") had never been asked.
#
# A SNAPSHOT IS RE-DERIVABLE AND THAT IS WHAT MAKES SHEDDING ONE HONEST. It is a `git clone --shared` plus a
# detached checkout at a named SHA — seconds, and the SHA names it exactly — so CLAUDE.md's third category
# applies: neither a cap nor a floor, but work whose RECIPE outlives its bytes, where shedding converts
# storage into recomputation and truncates nothing. What is NOT re-derivable is a run in progress and the
# evidence a run produced, which is why those two are answered FIRST and separately: the liveness gate keeps
# the one, and `preserve_evidence` carries the other out before anything goes.
#
# THE ORDER IS LEAST-RECENTLY-USED AND IT IS WHAT CLOSES THE PEER FORMS OF THE DEFECT BY CONSTRUCTION RATHER
# THAN BY A TIMEOUT. Both populations at risk sort NEWEST: a snapshot just handed to a create-then-use caller
# was touched by its own clone, and a snapshot between two short commands was touched by the first of them. A
# snapshot a lane abandoned two days ago has nothing newer than two days in it and sorts first. Age never
# LICENSES a deletion here — it only decides who answers first when the device is short, which is the
# tiebreak-on-what-leaves-first shape rather than a bound. There is deliberately NO age floor: the floor's N
# would be a guess at how long a lane thinks between two tool calls, and it fails in the destroying direction
# exactly when a lane is slow, which is the expensive one.
#
# AND THERE IS DELIBERATELY NO CLAIM FILE. A claim would record the lane, the revision and a creation time;
# the directory NAME already states the first two and the filesystem states the third, so every field of it is
# a second copy of something readable — and its PID would be dead the moment the creating call returned, which
# is the whole population this is about. A mechanism that answers nothing the filesystem does not already
# answer is the write-with-no-reader this project keeps paying for.
#
# WHAT THIS COSTS, STATED RATHER THAN HIDDEN: with room on the device, nothing is reclaimed and snapshots
# accumulate — which is the direction chosen on purpose, because a snapshot wrongly kept costs disk and one
# wrongly destroyed costs a measurement that can arrive looking like a result. When the device IS short and
# every candidate is somebody's, the least-recently-used one still goes; that is the case where something must
# give, and what gives is re-derivable and has had its evidence carried out first.
NEED="${FROZEN_SNAPSHOT_HEADROOM_MB:-}"
CAND=$(while IFS= read -r d; do
         [ -d "$d" ] || continue
         [ "$d" = "$DIR" ] && continue
         printf '%s\t%s\t%s\n' "$(snapshot_last_use "$d")" "$(snapshot_size_mb "$d")" "$d"
       done < <(find "$ROOT" -maxdepth 1 -name 'snap-*' -type d 2>/dev/null) | sort -n)
if [ -z "$NEED" ]; then
  # DEFAULTED FROM WHAT THE DEVICE REPORTS, WHICH HERE IS WHAT A SNAPSHOT ACTUALLY COSTS ON IT. The largest
  # one standing is the observed price of the thing about to be made, so the headroom self-corrects upward the
  # first time a full build fills an object directory, instead of being a number somebody typed. The 1024
  # below is only the answer when there is nothing to observe yet, and it is a POLICY INPUT — overridable per
  # run, and at every value MEMBERSHIP of the snapshot set is untouched, which is what keeps it off §NO BOUNDS.
  NEED=$(printf '%s\n' "$CAND" | awk -F'\t' 'BEGIN{m=0} $2>m{m=$2} END{print m+0}')
  [ "$NEED" -lt 1024 ] && NEED=1024
fi
AVAIL=$(root_avail_mb)
echo "reclaim    ${AVAIL}MB free, ${NEED}MB wanted for one snapshot$([ -n "${FROZEN_SNAPSHOT_HEADROOM_MB:-}" ] && echo " (FROZEN_SNAPSHOT_HEADROOM_MB)")"
while IFS=$'\t' read -r lu sz old; do
  [ -n "$old" ] || continue
  [ -d "$old" ] || continue
  if [ "$AVAIL" -ge "$NEED" ]; then
    echo "keeping $(basename "$old") — ${AVAIL}MB free is enough; nothing here is needed"
    continue
  fi
  if snapshot_is_live "$old"; then
    echo "keeping $(basename "$old") — a live process has it open or is cwd'd into it"; continue
  fi
  reclaim_snapshot "$old" "${AVAIL}MB free below the ${NEED}MB one snapshot needs; least recently used, last touched $(date -Is -d "@$lu" 2>/dev/null || echo "$lu")"
  AVAIL=$((AVAIL + sz))
done <<< "$CAND"
if [ "$AVAIL" -lt "$NEED" ]; then
  echo "WARNING: ${AVAIL}MB free after reclaiming everything reclaimable, below the ${NEED}MB a snapshot needs." >&2
  echo "  Proceeding — the clone below reports its own failure rather than this guessing at one." >&2
fi

# THE CLONE AND THE CHECKOUT REPORT THEIR OWN FAILURES. Both used to send stdout AND stderr to /dev/null, so
# under `set -e` a failure here killed this script with a status and NO OUTPUT AT ALL — which is the exact
# shape recorded one paragraph down for the retired second clone, and the shape this whole file exists to
# prevent: a freeze that never happened, reported by whoever ran it as a snapshot that "did not work".
if [ -z "$REUSE" ]; then
  if ! CLONE_ERR=$(git clone --shared -n "$SRC" "$DIR" 2>&1 >/dev/null); then
    echo "REFUSING: git clone --shared into $DIR failed — $CLONE_ERR" >&2; exit 1
  fi
  if ! CO_ERR=$(git -C "$DIR" checkout --detach "$SHA" 2>&1 >/dev/null); then
    echo "REFUSING: checkout --detach $SHA in $DIR failed — $CO_ERR" >&2; exit 1
  fi
fi
# THE SECOND CLONE THAT USED TO STAND HERE IS RETIRED, AND THE ARGUMENT FOR IT IS KEPT BECAUSE A READER WHO
# RE-DERIVES IT WILL RE-ADD IT. It said: a submodule is TRACKED and still arrives EMPTY, since a clone records
# the gitlink and populates nothing, and the tools GUARD their paths rather than requiring them — so the hole
# is silent and a resolved-of-total prints as a fraction of a population missing its largest member. Every
# clause of that was true while `engine/qjs` was a submodule. It is a TREE now, so the superproject clone above
# populates it, and the second clone became a command that could only FAIL — into a directory the first clone
# had already filled. Under `set -e` that failure killed this script with exit 128 and NO OUTPUT AT ALL, which
# is the shape this file exists to prevent: a gate that produces nothing and says nothing, reported by whoever
# runs it as a snapshot that "did not work" rather than as a freeze that never happened.
#
# THE COUNT GATE BELOW STAYS, and its argument changes rather than going with the clone. It no longer guards
# against a gitlink populating nothing; it guards against this script's own provisioning being wrong in any
# future way at all, which is the half that was never specific to submodules. A cheap check whose subject can
# still be empty is worth keeping after the one cause you knew about is gone.
#
# WHAT IS STILL A SUBMODULE IS `engine/qjs/test262`, declared at the ROOT `.gitmodules` with `update = none`,
# and it DOES arrive empty here — deliberately, since it is a corpus and not a source this build compiles. A
# gate that needs it provisions it itself and says so; nothing below claims it is present.

mkdir -p "$DIR/engine/.work"
rm -rf "$DIR/engine/.work/emsdk" "$DIR/engine/.work/wpt"
# SYMLINK A PURE TOOLCHAIN; give a PRIVATE, EMPTY directory to anything the build WRITES TO.
# The object cache reads like part of the toolchain and is part of the MEASUREMENT: sharing it makes every
# "frozen" snapshot a lie in both directions at once — the build reads objects compiled from other revisions
# and writes its own back for the next snapshot to read, which is the one input still moving under a gate whose
# entire product is a number belonging to a revision. A copy is not a fix either, because a copy carries
# exactly the stale objects that cause it. The price is a full compile per gate run and it is the right price.
# EMPTY MEANS EMPTY OF ANOTHER REVISION'S OBJECTS, WHICH IS WHY A REUSED SNAPSHOT KEEPS ITS OWN — see the
# reuse block above; those were compiled from these sources at this SHA and are shared with nobody.
[ -n "$REUSE" ] || rm -rf "$DIR/engine/.work/obj"
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
echo "reclaimed  $ROOT/RECLAIMED.log   (why a snapshot that is gone went, and to whose freeze)"
echo "snapshot   $DIR"
echo "revision   $SHA"
# ENGINE/QJS NO LONGER HAS A REVISION OF ITS OWN. This line used to print the submodule commit the
# superproject pinned, and a reader could quote it as a revision. After the subtree merge the only thing
# `<sha>:engine/qjs` names is a TREE, which is not a revision and must not be printed where one was — so what
# is printed is the population, which is what the gate above actually checked.
echo "engine/qjs $QN entries  (subtree of $SHA; no revision of its own)"
echo "load       $(cat /proc/loadavg)"
if [ $# -eq 0 ]; then
  # PRINT THE INVOCATION THAT NEEDS NO `cd`, because the caller's own `cd` is the one path that degrades into
  # a measurement of the working tree and this script cannot gate a shell it does not own. Passing the command
  # in makes the `cd` this script's, where `set -e` gates it — measured — and the snapshot now survives between
  # two such calls, so the sequence that used to force a caller into cd'ing once is no longer a reason to.
  echo "run it     engine/frozen_snapshot.sh $SHA $LANE <command...>"
  exit 0
fi
cd "$DIR"
"$@"
