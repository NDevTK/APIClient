#!/bin/bash
# ONE CENSUS PASS: every site in the list, one virgin browser each, against a FROZEN ENGINE ARTIFACT.
# Emits one JSON row per line; feed it to report.mjs.
# FROZEN here is the LANE'S OWN COPY OF extension/, whose sha256 this script prints -- never a frozen copy
# of the SITE, which is deleted and which the transport paragraph below is about. The two senses shared one
# word in this header while both existed, and only one of them survives.
#
#   LANE=/tmp/mylane ./run.sh pass1                      # sites.tsv
#   LANE=/tmp/mylane ./run.sh pass1 excalidraw           # one site
#   LANE=/tmp/mylane SITES=apps.tsv ./run.sh q5          # the app-page census
#
# THE BYTES COME FROM THE NETWORK, AT THE MOMENT OF THE RUN, AND THERE IS ONE TRANSPORT. `AT` USED TO SELECT
# one: `AT=frozen` served each site from a committed mirror through serve-faithful.mjs, `AT=live` drove the
# row's own URL. The mirror, that server and every line here that existed only for them are DELETED, so
# there is nothing left to select. `AT` is still READ, and ONLY so a stale caller is TOLD rather than
# silently handed a different measurement than it asked for; that refusal is the whole of why `AT` survives.
#
# THE ONE-DRIVER ARGUMENT IS KEPT, BECAUSE IT IS WHY THERE IS STILL ONE SCRIPT HERE AND NOT TWO. Everything
# that makes a row trustworthy — the lane, the virgin browser, the proof that the browser is ours, the
# pass-qualified transcript — belonged to NEITHER transport, which is why A SECOND SCRIPT FOR THE LIVE CASE
# IS WHAT THIS REPLACED. The two copies had already drifted: the /tmp one had `--noproxy 127.0.0.1` on its
# identity check and this one did not, so on a box exporting `http_proxy` they were asking two different
# questions and only one of them was about our browser. That is what a second driver costs, and the cost is
# live for the next person who wants one.
#
# AND THE RULE THIS HEADER CITED FOR FROZEN BYTES IS RETIRED, SO ITS REPLACEMENT IS NAMED HERE RATHER THAN
# LEFT TO BE RE-DERIVED — the reasoning for a mirror is sound and the next reader will reach it again. It
# read: CLAUDE.md §Testing, a before/after belongs on FROZEN BYTES where the only thing that changed is the
# engine, with live sites kept for DISCOVERING signatures. The project owner has RETIRED that: a page's
# scripts and styles are COMPUTED AND FETCHED AT RUNTIME, so a frozen copy is a program no visitor is
# served, and this repository may not carry one anyway. WHAT REPLACES IT NEEDS NOTHING THIS SCRIPT LACKS — a
# run COUNT and a SPREAD rather than one number, compared on what does not move with reach: a crash's
# IDENTITY, a conservation identity read WITHIN one sample, a count that cannot be true. So one pass settles
# whether a site aborts and on what, and settles nothing about how much work got done; the endpoint column
# is noise on any site that aborted, and report.mjs says so.
#
# THE LANE IS THE WHOLE POINT AND IT IS REQUIRED, NOT DEFAULTED. It is a directory holding a COPY of
# `testing/harness.js` and a COPY of `extension/`, because harness.js derives EXT_DIR from its own location
# and Chrome's extension id from that path. Copying the pair is what makes the browser provably load an
# artifact nobody can rebuild under you: the shared checkout's qjs.wasm was replaced in the middle of an
# earlier pass, and the rows either side of it were two different programs wearing one corpus. Pointing this
# at the live checkout is therefore not a convenient shortcut, it is the defect, so there is no default.
#
#     mkdir -p $LANE/testing && cp testing/harness.js $LANE/testing/ && cp -a extension $LANE/
#
# ONE VIRGIN BROWSER PER SITE. An engine abort tears down the renderer pool, and the NEXT site in the same
# browser then reports counters belonging to a poisoned pool -- which is how an earlier census read runs=0
# for three healthy origins and called them dead. `restart` (never `start`) also wipes IndexedDB and the V8
# code cache, so no row inherits the previous row's frontier.
#
# WHAT THE BROWSER REQUESTED IS ON THE ROW NOW, NOT IN A SIDE FILE. The deleted fixture server 404'd loudly
# for a resource the mirror lacked and that count went to logs/<id>.serve, which was the only record of what
# the browser actually ASKED FOR — the half a census row could not reconstruct, and what told a row with a
# thin document apart from a row whose engine learned nothing. THE MISS HALF HAS NO LIVE ANALOGUE AND NEEDS
# NONE: a 404 from the real site is a fact about the site rather than about a capture. THE REQUESTED HALF IS
# NOW THE ENGINE'S OWN RECORD AND IS BETTER PLACED — `egressAsked` and `egressDeclined` ride the census row
# itself (site.mjs, where both keep ABSENT apart from zero), so the discrimination outlived the server and
# no longer depends on a path the next pass can overwrite.
set -u
# WHERE THE CORPUS IS, AND IT IS OVERRIDABLE SO A LANE CAN RUN A PRIVATE COPY OF THIS SCRIPT.
# BASH STREAMS ITS OWN SCRIPT. It reads the file INCREMENTALLY as it executes, so an edit to this file
# while a pass is running can make the running shell resume at a byte offset inside a DIFFERENT construct
# — the same defect as the streamed site list a few lines down, one level up, and against the one input a
# lane could not copy.
# MEASURED, AND THE COORDINATOR CAUSED IT: a census lane's transcript carries
#   ./run.sh: line 145: syntax error near unexpected token `done'
# immediately after a site banner, and that invocation wrote TWO census rows — the site, and a second with
# an EMPTY id and `nav: goto:net::ERR_CONNECTION_REFUSED`. The lane re-hashed its own list and proved it
# contains no empty first field, so the empty id came from this script jumping, not from its input. It
# checked the blast radius rather than assuming: one row corrupted, the rest valid, and it deleted and
# re-queued the casualty.
# NODE IS NOT LIKE THIS AND THE ASYMMETRY IS THE USEFUL PART: node reads a module WHOLE at import, so
# site.mjs, report.mjs, list.mjs and fetch.mjs are safe against a mid-edit in a way a bash script is not.
# A LANE CANNOT DEFEND AGAINST IT EXCEPT BY COPYING THIS FILE, and a copy used to be impossible because
# this line derived the corpus path from the SCRIPT'S OWN LOCATION — so a copy looked for site.mjs beside
# itself and found nothing. The override is therefore not a convenience: it is a NECESSARY part of what makes
# a private copy work, and it is NOT THE WHOLE OF IT — this line used to claim it was, and a lane found the
# claim false by acting on it: site.mjs is ESM, ESM ignores `NODE_PATH`, so a private CORP resolves none of the
# driver's imports however this variable is set. The resolution check further down is the other part, and it
# refuses at the origin with the remedy named. The retired wording is kept because a reader who re-derives the
# override's purpose will re-derive the over-claim with it. `LANE` already carries harness.js and
# extension/ for exactly this reason; this closes the last shared input.
#     cp testing/corpus/run.sh $LANE/run.sh && CORP=/home/user/APIClient/testing/corpus $LANE/run.sh …
CORP="${CORP:-$(cd "$(dirname "$0")" && pwd)}"
[ -f "$CORP/site.mjs" ] || { echo "CORP=$CORP holds no site.mjs — a private copy of this script must be "\
                                 "given CORP explicitly; it is no longer derived from \$0 alone"; exit 2; }
: "${LANE:?set LANE to a directory holding testing/harness.js and extension/ -- see the comment above}"
[ -f "$LANE/testing/harness.js" ] || { echo "no $LANE/testing/harness.js"; exit 2; }
[ -f "$LANE/extension/lib/qjs/qjs.wasm" ] || { echo "no $LANE/extension/lib/qjs/qjs.wasm"; exit 2; }

LABEL=${1:-pass}
ONLY=${2:-}
# SITES names the list to walk (default the corpus). A repair pass over the handful of sites whose fixture
# data was rebuilt is a different list, not a different script.
SITES=${SITES:-sites.tsv}
case "$SITES" in /*) ;; *) SITES=$CORP/$SITES;; esac
[ -f "$SITES" ] || { echo "no site list at $SITES"; exit 2; }
# AN UNKNOWN `AT` IS FATAL RATHER THAN IGNORED, AND THAT ARGUMENT OUTLIVED THE PARAMETER IT WAS WRITTEN FOR.
# With two transports, a typo silently taken as one of them was a census that measured the other corpus
# under this one's label -- a row no counter in the output could contradict.
# One transport does not retire that reason, it NARROWS it: the refusal below is the same refusal over a
# smaller set, and the paragraph under it is the record of what the set used to hold.
# THERE IS ONE PLACE THE BYTES COME FROM AND IT IS THE NETWORK. `AT=frozen` served a committed copy of
# other people's sites through serve-faithful.mjs; that copy and that server are deleted, so the only
# transport left is the row's own URL. A page's scripts and styles are FETCHED AT RUNTIME. `AT` is still
# read so a stale caller is TOLD rather than silently given a different measurement than it asked for.
AT=${AT:-live}
if [ "$AT" != "live" ]; then
  echo "run.sh: AT=$AT is gone. The frozen transport served a committed mirror of real sites, which this" >&2
  echo "        repository no longer carries. Drive the row's own URL with AT=live." >&2
  exit 2
fi
# AND THERE IS NO SECOND VALIDATION UNDER THAT REFUSAL. A `case "$AT" in frozen|live) ;; *) exit 2;; esac`
# stood here and COULD NOT FIRE: the guard above has already refused everything but `live`, so both of its
# arms were unreachable -- a NON-check with a reassuring transcript, which is CLAUDE.md
# §AN-ASSERT-WHOSE-TWO-SIDES-CANNOT-DISAGREE. It is deleted rather than narrowed, because a validation whose
# whole population one line above it has already refused has no narrower form.
PORT=${HARNESS_PORT:-9451}
# THERE ARE TWO MODULE SYSTEMS HERE AND ONE VARIABLE SERVES EXACTLY ONE OF THEM. This script runs TWO node
# programs with different resolution rules, and a fix that treats them as one breaks the other:
#   - `site.mjs` is ESM — five `import` statements — and ESM RESOLUTION IGNORES `NODE_PATH` ENTIRELY, resolving
#     from the importing FILE's own directory upward. It is run with CORP as its working directory.
#   - `testing/harness.js` is CJS — six `require`s, `require('puppeteer')` among them — and CJS resolution DOES
#     consult `NODE_PATH`. It is run with LANE as its working directory, and a LANE is a private directory that
#     by construction holds no node_modules.
# SO THE EXPORT BELOW IS LOAD-BEARING FOR THE HARNESS AND INERT FOR THE DRIVER, and the two need two different
# things: the harness needs the variable, and the driver needs node_modules reachable from CORP by ordinary ESM
# resolution. Neither substitutes for the other.
#
# THIS PARAGRAPH SAID `NODE_PATH IS A NO-OP FOR THIS DRIVER AND THE LINE THAT SET IT CLAIMED OTHERWISE`, AND IT
# DELETED THE EXPORT ON THAT GROUND. The first half was measured and is true; the sentence that followed it —
# "so the export below was protecting CJS and nothing this script runs is CJS" — WAS AN ABSOLUTE ABOUT EVERY
# PROGRAM THIS SCRIPT RUNS, made after reading only one of them, and `harness.js` refutes it. THE DELETION
# BROKE EVERY DRIVE: three consecutive passes answered `harness restart exited 1` within one second each, and
# the restart log's cause is `Error: Cannot find module 'puppeteer'` out of CJS's own loader. It is CLAUDE.md
# §AN-OVER-CLAIM-IS-REFUTABLE committed in a commit message, and the refutation arrived by acting on it.
# THE RETIRED WORDING IS KEPT BECAUSE THE MEASUREMENT BEHIND IT IS CORRECT AND A READER WILL RE-DERIVE IT: the
# ESM half really does ignore this variable, so a reader who measures that half and stops will delete the line
# again. What was missing was never a better measurement — it was asking the SECOND program the SAME question.
#
# AND THE PROBE THAT REPLACED IT PASSED WHILE THE DRIVE WAS BROKEN, which is the part worth more than the
# incident. It asked the DRIVER's resolution question from CORP, correctly, with an armed control — and it was
# structurally silent about the HARNESS, because a probe for one program's resolution says nothing about
# another program's. That is §A-CONTROL-ARMS-ONLY-ON-A-SITE-THE-INSTRUMENT-CAN-JUDGE: the control armed on the
# axis the author was thinking about and the failure was on the axis they had not asked.
# SO THE CHECK IS TWO-SIDED, ONE SIDE PER PROGRAM, EACH ASKED THE WAY THAT PROGRAM ASKS. Neither side can
# satisfy the other and a single green is not a pass. Both are evaluated with the working directory the real
# program gets, and both read their EXIT STATUS DIRECTLY — never through a pipe, because a pipeline hands you
# its last stage's status and this check would then always pass.
# NEITHER WRITES A FILE, and that is a correctness choice rather than tidiness: a probe `.mjs` or `.cjs` leaves
# an untracked file in a SHARED tree if the script is killed between the write and the remove, and putting one
# in a gitignored subdirectory instead asks the question ONE DIRECTORY DEEPER than the real program, adding a
# resolution root the real program does not have — a one-directional bias pointing the wrong way for a refusal
# gate, since it can only turn a real failure into a PASS. `-e` resolves against the CWD for both systems.
# MEASURED, each side with its own armed control: from a scratch directory with no reachable node_modules, the
# ESM form exits 1 with NODE_PATH SET and the CJS form exits 1 with it UNSET and 0 with it SET; in this corpus
# the ESM form exits 0, and from a LANE directory the CJS form exits 0 only because of the export above.
export NODE_PATH=${NODE_PATH:-/home/user/APIClient/node_modules}
if ! ( cd "$CORP" && node --input-type=module -e "import 'puppeteer'" ) >/dev/null 2>&1; then
  echo "CORP=$CORP cannot resolve the DRIVER's imports: site.mjs is ESM and ESM IGNORES NODE_PATH, so a"
  echo "private CORP needs node_modules reachable from it by ORDINARY ESM RESOLUTION. Remedy, either one:"
  echo "    ln -s /home/user/APIClient/node_modules \$(dirname $CORP)/node_modules"
  echo "  or keep CORP at /home/user/APIClient/testing/corpus and give LANE the private harness+extension,"
  echo "  which is what LANE is for and is the shape this script's header describes."
  echo "REFUSED HERE rather than at the first drive: the driver call below pipes stderr through a grep for"
  echo "ROW, so this failure would otherwise arrive as the generic 'driver produced no row' after a browser"
  echo "start and a 360s timeout, for every site in the list."
  exit 2
fi
if ! ( cd "$LANE" && node -e "require('puppeteer')" ) >/dev/null 2>&1; then
  echo "LANE=$LANE cannot resolve the HARNESS's requires: testing/harness.js is CJS and is run with LANE as"
  echo "its working directory, so it needs NODE_PATH (exported above) or node_modules reachable from LANE."
  echo "NODE_PATH is currently: ${NODE_PATH:-<unset>}"
  echo "Remedy: leave the export above alone, or point NODE_PATH at a directory that holds puppeteer."
  echo "REFUSED HERE rather than at the first restart: a failing restart answers 'harness restart exited 1'"
  echo "for ten different causes, and the cause is readable only in logs/<label>-<id>.restart."
  exit 2
fi
export HARNESS_PROFILE=$LANE/prof HARNESS_LOCK=$LANE/harness.lock HARNESS_PORT=$PORT
export HARNESS_EXT_DIR=$LANE/extension CDP=$PORT DWELL=${DWELL:-60000}
OUT=$CORP/census-$LABEL.jsonl
mkdir -p "$CORP/logs"
: > "$OUT"
echo "artifact $(sha256sum "$LANE/extension/lib/qjs/qjs.wasm" | cut -c1-64)"
echo "list $SITES   at $AT"
echo "load at start: $(cut -d' ' -f1-3 /proc/loadavg)"

# THE LIST IS READ ONCE, INTO MEMORY, BEFORE THE LOOP — because `done < "$SITES"` holds an OPEN FD and the
# shell reads INCREMENTALLY, so a rewrite of the list while a pass is running moves the file under the
# offset. This is the same shared-input hazard the LANE exists for, on the one input that was still
# pointing at the shared checkout: the lane copies harness.js and extension/ precisely so nobody can swap
# them mid-pass, and then the site list was read live from a file a peer appends to.
# MEASURED, AND IT COST ELEVEN ROWS: a mirroring lane appended a verdict to one row's third column while a
# census was walking the list. The loop resumed at a byte offset INSIDE the new prose and fed PROSE
# FRAGMENTS in as site ids — one row's id was `pass" AND IT NOW ANSWERS 200. Measured four times…` and the
# next was `c)`. Rows 1-8 were exactly right and 9 onward were not, which is the worst shape: a pass that
# is partly valid.
# WHAT CAUGHT IT WAS report.mjs BEING FATAL on a row its list does not name. A softer instrument would
# have printed a ten-row census with two nonsense rows in it and the pass would have been reported. That
# refusal is the rule earning its keep, and it is the reason this defect is a comment rather than a
# silently wrong number in somebody's report.
# A COPY IS NOT ENOUGH AND THE SNAPSHOT IS NOT THE POINT — what matters is that the read happens ONCE, at
# a known instant, so every row of one pass comes from one version of the list. `mapfile` does that with
# no second file to keep in step, and the count is printed so a pass states how many rows it was handed.
mapfile -t SITE_ROWS < "$SITES"
echo "list read once: ${#SITE_ROWS[@]} line(s) at $(date -u +%H:%M:%S) — rows below come from THIS snapshot of the list, not from re-reads"
for _row in "${SITE_ROWS[@]}"; do
  IFS=$'\t' read -r id url stack <<< "$_row"
  [ -z "$id" ] && continue
  case "$id" in \#*) continue;; esac
  [ -n "$ONLY" ] && [ "$ONLY" != "$id" ] && continue
  echo "=== $(date -u +%H:%M:%S) $id   load $(cut -d' ' -f1 /proc/loadavg)"
  # EVERY PER-PASS ARTIFACT THIS SCRIPT WRITES IS PASS-QUALIFIED, AND THIS RESTART LOG WAS THE LAST ONE THAT
  # WAS NOT. The driver transcript carries `<label>-<id>` for the reason stated at the site.mjs call below;
  # this log carried `<id>` alone, so the next pass owned the same path and a reader sent here by the fatal
  # row below met ANOTHER PASS'S launch under this pass's id.
  # MEASURED ON THE SIBLING THIS SCRIPT NO LONGER HAS, WHICH IS WHY THE INCIDENT OUTLIVES ITS CODE: the
  # deleted frozen transport's serve log was the one artifact written to a single path per site, and a second
  # pass launched seconds after the first finished TRUNCATED it while the lane was copying it aside -- the
  # first pass's log survived as 43 BYTES (its bind line) against the 37203 bytes and 1252 MISS lines the
  # same site produced in the pass that was not raced. The lane had already written that hazard down and lost
  # the data anyway, which is why the rule is not "be careful": A SNAPSHOT TAKEN "AFTER THE PASS" IS NOT
  # PROTECTION, BECAUSE THE NEXT PASS OWNS THE PATH. The only protection is that the path carries the pass,
  # so it does here now, and the serve log's deletion cost the argument nothing.
  ( cd "$LANE" && timeout 240 node testing/harness.js restart "$PORT" ) >"$CORP/logs/$LABEL-$id.restart" 2>&1
  RC=$?
  # A FAILED `restart` IS ITS OWN FATAL AND MUST NOT FALL THROUGH TO THE IDENTITY POLL BELOW, because that
  # poll answers with ONE SENTENCE for TWO STATES that take opposite work: somebody else's browser is on this
  # port (go and find it), and our own launch refused or died before any browser existed (read why it
  # refused). The identity poll cannot tell them apart -- it asks whether the port serves OUR id, and both
  # states answer no -- so with the exit code discarded a reader met the port-collision reading first and
  # went looking for a peer that was never there. `harness.js` states the discriminating cause in its own
  # refusal, and that refusal lands in this log: the only thing missing was anything reading the status.
  # 124 is `timeout`'s, which is the launch taking longer than the window rather than refusing.
  if [ "$RC" -ne 0 ]; then
    echo "{\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"harness restart exited $RC, so no browser of ours was ever launched on $PORT -- read logs/$LABEL-$id.restart, whose last paragraph names the cause; this is NOT the port-collision reading\"}" >> "$OUT"
    continue
  fi
  # CONFIRM THE BROWSER IS OURS BEFORE DRIVING IT. `restart` can report "started" while its Chrome is
  # already gone, and one lane then silently drove another agent's browser and lost a whole pass.
  # AND THE SHAPE IS A REUSED PORT, WHICH IS WHY THIS POLL IS THE RULE AND NOT A PRECAUTION. `PORT` is ONE
  # port for every row of the pass, so a browser that failed to die would still be bound and would answer for
  # the PREVIOUS row -- a row measuring the wrong thing under the right name, which no counter in the census
  # could contradict. The deleted fixture transport had the identical hazard on its own reused port and
  # refused it the same way, by a line only a successful listen could print; this poll is the surviving
  # member of that pair, so the rule lives HERE now. `testing/corpus/control/serve.mjs` cites "run.sh's own
  # rule" that such a line is the proof a server is this run's, which is a live citation of this paragraph:
  # trimming it would leave that file deferring to a sentence this script no longer makes.
  MYID=$(node -e "const c=require('crypto');const h=c.createHash('sha256').update(Buffer.from(process.argv[1],'utf8')).digest('hex').slice(0,32);let i='';for(const x of h)i+=String.fromCharCode(97+parseInt(x,16));console.log(i)" "$LANE/extension")
  # POLLED, NOT ASKED ONCE, AND `--noproxy` BECAUSE THE ANSWER MUST COME FROM *THIS* BOX. `restart` prints
  # "started" when it has spawned Chrome, which is BEFORE the DevTools HTTP endpoint answers -- so a single
  # curl loses a race it was never meant to be in, and the row it kills is written into the census as a fatal
  # measurement rather than the flake it is. Measured: a control run that had ALREADY LAUNCHED (the restart
  # log's own last line named our extension id) was refused by this check one line later. The identity is
  # still the gate; only the number of chances it gets to answer changed. `--noproxy 127.0.0.1` because a box
  # exporting `http_proxy` would otherwise ask a proxy about our loopback browser.
  OURS=
  for _ in $(seq 1 20); do
    curl -s --noproxy 127.0.0.1 --max-time 5 "http://127.0.0.1:$PORT/json/list" | grep -q "$MYID" && { OURS=1; break; }
    sleep 0.5
  done
  if [ -z "$OURS" ]; then
    echo "{\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"port $PORT is not serving our extension $MYID\"}" >> "$OUT"
    continue
  fi
  # THE PASS LABEL GOES TO THE DRIVER, so its transcript is logs/<label>-<id>.log rather than one path per
  # site that the next pass overwrites. Without it every pass's rows are read against the LAST pass's console
  # and a site that ran cleanly in one pass inherits another pass's abort. report.mjs reads the name off the
  # row, so passing it here is what makes a multi-pass census one measurement per (site, pass).
  # THE DRIVER'S OWN OUTPUT IS KEPT, BECAUSE `driver produced no row` IS ONE SENTENCE FOR TEN FAILURES. The
  # grep below selects the ROW and the tee keeps everything, so a run that produced no row can be diagnosed
  # from what it DID say instead of from the absence. That is the §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES pairing
  # a lane paid for: an unresolvable import, a browser that never started, a 360s timeout and a mid-drive
  # abort all rendered as the same string, and the one that had actually happened was readable nowhere.
  DRV=$CORP/logs/$LABEL-$id.driver
  R=$(cd "$CORP" && timeout 360 node site.mjs "$id" "$url" "$LABEL" 2>&1 | tee "$DRV" | grep '^ROW ' | head -1)
  if [ -z "$R" ]; then
    # THE LAST LINE IS CARRIED INTO THE ROW, which is what makes the fatal actionable without opening the file
    # — and the file is named in it either way, because a last line is a sample and not the account.
    LAST=$(tail -3 "$DRV" 2>/dev/null | tr '\n\t"\\' '    ' | tail -c 400)
    R="ROW {\"id\":\"$id\",\"url\":\"$url\",\"fatal\":\"driver produced no row -- its own output is at $DRV and its last lines were: $LAST\"}"
  fi
  echo "${R#ROW }" >> "$OUT"
done

# TEAR DOWN BY THE PID IN *OUR* LOCK, never by a pattern. `pkill -f testing/harness.js` matches every lane's
# harness on a shared checkout, which is the exact way one agent has already killed another's browser.
MYPID=$(node -e "try{console.log(JSON.parse(require('fs').readFileSync(process.argv[1],'utf8')).pid)}catch{}" "$HARNESS_LOCK" 2>/dev/null)
[ -n "${MYPID:-}" ] && kill "$MYPID" 2>/dev/null
echo "DONE -> $OUT"
