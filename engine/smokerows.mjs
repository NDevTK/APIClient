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
 * A row at 0 in every table is consistent with THREE states and this tool cannot separate them:
 *   (a) the mechanism is missing — the statement cannot be answered by this engine at all;
 *   (b) the mechanism exists and no flow reached it before the stage's CPU budget was spent;
 *   (c) the mechanism exists and this run's interleaving did not schedule it, where another run would.
 * The smoke stage is budget-killed rather than run to completion, and this host has no CPU clock, so (b) and
 * (c) are live for every row. Quoting a row as an absent capability requires evidence this tool does not have.
 * What it IS good for is SHAPE: a family whose early rows answer and whose later rows are uniformly 0 says
 * something a single number cannot, because the split is inside one family that one run exercised.
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────── */

import { readFileSync } from "node:fs";

const path = process.argv[2];
if (!path) {
    console.error("usage: node engine/smokerows.mjs <smoke log>");
    console.error("  the smoke stage names its live log in the build output; a finished build's copy is kept");
    console.error("  beside the build log by whatever froze the snapshot.");
    process.exit(2);
}

const text = readFileSync(path, "utf8");

/* A row is `name=<digits>` on an `@H` line. Anchored at line start so an `@H` quoted inside a message is not
   read as a table. */
const tables = text.split("\n").filter((l) => l.startsWith("@H "));
if (tables.length === 0)
    throw new Error(`${path}: no @H table lines — either this is not a smoke log, or the fixture's report shape ` +
                    `changed and this tool is measuring a subset without saying so, which is the failure it ` +
                    `exists to avoid. Read the fixture's own emitter before editing this pattern.`);

const best = new Map();
for (const line of tables)
    for (const m of line.matchAll(/([a-z][a-z0-9-]*)=(\d+)/g)) {
        const k = m[1], v = Number(m[2]);
        best.set(k, Math.max(best.get(k) ?? 0, v));
    }
if (best.size === 0) throw new Error(`${path}: @H lines carry no name=value pairs — the row spelling changed`);

const answered = [...best].filter(([, v]) => v > 0).map(([k]) => k).sort();
const never = [...best].filter(([, v]) => v === 0).map(([k]) => k).sort();

/* GROUPED BY FAMILY, because the split INSIDE a family is the readable signal and a flat list of 155 names is
   not. The family is the row name's first hyphen-separated segment — the fixture's own naming, not a taxonomy
   invented here. A family whose early rows answer and whose later rows are uniformly 0 is one run's evidence
   about where a ladder stops; a family that is entirely 0 is consistent with never having been scheduled. */
const fam = (k) => (k.includes("-") ? k.slice(0, k.indexOf("-")) : k);
const families = new Map();
for (const [k, v] of best) {
    const f = fam(k);
    if (!families.has(f)) families.set(f, { yes: [], no: [] });
    families.get(f)[v > 0 ? "yes" : "no"].push(k);
}

console.log(`smoke rows from ${path}`);
console.log(`  ${tables.length} @H table line(s), ${best.size} distinct row name(s) derived from them`);
console.log(`  ${answered.length} answered at least once, ${never.length} never answered in any table`);
console.log("");
console.log("SPLIT FAMILIES — some rows answered and some not, which is one run's evidence about where a");
console.log("ladder stops. These are the readable ones: the family WAS exercised, so a 0 beside a 1 is not");
console.log("a scheduling accident in the way a wholly-absent family's zeros may be.");
for (const [f, g] of [...families].sort()) {
    if (g.yes.length === 0 || g.no.length === 0) continue;
    console.log(`  ${f}: ${g.yes.length} answered, ${g.no.length} not`);
    console.log(`      answered: ${g.yes.sort().join(" ")}`);
    console.log(`      not:      ${g.no.sort().join(" ")}`);
}
console.log("");
console.log("WHOLLY-UNANSWERED FAMILIES — every row 0. Consistent with a missing mechanism AND with a family");
console.log("this run never scheduled; this tool cannot tell those apart and does not claim to.");
for (const [f, g] of [...families].sort())
    if (g.yes.length === 0) console.log(`  ${f}: ${g.no.sort().join(" ")}`);
console.log("");
console.log("This tool REPORTS; it exits 0 by design. It is not a gate and must not become one: a fixture row");
console.log("count is a property of the fixture and of one interleaving, and a build that failed on it would");
console.log("be failing on how the scheduler spent a budget.");
