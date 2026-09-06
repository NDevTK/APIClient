/* WHICH FIXTURE STATEMENTS A SMOKE RUN NEVER ANSWERED, AND WHAT THAT DOES NOT ESTABLISH.
 *
 * `node engine/build.mjs cow`'s smoke stage prints one number — "N/M of the fixture's statements answered" —
 * and that number is the only thing anyone quotes. M is a population the fixture itself asks and N is how much
 * of it a run reached, so the interesting object is neither: it is WHICH rows are still 0, and in what SHAPE.
 * This reads a smoke log and says so.
 *
 * IT EXISTS BECAUSE THE NUMBERS WERE QUOTED FIRST. CLAUDE.md requires that an instrument whose output anyone
 * quotes be COMMITTED in the same diff as the first quotation of its number, and it records an incident where
 * a measurement outlived its tool: a count of absent platform globals, six runs, zero spread, dispatched into
 * briefs as ranked work, produced by a script that existed in no revision. A reader who re-derives such a
 * figure finds nothing and cannot tell a measurement that was never made from one whose tool was never kept.
 *
 * THE ROW NAMES ARE DERIVED FROM THE LOG, NEVER LISTED HERE. A hand-kept list is the second copy of a fact the
 * fixture owns, and this project has been bitten by that shape repeatedly — an auditor's table drifting from
 * the artifact it audits. The fixture gains and loses rows as lanes add probes; this tool learns them from the
 * `@H` lines it is reading and reports what it found.
 *
 * THERE IS NO EXPECTED TOTAL AND NO EXPECTED COUNT, DELIBERATELY. Both rot, and CLAUDE.md rates a count of what
 * is MISSING as the worst status of all: it shrinks as people do the work, and its only reader is the person
 * about to invalidate it. What is printed is today's partition of today's rows, beside the log it came from.
 *
 * ── WHAT A ZERO HERE DOES NOT MEAN ───────────────────────────────────────────────────────────────────────────
 * A row at 0 in every table OF ONE RUN is consistent with THREE states and one log cannot separate them:
 *   (a) the mechanism is missing — the statement cannot be answered by this engine at all;
 *   (b) the mechanism exists and no flow reached it before the stage's CPU budget was spent;
 *   (c) the mechanism exists and this run's interleaving did not schedule it, where another run would.
 * The smoke stage is budget-killed rather than run to completion, and this host has no CPU clock, so (b) and
 * (c) are live for every row. Quoting a row as an absent capability requires evidence this tool does not have.
 * What it IS good for is SHAPE: a family whose early rows answer and whose later rows are uniformly 0 says
 * something a single number cannot, because the split is inside one family that one run exercised.
 *
 * AND SEVERAL LOGS SEPARATE (a) FROM (b) AND (c), WHICH IS WHY THIS TAKES MORE THAN ONE. Interleaving and
 * budget vary per run; a MISSING MECHANISM does not. So a row 0 in one log is weak and a row 0 across many
 * logs at many revisions is strong, and the per-run bitmap below is the output rather than a total, because
 * a reader has to see WHICH runs answered a row to judge it. This tool was landed reading one log, and its
 * author immediately quoted that log's zeros as a shape — the exact reading the paragraph above forbids. The
 * multi-log form exists because obeying a caveat is not the same as writing one.
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────── */

import { readFileSync } from "node:fs";

const paths = process.argv.slice(2);
if (paths.length === 0) {
    console.error("usage: node engine/smokerows.mjs <smoke log> [<smoke log> ...]");
    console.error("  several logs are better than one: interleaving and budget vary per run and a missing");
    console.error("  mechanism does not, so a row 0 across many runs is evidence where a row 0 in one is not.");
    console.error("  the smoke stage names its live log in the build output; a finished build's copy is kept");
    console.error("  beside the build log by whatever froze the snapshot.");
    process.exit(2);
}

/* Each log is read independently; a row's evidence is the VECTOR of per-run answers, never a sum. Summing
   would re-create the single number this tool exists to unpack. */
const runs = [];
for (const path of paths) {
    const text = readFileSync(path, "utf8");
    /* A row is `name=<digits>` on an `@H` line. Anchored at line start so an `@H` quoted inside a message is
       not read as a table. */
    const tables = text.split("\n").filter((l) => l.startsWith("@H "));
    if (tables.length === 0) {
        console.log(`SKIPPED ${path}: no @H table lines — a build that died before the smoke stage writes one`);
        console.log("  (this is REPORTED rather than thrown, because a run that produced no table is a fact");
        console.log("   about that run; a single log with no tables still throws below, since asking one");
        console.log("   question of one empty log is a mis-addressed question rather than a datum.)");
        runs.push({ path, best: new Map(), lines: 0, empty: true });
        continue;
    }
    const best = new Map();
    for (const line of tables)
        for (const m of line.matchAll(/([a-z][a-z0-9-]*)=(\d+)/g)) {
            const k = m[1], v = Number(m[2]);
            /* EVERY ROW IS A STATEMENT AND A STATEMENT IS ANSWERED OR IT IS NOT, so a value outside {0,1} is
               a COUNTER that has leaked onto an @H line — and this tool would silently read any non-zero as
               "answered", turning a magnitude into a boolean and reporting a row as reached because a
               counter happened to be positive. The assumption was implicit until it was checked, and it
               checked out (249 row names over 70 logs, no value but 0 or 1), which is exactly when to make
               it explicit: an invariant confirmed once and left unasserted is one the next emitter breaks.
               It throws rather than clamping, because the honest answer to a row this tool cannot classify
               is not a smaller number. */
            if (v !== 0 && v !== 1)
                throw new Error(`${path}: row \`${k}\` has value ${v} — @H rows are statements answered or ` +
                                `not, so a value outside {0,1} means a counter now shares the table and this ` +
                                `tool would read any non-zero as answered. Separate them at the emitter.`);
            best.set(k, Math.max(best.get(k) ?? 0, v));
        }
    if (best.size === 0)
        throw new Error(`${path}: @H lines carry no name=value pairs — the row spelling changed and this tool ` +
                        `would otherwise measure a subset without saying so, which is the failure it exists to avoid`);
    runs.push({ path, best, lines: tables.length, empty: false });
}

const live = runs.filter((r) => !r.empty);
if (live.length === 0)
    throw new Error(`no log carried an @H table — either these are not smoke logs, or the fixture's report ` +
                    `shape changed. Read the fixture's own emitter before editing this pattern.`);

/* A name present in ANY run is a row; a run that never mentions it is evidence about that run, not an absence
   of the row. */
const names = new Set();
for (const r of live) for (const k of r.best.keys()) names.add(k);

const vector = (k) => live.map((r) => ((r.best.get(k) ?? 0) > 0 ? "1" : "."));
const everAnswered = (k) => vector(k).includes("1");

const answered = [...names].filter(everAnswered).sort();
const never = [...names].filter((k) => !everAnswered(k)).sort();

const fam = (k) => (k.includes("-") ? k.slice(0, k.indexOf("-")) : k);
const families = new Map();
for (const k of names) {
    const f = fam(k);
    if (!families.has(f)) families.set(f, []);
    families.get(f).push(k);
}

console.log(`smoke rows across ${live.length} log(s) with tables` +
            (runs.length > live.length ? `, ${runs.length - live.length} skipped as tableless` : ""));
for (const r of live) console.log(`  ${r.lines.toString().padStart(5)} @H line(s)  ${r.path}`);
console.log("");
console.log(`  ${names.size} distinct row name(s), derived from the logs and not listed in this file`);
console.log(`  ${answered.length} answered in at least one run, ${never.length} never answered in ANY run`);
console.log("");
console.log("The column order below is the log order above. A row answered in some runs and not others is");
console.log("evidence about SCHEDULING; a row answered in none, across runs at different revisions with");
console.log("different budgets, is evidence about a MECHANISM — which is the distinction one log cannot make.");
console.log("");

for (const [f, ks] of [...families].sort()) {
    const sorted = ks.sort();
    const anyYes = sorted.some(everAnswered), anyNo = sorted.some((k) => !everAnswered(k));
    const kind = !anyYes ? "NEVER ANSWERED IN ANY RUN" : anyNo ? "SPLIT" : "all answered somewhere";
    console.log(`── ${f}  (${kind})`);
    for (const k of sorted) console.log(`   ${vector(k).join("")}  ${k}`);
}
console.log("");
console.log("This tool REPORTS; it exits 0 by design. It is not a gate and must not become one: a fixture row");
console.log("count is a property of the fixture and of one interleaving, and a build that failed on it would");
console.log("be failing on how the scheduler spent a budget.");
