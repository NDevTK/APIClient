/* TWO GATE RUNS, STAGE BY STAGE — AND THE ONE COMPARISON THAT IS ALLOWED TO BE MADE BETWEEN THEM.
 *
 * CLAUDE.md states the rule and nothing performed it: the fixture's answered-statement count "tracks JOBS
 * RUN, not work done", so two runs' counts are comparable only where their job counts agree, and a
 * comparison whose job counts differ is REFUSED rather than reported as a delta. Doing that by hand means
 * opening two snapshots, finding each stage's own log, taking the terminal `@HWORK` line out of each,
 * folding the `@H` tables, and only then deciding whether a difference means anything. It has been done by
 * hand, repeatedly, and §AND-A-MEASUREMENT-CAN-OUTLIVE-ITS-INSTRUMENT is explicit that an instrument whose
 * output anyone quotes is COMMITTED in the same diff as the first quotation of its number. This is that.
 *
 *   node engine/gatecmp.mjs <A>/engine/host/out <B>/engine/host/out
 *
 * EACH STAGE IS READ FROM ITS OWN LOG AND NEVER FROM THE AGGREGATE. A build tees every stage into one
 * stream, so a grep over the aggregate answers about whichever stage happened to emit last — the
 * several-subjects-in-one-directory defect with the subjects interleaved in one file. The per-stage logs are
 * what the build names in its `live at` banners and what survives in the snapshot, so they are the subject
 * here. A directory also carries `run-<stage>.log` as a SYMLINK to `run-<stage>.<pid>.log`; walking both
 * counts every stage twice, so only regular files are read and the stage name comes from the pid-bearing
 * form.
 *
 * ── WHAT IT REFUSES, AND WHY THE REFUSAL IS THE PRODUCT ──────────────────────────────────────────────────
 * The answered-statement count is the one column CLAUDE.md endorses as surviving a repeat, and it survives a
 * repeat AT EQUAL JOBS. Two runs that ran a different number of jobs answered a different amount of the
 * fixture for that reason alone, and a delta between them is a reading of the interleaving. So this prints
 * the two counts with the gate's verdict beside them and REFUSES the delta where the gate is open; a reader
 * who wants the number anyway has it, and what they do not have is a difference presented as a finding.
 *
 * EVERY RECORD IS GATED ON A WORK CURSOR AND NEVER ON THE STAGE'S, WHICH THIS FILE GOT WRONG TWICE AND ITS
 * OWN ARMED RUNS REFUTED BOTH TIMES. `@HWORK` is written immediately before the `@H` table it belongs to, so
 * it pairs WITHIN A LINE with the answered count and is the right gate for that one number. The censuses are
 * sampled on a DIFFERENT cadence, so gating them on `@HWORK` is the cross-sample comparison CLAUDE.md
 * forbids — and it is not a theoretical hazard: measured on the vehicle smoke of two real runs, `@HWORK`
 * reads `workDone 1` on BOTH (the table composed before any flow was ever dispatched) while the terminal
 * `@WFQ` stands at 348 on one and 113 on the other, so a stage-level gate certified a comparison of two runs
 * at two completely different points and reported 80 of 125 keys as differing.
 *
 * THE SECOND WRONG GATE WAS `censusSeq`, WHICH IS AN ORDINAL AND NOT A POSITION. Census pass 5 of one run is
 * not the same point as pass 5 of another — §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED, over a set
 * whose members are sampling instants the scheduler chose. Measured on the same pair: the native smoke's
 * `@COLD` passed a censusSeq gate at 5 on both and reported 50 differing keys, while the `@WFQ` OF THAT SAME
 * PASS refused at workDone 2130 against 2140. So a sequence number equal on both sides is exactly the shape
 * that certifies a comparison of two instants.
 *
 * WHAT POSITIONS A PASS IS THE `@WFQ` OF THAT PASS, AND THE RECORDS SAY SO THEMSELVES. One census pass emits
 * `@WFQ`, `@COLD`, `@HEAP` and `@SWAP` under one `censusSeq`, and `@WFQ` is the one that carries `workDone`,
 * so the whole pass's cursor is derived from it rather than assumed. A record whose `censusSeq` names no
 * `@WFQ` pass, or which carries no `censusSeq` at all, CANNOT be positioned: it is compared and said to rest
 * on nothing, which is the honest state and not a gate.
 *
 * IT CLASSIFIES NO DIFFERING CENSUS ROW, DELIBERATELY. Where a record pair IS at one position, every row is
 * compared and the differing ones are named. Calling one of them "the interleaving" and another "a finding"
 * would need a per-row KIND, and this tree has no declared one: the key names carry a convention
 * (`Lifetime`, `UsLife`) that most rows follow and some do not — `svcMax` is a quantum count derived from
 * microseconds and says so nowhere in its name — so a regex over the keys would be a count of a SPELLING
 * standing in for a fact about the producer. What this reports is the SET, with both values, so a reader can
 * open the accessor. The build's own verdict block names the one mechanism that moves a row on one artifact:
 * the cooperative slice is thread-CPU, so a loaded box costs an opcode sequence more of it and the quantum
 * fires at a different opcode.
 *
 * A STAGE IN ONLY ONE RUN IS NAMED AS SUCH. That is how a truncated run reads correctly: a container restart
 * or a killed wrapper leaves a snapshot whose stage logs simply stop, and the aggregate log's last line is
 * then an ANNOUNCEMENT rather than a result. Those stages are UNSCORED, and a short list that does not say so
 * reads as a clean one.
 *
 * NO EXPECTED TOTAL, NO VERDICT, EXIT 0. Which stages a build runs, and how many statements its fixture
 * asks, both move; and a reading whose exit code refused a build would be refusing it on how a scheduler
 * spent a budget. The one thing that throws is a stream this reader cannot parse.
 *
 * RETIREMENT: this record goes when the build ITSELF emits the pair — a `gates --against <snapshot>` verb
 * whose per-stage verdict line carries the other run's quad beside its own and states the gate — because the
 * comparison is then in the output a reader already has rather than in a command they have to remember.
 */
import fs from "node:fs";
import path from "node:path";
import { probeBest } from "./probe_rows.mjs";

const argv = process.argv.slice(2);
if (argv.length !== 2)
  throw new Error("usage: node engine/gatecmp.mjs <A-out-dir> <B-out-dir>\n" +
    "Each argument is a directory holding a run's per-stage logs — a frozen snapshot's " +
    "engine/host/out. Two aggregate build logs are NOT the subject: a stage's numbers are read from its " +
    "own log, because the aggregate interleaves every stage into one stream.");

/* THE STAGES A DIRECTORY HOLDS, BY NAME, FROM THE PID-BEARING FORM ONLY. */
function stages(dir) {
  const out = new Map();
  if (!fs.existsSync(dir)) throw new Error(`gatecmp: ${dir} does not exist`);
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (!e.isFile()) continue;           /* the un-pidded name is a symlink to one of these */
    const m = /^run-(.+)\.(\d+)\.log$/.exec(e.name);
    if (m) out.set(m[1], path.join(dir, e.name));
  }
  if (!out.size)
    throw new Error(`gatecmp: ${dir} holds no run-<stage>.<pid>.log — either no stage ever started there or ` +
                    `this is not a run's output directory. An empty stage set and a run that produced ` +
                    `nothing are different facts and this reader will not report one as the other.`);
  return out;
}

/* EVERY MARKER'S LAST JSON LINE, PLUS THE WORK CURSOR OF EVERY CENSUS PASS. The key set comes from the
   records themselves rather than from a list here, which is also what makes a key present in one run and
   absent in the other a finding this can report: the producer changed between the two revisions, and a hand
   list would have hidden exactly that. `cursor` is derived from the `@WFQ` stream — the one record of a pass
   that states a work cursor — so every other record of that pass can be positioned without this file
   asserting anything about which markers are emitted together. */
function records(text, where) {
  const byMarker = new Map();
  const cursor = new Map();
  let bad = 0;
  for (const m of text.matchAll(/^(@[A-Z][A-Z0-9_-]*) (\{.*\})$/gm)) {
    let v;
    try { v = JSON.parse(m[2]); } catch { bad++; continue; }   /* a truncated tail is a real state */
    if (!v || typeof v !== "object" || Array.isArray(v)) continue;
    byMarker.set(m[1], v);
    if (m[1] === "@WFQ" && typeof v.censusSeq === "number" && typeof v.workDone === "number")
      cursor.set(v.censusSeq, v.workDone);
  }
  if (bad > 1)
    throw new Error(`gatecmp: ${where} has ${bad} unparseable record line(s) — a killed writer leaves at ` +
                    `most ONE partial line at the tail, so more than that is a stream whose shape this ` +
                    `reader no longer knows and whose numbers it would be attributing to the wrong marker.`);
  return { byMarker, cursor };
}

/* WHERE A RECORD STANDS, IN WORK, OR NOTHING. */
function at(rec, cursor) {
  if (typeof rec.workDone === "number") return rec.workDone;
  if (typeof rec.censusSeq === "number" && cursor.has(rec.censusSeq)) return cursor.get(rec.censusSeq);
  return null;
}

function diffKeys(a, b) {
  const out = [];
  for (const k of [...new Set([...Object.keys(a), ...Object.keys(b)])].sort()) {
    const av = a[k], bv = b[k];
    if (JSON.stringify(av) !== JSON.stringify(bv)) out.push([k, av, bv]);
  }
  return out;
}

const [dirA, dirB] = argv;
const A = stages(dirA), B = stages(dirB);
console.log(`A ${dirA}  (${A.size} stage log(s))`);
console.log(`B ${dirB}  (${B.size} stage log(s))`);

const only = [];
for (const n of A.keys()) if (!B.has(n)) only.push(["A only", n]);
for (const n of B.keys()) if (!A.has(n)) only.push(["B only", n]);
const both = [...A.keys()].filter((n) => B.has(n));

for (const name of both) {
  const ta = fs.readFileSync(A.get(name), "utf8"), tb = fs.readFileSync(B.get(name), "utf8");
  const ra = records(ta, A.get(name)), rb = records(tb, B.get(name));
  const wa = ra.byMarker.get("@HWORK"), wb = rb.byMarker.get("@HWORK");
  const pa = probeBest(ta, A.get(name)), pb = probeBest(tb, B.get(name));
  const ansA = [...pa.best.values()].filter(Boolean).length, ansB = [...pb.best.values()].filter(Boolean).length;

  console.log(`\n── ${name}`);
  if (!wa || !wb) {
    console.log(`   NO WORK LINE on ${!wa ? "A" : ""}${!wa && !wb ? " and " : ""}${!wb ? "B" : ""} — a stage ` +
                `that printed no @HWORK makes no claim about how much engine work it did, so nothing below ` +
                `is comparable and the answered counts are reported bare: ` +
                `A ${ansA}/${pa.best.size} over ${pa.tables} table(s), B ${ansB}/${pb.best.size} over ` +
                `${pb.tables}.`);
    continue;
  }
  const workSame = JSON.stringify(wa) === JSON.stringify(wb);
  const jobsSame = wa._jobsRun === wb._jobsRun;
  const quad = (w) => `work ${w.workDone} / ${w._switches} switches / ${w._flows} flows / ${w._jobsRun} jobs`;
  console.log(`   A  ${quad(wa)}   answered ${ansA}/${pa.best.size} over ${pa.tables} table(s)`);
  console.log(`   B  ${quad(wb)}   answered ${ansB}/${pb.best.size} over ${pb.tables} table(s)`);

  /* THE ANSWERED COUNT, GATED ON THE LINE IT PAIRS WITH. */
  if (!jobsSame)
    console.log(`   answered: REFUSED — the job counts differ (${wa._jobsRun} vs ${wb._jobsRun}). The ` +
                `answered count tracks jobs run rather than work done, so ${ansA} against ${ansB} is a ` +
                `reading of the interleaving and this tool will not present it as a difference between the ` +
                `two revisions. What IS comparable across a stage like this is the IDENTITY of whatever ` +
                `assertion ended it.`);
  else
    console.log(`   answered: ${ansA === ansB ? `IDENTICAL at ${ansA}/${pa.best.size}`
                                              : `MOVED ${ansA} -> ${ansB} AT EQUAL JOBS — a finding`}` +
                ` (${wa._jobsRun} jobs on both` +
                (workSame ? ", and the whole work line is identical — stronger than the gate requires)"
                          : ", though the rest of the work line is not: " +
                            diffKeys(wa, wb).map(([k, x, y]) => `${k} ${x}->${y}`).join(", ") + ")"));

  /* AND EVERY CENSUS ON THE WORK CURSOR OF ITS OWN PASS. */
  const markers = [...new Set([...ra.byMarker.keys(), ...rb.byMarker.keys()])].sort();
  const unpositioned = [];
  for (const mk of markers) {
    if (mk === "@HWORK") continue;
    const a = ra.byMarker.get(mk), b = rb.byMarker.get(mk);
    if (!a || !b) { console.log(`   ${mk}: present on ${a ? "A" : "B"} only — UNSCORED`); continue; }
    const pa2 = at(a, ra.cursor), pb2 = at(b, rb.cursor);
    const n = `${Object.keys(a).length} vs ${Object.keys(b).length} key(s)`;
    if (pa2 !== null && pb2 !== null && pa2 !== pb2) {
      console.log(`   ${mk}: REFUSED — these records stand at workDone ${pa2} and ${pb2}, so they are two ` +
                  `different points of two runs and no row delta between them is about this revision. ${n}.`);
      continue;
    }
    const where = pa2 !== null && pb2 !== null ? `at workDone ${pa2}` : "UNPOSITIONED";
    if (where === "UNPOSITIONED") unpositioned.push(mk);
    const d = diffKeys(a, b);
    if (!d.length) { console.log(`   ${mk}: ${Object.keys(a).length} key(s) ${where}, BYTE-IDENTICAL`); continue; }
    console.log(`   ${mk}: ${n} ${where}, ${d.length} differ (UNCLASSIFIED — see this file's banner on why)`);
    for (const [k, x, y] of d) console.log(`       ${k}: ${JSON.stringify(x)} -> ${JSON.stringify(y)}`);
  }
  if (unpositioned.length)
    console.log(`   UNPOSITIONED above (${unpositioned.join(" ")}): no work cursor reaches these — they carry ` +
                `no workDone and no censusSeq that a @WFQ pass of this run names — so their pairing rests on ` +
                `nothing and a differing row there may be two instants rather than two revisions.`);

  /* THE STATE THAT MADE THE STAGE GATE WRONG, NAMED WHERE IT SHOWS. A probe table composed at a work cursor
     far below where the censuses reached is the table-before-anything-happened state the build names in
     prose; it is read here off the two numbers rather than inferred from a sample count. */
  const far = [...ra.cursor.values(), ...rb.cursor.values()];
  const reached = far.length ? Math.max(...far) : null;
  if (reached !== null && reached > 4 * Math.max(wa.workDone, wb.workDone, 1))
    console.log(`   NOTE: the @H table was composed at workDone ${wa.workDone}/${wb.workDone} while a census ` +
                `in the same run reached ${reached} — so the answered count above is a reading of the run's ` +
                `OPENING and the censuses are a reading of much later, which is why they are gated apart.`);
}

if (only.length) {
  console.log(`\n── STAGES IN ONE RUN ONLY — UNSCORED, and the reason this list is printed even when it is ` +
              `short: a run cut by a container restart or a killed wrapper leaves exactly this shape, and a ` +
              `comparison over the stages that DID pair reads as complete unless the missing ones are named.`);
  for (const [side, n] of only) console.log(`   ${side}: ${n}`);
}
console.log(`\n${both.length} stage(s) paired, ${only.length} unpaired. This is a reading and not a verdict.`);
