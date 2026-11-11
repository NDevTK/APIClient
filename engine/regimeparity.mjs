/* The assert-regime comparison: one fixture document run on the dev and the release build of one program at
 * one revision. Engine work is a function of the flow's path, so the two regimes retire the same work and pick
 * the same members in the same order; a difference is a computation only the dev build makes (an assert's
 * condition, a `#if APICLIENT_DEV` region) that was counted as work, spent an allowance, or left a memo a
 * priced path then read. Compared, when both runs completed: each statement's value in the final @H table and
 * the verdict; every @H table paired by the `@HEVENTS` work total it was composed at; the result document's
 * `_workRetired`; and of its frontier census the pick digest, the steps, the work the steps and the instance
 * retired, and every thread-work row that is not thread CPU. A dev run that aborted at an assert has no
 * schedule to compare, so its pair is not asked and names the abort. One process cannot hold both regimes,
 * which is why this compares two binaries' output and is not a row of the fixture's own table.
 *   node engine/regimeparity.mjs --dev <bin> --release <bin> --out <dir> [--cpu <s>] [--jobs <n>] <doc>...
 * runs each document on both binaries under the CPU budget and prints one comparison per document; `min` names
 * test_forced.c's minimal document (`--min`), any other name a `--doc <name>`. */

import { spawn } from "node:child_process";
import { mkdirSync, readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { vehicleReading } from "./vehicle.mjs";

/* The fixture arguments that run document `doc`. */
export const regimeDocArgs = (doc) => doc === "min" ? ["--min"] : ["--doc", doc];

/* The census rows that are work or its input, read off the census itself so a kind added to
   core/timing/thread_work.h is compared without an entry here. */
const CENSUS_NAMED = ["pickDigest", "steps", "stepWork", "serviceWork"];
const censusRows = (c) => [...CENSUS_NAMED, ...Object.keys(c).filter((k) => /^tw[A-Z]/.test(k) && !k.endsWith("Us"))];

/* One run's reading: engine/vehicle.mjs's, with the result document's own work total beside it. */
export function regimeReading(out, signal, where) {
  const r = vehicleReading(out, signal, where);
  let retired = null;
  for (const line of out.split("\n")) {
    if (!line.startsWith("@RESULT ")) continue;
    retired = JSON.parse(line.slice(8))._workRetired;
    if (!Number.isInteger(retired))
      throw new Error(`${where}: the result document carries no integer \`_workRetired\` — solver/result.c ` +
                      `composes it into every result, so the composer and this reader disagree`);
  }
  return Object.assign(r, { retired });
}

/* The comparison of one document's two readings: `{ defect, answered, compared, uncompared, differing }`.
   `defect` is null or the first disagreement; `differing` lists every work row that differs, as
   `row=dev|release`; `answered` says whether two completed schedules were compared at all. */
export function compareRegimes(doc, dev, rel) {
  const compared = [], uncompared = [], differing = [];
  let defect = null, answered = false;
  const fail = (s) => { if (defect === null) defect = `${doc}: ${s}`; };

  if (dev.crash !== null && rel.crash !== null) {
    /* An always-fatal CHECK is in both regimes; a dev-only abort beside it is a different site. */
    answered = true;
    if (dev.crash !== rel.crash) fail(`the crashes differ: dev ${dev.crash}; release ${rel.crash}`);
    else compared.push(`the crash ${dev.crash}`);
    return { defect, answered, compared, uncompared, differing };
  }
  if (dev.crash !== null || rel.crash !== null) {
    const [who, at] = dev.crash !== null ? ["dev", dev.crash] : ["release", rel.crash];
    if (who === "release") { answered = true; fail(`release crashed at ${at} and dev did not`); }
    else uncompared.push(`the schedules (dev aborted at ${at}, which a release build runs past)`);
    return { defect, answered, compared, uncompared, differing };
  }
  if (dev.final === null || rel.final === null || dev.cold === null || rel.cold === null ||
      !dev.cold.terminal || !rel.cold.terminal) {
    uncompared.push(`the schedules (completed: dev ${dev.final !== null}, release ${rel.final !== null})`);
    return { defect, answered, compared, uncompared, differing };
  }
  answered = true;

  const asked = Object.keys(dev.final).find((n) => !(n in rel.final)) ??
                Object.keys(rel.final).find((n) => !(n in dev.final));
  if (asked !== undefined) fail(`the two regimes ask different statements; first: ${asked}`);
  const stmt = Object.keys(dev.final).find((k) => dev.final[k] !== rel.final[k]);
  if (stmt !== undefined)
    fail(`the final tables differ; first statement: ${stmt} (dev ${+dev.final[stmt]}, release ${+rel.final[stmt]})`);
  else if (dev.verdict !== rel.verdict) fail(`the verdicts differ: dev ${dev.verdict}; release ${rel.verdict}`);
  else compared.push(`every final statement's value and the verdict ${dev.verdict}`);

  const dc = dev.cold.census, rc = rel.cold.census, rows = censusRows(dc);
  for (const k of censusRows(rc)) if (!rows.includes(k)) rows.push(k);
  for (const k of rows) {
    if (!Number.isInteger(dc[k]) || !Number.isInteger(rc[k]))
      throw new Error(`${doc}: the frontier census row \`${k}\` is ${JSON.stringify(dc[k])} in the dev run and ` +
                      `${JSON.stringify(rc[k])} in the release run — solver/metrics/cold.def declares one ` +
                      `integer row for both regimes, so a composer and this reader disagree`);
    if (dc[k] !== rc[k]) differing.push(`${k}=${dc[k]}|${rc[k]}`);
  }
  if (dev.retired !== rel.retired) differing.push(`_workRetired=${dev.retired}|${rel.retired}`);
  if (differing.length)
    fail(`a dev build and a release build of one revision ran different schedules or retired different work ` +
         `(dev|release): ${differing.join(" ")}`);
  else compared.push(`${rows.length + 1} work row(s), the pick digest ${dc.pickDigest} among them`);

  const at = new Map(rel.tables.filter((t) => t.work !== null).map((t) => [t.work, t.row]));
  let paired = 0;
  for (const t of dev.tables) {
    if (t.work === null || !at.has(t.work)) continue;
    paired++;
    const o = at.get(t.work), k = Object.keys(t.row).find((n) => t.row[n] !== o[n]);
    if (k !== undefined) { fail(`the tables at work ${t.work} differ; first statement: ${k}`); break; }
  }
  compared.push(`${paired} table(s) paired by work total`);
  return { defect, answered, compared, uncompared, differing };
}

/* The comparison of one document run with its forced pressure act and as its no-pressure twin
   (`--pressure-act none`) on one binary: `{ defect, answered, compared, differing }`. Memory decides where a flow
   lives, never whether or when it runs, so the two pick the same members in the same order and retire the same
   work; the final tables are not compared, since the act's own rows read the act. */
export function comparePressure(doc, act, none) {
  const compared = [], differing = [];
  if (act.crash !== null || none.crash !== null || act.cold === null || none.cold === null ||
      !act.cold.terminal || !none.cold.terminal)
    return { defect: null, answered: false, differing,
             compared: [`nothing (act: ${act.crash ?? (act.cold?.terminal ? "completed" : "no terminal census")}; ` +
                        `none: ${none.crash ?? (none.cold?.terminal ? "completed" : "no terminal census")})`] };
  const ac = act.cold.census, nc = none.cold.census, rows = censusRows(ac);
  for (const k of censusRows(nc)) if (!rows.includes(k)) rows.push(k);
  for (const k of rows) {
    if (!Number.isInteger(ac[k]) || !Number.isInteger(nc[k]))
      throw new Error(`${doc}: the frontier census row \`${k}\` is ${JSON.stringify(ac[k])} with the act and ` +
                      `${JSON.stringify(nc[k])} without it — solver/metrics/cold.def declares one integer row`);
    if (ac[k] !== nc[k]) differing.push(`${k}=${ac[k]}|${nc[k]}`);
  }
  if (act.retired !== none.retired) differing.push(`_workRetired=${act.retired}|${none.retired}`);
  compared.push(`${rows.length + 1} work row(s), the pick digest ${ac.pickDigest} among them`);
  return { defect: differing.length ? `${doc}: the run with its pressure act and its no-pressure twin ran different ` +
                                      `schedules or retired different work (act|none): ${differing.join(" ")}` : null,
           answered: true, compared, differing };
}

/* ── The lane-facing driver ─────────────────────────────────────────────────────────────────────────────── */

function args(argv) {
  const o = { docs: [], cpu: 900, jobs: 2 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--dev") o.dev = argv[++i];
    else if (a === "--release") o.release = argv[++i];
    else if (a === "--cpu") o.cpu = Number(argv[++i]);
    else if (a === "--jobs") o.jobs = Number(argv[++i]);
    else if (a === "--out") o.out = argv[++i];
    else if (a.startsWith("--")) throw new Error(`regimeparity.mjs: unknown flag ${a}`);
    else o.docs.push(a);
  }
  if (!o.dev || !o.release || !o.out || !o.docs.length || !(o.cpu > 0) || !(o.jobs > 0))
    throw new Error("usage: node engine/regimeparity.mjs --dev <bin> --release <bin> --out <dir> [--cpu <s>] " +
                    "[--jobs <n>] <doc>...");
  return o;
}

/* One budgeted run through engine/child_run.mjs, as its own process so its CPU is its own reaper's. */
function budgeted(spec) {
  return new Promise((done, fail) => {
    let text = "";
    const c = spawn(process.execPath, [join(dirname(fileURLToPath(import.meta.url)), "child_run.mjs"),
                                       JSON.stringify(spec)], { stdio: ["ignore", "pipe", "inherit"] });
    c.stdout.on("data", (d) => { text += d; });
    c.on("error", fail);
    c.on("close", (code) => code === 0 ? done(JSON.parse(text))
                                       : fail(new Error(`child_run.mjs exited ${code} for ${spec.slug}`)));
  });
}

async function main() {
  const o = args(process.argv.slice(2));
  mkdirSync(o.out, { recursive: true });
  const jobs = [];
  for (const d of o.docs)
    for (const [regime, prog] of [["dev", o.dev], ["release", o.release]])
      jobs.push({ doc: d, regime, prog, args: regimeDocArgs(d) });
  const runs = new Map();
  let next = 0;
  const worker = async () => {
    for (let i = next++; i < jobs.length; i = next++) {
      const j = jobs[i], log = join(o.out, `${j.doc}.${j.regime}.log`);
      const t = await budgeted({ slug: `regime-${j.doc}-${j.regime}`, prog: j.prog, args: j.args, log,
                                 hardS: o.cpu + 60, softS: o.cpu, deadlockMs: 4 * o.cpu * 1000 });
      runs.set(`${j.doc}.${j.regime}`, { t, r: regimeReading(readFileSync(log, "utf8"), t.signal, log) });
    }
  };
  await Promise.all(Array.from({ length: Math.min(o.jobs, jobs.length) }, worker));
  let defects = 0, unasked = 0;
  for (const d of o.docs) {
    const dv = runs.get(`${d}.dev`), rl = runs.get(`${d}.release`);
    const c = compareRegimes(d, dv.r, rl.r);
    if (c.defect !== null) defects++;
    else if (!c.answered) unasked++;
    console.log(`[regime] ${d}: ${c.defect !== null ? "DEFECT — " + c.defect
                                 : c.answered ? "AGREE" : "NOT ASKED — " + c.uncompared.join("; ")}`);
    console.log(`[regime]   compared: ${c.compared.join("; ") || "nothing"}`);
    console.log(`[regime]   cpu: dev ${dv.t.cpuSeconds.toFixed(2)} s (${dv.t.signal ?? "exit " + dv.t.status}); ` +
                `release ${rl.t.cpuSeconds.toFixed(2)} s (${rl.t.signal ?? "exit " + rl.t.status})`);
  }
  console.log(`[regime] ${defects} DEFECT(S) and ${unasked} NOT ASKED over ${o.docs.length} document(s)`);
  process.exit(defects || unasked ? 1 : 0);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) await main();
