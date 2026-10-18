/* The vehicle comparison: one fixture document run on the native development host and on the shipped WASM
 * build through one entry (test_forced.c's `--doc <name>`), and the two runs' answers and work compared:
 *   - the statements asked (the row names of the @H table): a property of the program, so always;
 *   - the crash identity (engine/abort_record.mjs): when both runs crashed, and when one crashed while the
 *     other reached its final table, since a completed run executed every flow the crashing one could have;
 *   - each statement's value in the final @H table (the one printed after `@RESULT`) and the fixture's own
 *     verdict line: when both runs completed;
 *   - every @H table paired by the `@HWORK` work total it was composed at, and the work retired by a completed
 *     run: only when both hosts announce the same `@QUANTUM` slice, because a slice in different currencies
 *     interleaves flows differently and the two runs then take different paths through one document.
 *   node engine/vehicle.mjs --native <bin> --wasm <qjs.js> [--wasm-node <node>] [--cpu <s>] [--jobs <n>]
 *                           --out <dir> <doc>...
 * runs each named document on both hosts under the CPU budget and prints one comparison per document. */

import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { probeTables } from "./probe_rows.mjs";
import { crashIdentity } from "./abort_record.mjs";

/* The fixture's verdict sentences, in test_forced.c's main() after the final table. */
const VERDICTS = [["PASS:", "PASS"], ["FAIL:", "FAIL"], ["[test-forced] NOT ASKED", "NOT ASKED"],
                  ["LIFECYCLE:", "LIFECYCLE"]];

/* One run's reading. `out` is the run's whole captured output, `signal` its terminating signal or null, and
   `where` names the artifact for the one throw probeTables can make. A final line with no newline is a write
   the run was killed in the middle of, and is not read. */
export function vehicleReading(out, signal, where) {
  const lines = out.split("\n");
  if (!out.endsWith("\n")) lines.pop();
  const tables = [];
  let work = null, completed = false, verdict = null, quantum = null, cold = null;
  for (const line of lines) {
    if (line.startsWith("@HWORK ")) { work = JSON.parse(line.slice(7)).workDone; continue; }
    if (line.startsWith("@RESULT ")) {
      completed = true;
      const r = JSON.parse(line.slice(8));
      if (!r._cold || typeof r._cold !== "object")
        throw new Error(`${where}: the result document carries no \`_cold\` census — solver/result.c composes ` +
                        `it into every result, so the composer and this reader disagree`);
      cold = { census: r._cold, terminal: true };
      continue;
    }
    if (line.startsWith("@QUANTUM ")) { if (quantum === null) quantum = line.slice(9); continue; }
    if (line.startsWith("@COLD {")) {
      if (!completed) cold = { census: JSON.parse(line.slice(6)), terminal: false };
      continue;
    }
    if (line.startsWith("@H ")) {
      const t = probeTables(line, where);
      if (t.length) tables.push({ row: t[0], work, final: completed });
      work = null;
      continue;
    }
    if (completed && verdict === null)
      for (const [head, word] of VERDICTS) if (line.startsWith(head)) verdict = word;
  }
  const finals = tables.filter((t) => t.final);
  if (completed && verdict === null)
    throw new Error(`${where}: the run printed @RESULT and none of test_forced.c's verdict sentences after ` +
                    `it — main() prints one on every completion, so the emitter and this reader disagree`);
  return { tables, final: finals.length ? finals[finals.length - 1].row : null, verdict,
           crash: crashIdentity(out, signal), quantum, work: coldWork(cold, where) };
}

/* The work the run retired: every unit the event loop's clock banked under a flow, the units retired with no
   flow running (discarded), and the part of both that is host work charged in the opcode unit
   (core/timing/thread_work.h). A completed run's is the result document's own census, taken at completion;
   an incomplete run's is its last @COLD census, a floor (`terminal` false). Null where there is no census. */
function coldWork(cold, where) {
  if (cold === null) return null;
  const c = cold.census;
  for (const k of ["twBankedUnits", "twChargedUnits", "twDiscardedUnits"])
    if (!Number.isInteger(c[k]))
      throw new Error(`${where}: the frontier census carries no integer \`${k}\` — solver/metrics/cold.def ` +
                      `declares it, so the composer and this reader disagree`);
  const retired = c.twBankedUnits + c.twDiscardedUnits;
  return { retired, charged: c.twChargedUnits, opcodes: retired - c.twChargedUnits, terminal: cold.terminal };
}

/* The first statement, in `a`'s order, whose value differs between two tables of one program. */
function firstDiffering(a, b) {
  for (const k of Object.keys(a)) if (a[k] !== b[k]) return k;
  for (const k of Object.keys(b)) if (!(k in a)) return k;
  return null;
}

const workText = (r) => r.work === null ? "no frontier census"
  : `${r.work.retired} units retired${r.work.terminal ? " at completion" : " by the last census (a floor)"} ` +
    `(${r.work.opcodes} opcodes, ${r.work.charged} charged host work)`;

/* The comparison of one document's two readings: `{ defect, answered, compared, uncompared, work }`, where
   `defect` is null or the first disagreement, naming the document and the statement, and `answered` says
   whether the crashes or the final answers were compared at all. */
export function compareVehicle(doc, nat, veh) {
  const compared = [], uncompared = [];
  let defect = null, answered = false;
  const fail = (s) => { if (defect === null) defect = `${doc}: ${s}`; };
  const askedOf = (r) => r.tables.length ? r.tables[r.tables.length - 1].row : null;
  const na = askedOf(nat), va = askedOf(veh);
  if (na !== null && va !== null) {
    const k = Object.keys(na).find((n) => !(n in va)) ?? Object.keys(va).find((n) => !(n in na));
    if (k !== undefined) fail(`the two programs ask different statements; first: ${k}`);
    else compared.push(`the ${Object.keys(na).length} statement(s) asked`);
  } else uncompared.push(`the statements asked (${na === null ? "native" : "wasm"} printed no @H table)`);

  if (nat.crash !== null && veh.crash !== null) {
    answered = true;
    if (nat.crash !== veh.crash) fail(`the crashes differ: native ${nat.crash}; wasm ${veh.crash}`);
    else compared.push(`the crash ${nat.crash}`);
  } else if (nat.crash !== null && veh.final !== null) {
    answered = true;
    fail(`native crashed at ${nat.crash} and wasm completed with ${veh.verdict}`);
  } else if (veh.crash !== null && nat.final !== null) {
    answered = true;
    fail(`wasm crashed at ${veh.crash} and native completed with ${nat.verdict}`);
  } else if (nat.crash !== null || veh.crash !== null) {
    const [who, at] = nat.crash !== null ? ["native", nat.crash] : ["wasm", veh.crash];
    uncompared.push(`the crash (${who} crashed at ${at}; the other neither crashed nor completed)`);
  }

  if (nat.final !== null && veh.final !== null) {
    answered = true;
    const k = firstDiffering(nat.final, veh.final);
    if (k !== null) fail(`the final tables differ; first statement: ${k} (native ${+nat.final[k]}, wasm ` +
                         `${+veh.final[k]})`);
    else if (nat.verdict !== veh.verdict) fail(`the verdicts differ: native ${nat.verdict}; wasm ${veh.verdict}`);
    else compared.push(`every final statement's value and the verdict ${nat.verdict}`);
  } else uncompared.push(`the final answers (completed: native ${nat.final !== null}, wasm ${veh.final !== null})`);

  const exact = nat.quantum !== null && nat.quantum === veh.quantum;
  let work;
  if (exact) {
    const at = new Map(veh.tables.filter((t) => t.work !== null).map((t) => [t.work, t.row]));
    let paired = 0;
    for (const t of nat.tables) {
      if (t.work === null || !at.has(t.work)) continue;
      paired++;
      const k = firstDiffering(t.row, at.get(t.work));
      if (k !== null) { fail(`the tables at work ${t.work} differ; first statement: ${k}`); break; }
    }
    compared.push(`${paired} table(s) paired by work total`);
    if (nat.final !== null && veh.final !== null && nat.work !== null && veh.work !== null &&
        nat.work.retired !== veh.work.retired)
      fail(`the completed runs retired different work: native ${nat.work.retired}, wasm ${veh.work.retired}`);
    work = `exact (one slice on both hosts: ${nat.quantum}); native ${workText(nat)}; wasm ${workText(veh)}`;
  } else {
    work = `not yet exact: the hosts slice in different currencies (native ${nat.quantum ?? "no @QUANTUM"}; ` +
           `wasm ${veh.quantum ?? "no @QUANTUM"}), so their paths differ and their work is reported, not ` +
           `compared; native ${workText(nat)}; wasm ${workText(veh)}` +
           (nat.work !== null && veh.work !== null && nat.work.terminal && veh.work.terminal
              ? `; at completion they ${nat.work.retired === veh.work.retired ? "are equal"
                                       : `differ by ${veh.work.retired - nat.work.retired} units (wasm - native)`}`
              : ``);
  }
  return { defect, answered, compared, uncompared, work };
}

/* The node that runs the wasm64 artifact: emsdk's own (its `.emscripten` config's NODE_JS), because the final
   Memory64 encoding needs node 24 (emscripten's tools/feature_matrix.py, MEMORY64 'node': 240000) and the node
   on the path may be older. */
export function emsdkNode(emsdk) {
  const cfg = readFileSync(join(emsdk, ".emscripten"), "utf8").match(/^NODE_JS = '([^']+)'$/m);
  if (!cfg) throw new Error(`${join(emsdk, ".emscripten")} states no NODE_JS, so no node is named that can run ` +
                            `the wasm64 artifact`);
  const node = cfg[1].replace("$CFGDIR", emsdk);
  if (!existsSync(node)) throw new Error(`emsdk's NODE_JS ${node} does not exist`);
  return node;
}

/* ── The lane-facing driver ─────────────────────────────────────────────────────────────────────────── */

function args(argv) {
  const o = { docs: [], cpu: 900, jobs: 2, wasmNode: null };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--native") o.native = argv[++i];
    else if (a === "--wasm") o.wasm = argv[++i];
    else if (a === "--wasm-node") o.wasmNode = argv[++i];
    else if (a === "--cpu") o.cpu = Number(argv[++i]);
    else if (a === "--jobs") o.jobs = Number(argv[++i]);
    else if (a === "--out") o.out = argv[++i];
    else if (a.startsWith("--")) throw new Error(`vehicle.mjs: unknown flag ${a}`);
    else o.docs.push(a);
  }
  if (!o.native || !o.wasm || !o.out || !o.docs.length || !(o.cpu > 0) || !(o.jobs > 0))
    throw new Error("usage: node engine/vehicle.mjs --native <bin> --wasm <qjs.js> [--wasm-node <node>] " +
                    "[--cpu <s>] [--jobs <n>] --out <dir> <doc>...");
  if (o.wasmNode === null) o.wasmNode = emsdkNode(join(dirname(fileURLToPath(import.meta.url)), ".work", "emsdk"));
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
  for (const d of o.docs) {
    jobs.push({ doc: d, host: "native", prog: o.native, args: ["--doc", d] });
    jobs.push({ doc: d, host: "wasm", prog: o.wasmNode, args: [o.wasm, "--doc", d] });
  }
  const runs = new Map();
  let next = 0;
  const worker = async () => {
    for (let i = next++; i < jobs.length; i = next++) {
      const j = jobs[i], log = join(o.out, `${j.doc}.${j.host}.log`);
      const t = await budgeted({ slug: `vehicle-${j.doc}-${j.host}`, prog: j.prog, args: j.args, log,
                                 hardS: o.cpu + 60, softS: o.cpu, deadlockMs: 4 * o.cpu * 1000 });
      runs.set(`${j.doc}.${j.host}`, { t, r: vehicleReading(readFileSync(log, "utf8"), t.signal, log) });
    }
  };
  await Promise.all(Array.from({ length: Math.min(o.jobs, jobs.length) }, worker));
  let defects = 0;
  for (const d of o.docs) {
    const n = runs.get(`${d}.native`), w = runs.get(`${d}.wasm`);
    const c = compareVehicle(d, n.r, w.r);
    if (c.defect !== null) defects++;
    console.log(`[vehicle] ${d}: ${c.defect === null ? "AGREE" : "DEFECT — " + c.defect}`);
    console.log(`[vehicle]   compared: ${c.compared.join("; ") || "nothing"}`);
    if (c.uncompared.length) console.log(`[vehicle]   not compared: ${c.uncompared.join("; ")}`);
    console.log(`[vehicle]   work: ${c.work}`);
    console.log(`[vehicle]   cpu: native ${n.t.cpuSeconds.toFixed(2)} s (${n.t.signal ?? "exit " + n.t.status}); ` +
                `wasm ${w.t.cpuSeconds.toFixed(2)} s (${w.t.signal ?? "exit " + w.t.status})`);
  }
  console.log(`[vehicle] ${defects} DEFECT(S) over ${o.docs.length} document(s)`);
  process.exit(defects ? 1 : 0);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) await main();
