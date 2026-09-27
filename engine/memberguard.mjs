/* WHAT AN ABSENT WEB IDL *MEMBER* COSTS AT EVERY SITE A REAL CORPUS CALLS IT — the guarded question, asked
 * for a member instead of for a global name.
 *
 * WHY IT EXISTS. engine/absentkind.mjs decides an absent member's cost from the member's KIND with no corpus
 * at all, and says so in its own header: its answer is what the language does at an UNGUARDED use, and the
 * guarded question "needs a corpus and engine/absentrank.mjs is what asks it for the names it can anchor".
 * The names it can anchor are GLOBALS — engine/js_guard_shape.mjs's receiver set is `window`, `self`,
 * `globalThis`, and its ARM_UNLOCATED table asserts that a member property reaches no verdict at all. So the
 * question §NO-STUBS asks before EVERY member landing — may I install this, or does installing flip a
 * page's presence test onto a branch that cannot complete — had no instrument, and was answered by reading
 * sites by hand. This is that instrument, over `memberGuardShapeReader`.
 *
 * IT IS PER NAME AND A CORPUS-WIDE RANKING IS REFUSED, WHICH IS MEASURED RATHER THAN A SCOPE CHOICE. A
 * MEMBER NAME IS NOT OWNED BY AN INTERFACE. Feeding this reader the whole absent-member population — the 471
 * names engine/idlgen.mjs's ABSENT rows carry — answers 35321 call sites over the corpus below, of which
 * 89% stand on FIFTEEN names: `add`, `filter`, `replace`, `all`, `parse`, `decode`, `clear`, `request`,
 * `close`, `reset`, `setTimeout`, `cols`, `fill`, `start`, `text`. Every one of those is `Set.prototype.add`
 * or `Array.prototype.filter` or `String.prototype.replace` or `Promise.all` at almost all of its sites —
 * members that are PRESENT, on receivers that have nothing to do with the interface the IDL row is about.
 * Nothing here knows a receiver's TYPE, so nothing here can separate them; inventing one would be the scope
 * analysis engine/absentrank.mjs built, measured and DECLINED. A ranked table over that population would be
 * a number ordering noise, so it is not printed at any flag. WHAT A CALLER SUPPLIES IS THE NAME, which is
 * also the shape of the question actually being asked: a lane deciding one landing knows its member.
 * SO THE RECEIVERS ARE PRINTED, because whether a name is distinctive is the reader's to judge and cannot be
 * judged from a count: 21 sites on `setPointerCapture` is a whole population, and 12830 on `add` is a
 * different question wearing the same column.
 *
 * WHAT IT MEASURES AND WHAT IT DOES NOT. `sites` counts CALLS, which is the subject
 * memberGuardShapeReader's header argues for at length — an absent member READ answers `undefined` and
 * raises nothing, so only ECMAScript §13.3.6.2 "EvaluateCall ( func , thisValueRef , argumentListNode ,
 * tailPosition )"' "If func is not an Object, throw a TypeError exception" ends a flow. `accesses` counts
 * every access including the ones that reach no verdict, so the split is reported rather than inferred from
 * a smaller list. `thr` is the only verdict that ends a flow; `sil` is the one §NO-STUBS is about, because
 * installing the member flips that test TRUE and takes the page off a branch that runs today.
 *
 * NO FILE IS SKIPPED FOR ITS SIZE, AND THAT IS A DEFECT THIS TOOL WAS BUILT WITH AND CAUGHT. The probe that
 * produced the first figures here carried a 12MB cap, chosen so a parse could not blow the heap — and the
 * corpus's single largest file is 13.9MB and held BOTH of the sites that separated a correct reading from a
 * wrong one. A cap silently removed the biggest bundle, in the DEMOTING direction, and the output read as
 * complete. So there is no cap, a file that cannot be read or parsed is REPORTED with the parser's own
 * reason, and NOTHING is claimed about its sites — which is the contract absentrank already states for the
 * same reader and the reason its own "3 file(s) ... did not parse" line exists.
 *
 * THE CORPUS IS FETCHED RATHER THAN COMMITTED, so the derivation is the command and the run.json stamp
 * beside it, never the figure:
 *     NODE_USE_ENV_PROXY=1 SITES=apps.tsv node testing/corpus/fetch.mjs   # prints the --corpus path
 *     node engine/memberguard.mjs --corpus <that path>/mirror --member setPointerCapture --member ... */
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { memberGuardShapeReader, MEMBER_VERDICTS } from "./js_guard_shape.mjs";

const argsOf = (flag) => {
  const out = [];
  for (let i = 0; i < process.argv.length; i++)
    if (process.argv[i] === flag && process.argv[i + 1]) out.push(process.argv[i + 1]);
  return out;
};
const CORPUS = argsOf("--corpus")[0] || null;
const MEMBERS = argsOf("--member");
if (!CORPUS || !MEMBERS.length)
  throw new Error("[memberguard] --corpus <dir> and at least one --member <name> are REQUIRED. The corpus is "
                  + "a directory of responses a real-network fetch wrote; testing/corpus/fetch.mjs prints the "
                  + "path and stamps run.json beside it. A ranking over the whole absent-member population is "
                  + "deliberately not offered — see this file's header for the measurement that refuses it.");

/* The reader ARMS ITSELF on construction and throws rather than returning one that cannot speak, so this is
   the calibration and there is nothing further for this file to assert about it. */
const R = memberGuardShapeReader();

const files = [];
(function walk(d) {
  for (const e of readdirSync(d, { withFileTypes: true })) {
    const p = join(d, e.name);
    if (e.isDirectory()) walk(p); else files.push(p);
  }
})(CORPUS);

const names = new Set(MEMBERS);
const per = new Map();
for (const n of names) per.set(n, { sites: [], apps: new Set() });
let accesses = 0, scanned = 0;
const unparsed = [];
for (const f of files) {
  let src;
  try { src = readFileSync(f, "utf8"); } catch (e) { unparsed.push([f, `read: ${e && e.message}`]); continue; }
  let touches = false;
  for (const n of names) if (src.includes(n)) { touches = true; break; }
  if (!touches) continue;
  scanned++;
  const r = R.classify(src, names);
  if (!r.parsed) { unparsed.push([f, r.why]); continue; }
  accesses += r.seen;
  const app = f.slice(CORPUS.length).replace(/^\/+/, "").split("/")[0];
  for (const s of r.sites) { const b = per.get(s.name); b.sites.push({ ...s, app, file: f }); b.apps.add(app); }
}

const say = (s) => console.log(`[memberguard] ${s}`);
say(`${files.length} file(s) under ${CORPUS}; ${scanned} name the member(s); ${R.armed} control(s) armed.`);
say(`${accesses} member access(es) of the named member(s), of which the CALL sites below are the subject — a `
    + `member access that is not called answers undefined and raises nothing, so it reaches no verdict.`);
if (unparsed.length) {
  say(`${unparsed.length} file(s) naming a member did NOT parse and NOTHING is claimed about their sites:`);
  for (const [f, why] of unparsed) say(`    ${f.slice(CORPUS.length)} — ${why}`);
} else {
  say(`0 file(s) failed to read or parse — every file naming a member was classified, with no size cap.`);
}
for (const n of MEMBERS) {
  const b = per.get(n);
  const t = Object.fromEntries(MEMBER_VERDICTS.map((v) => [v, 0]));
  for (const s of b.sites) t[s.verdict]++;
  say("");
  say(`${n}: ${b.sites.length} CALL site(s) over ${b.apps.size} app(s) — `
      + `thr=${t["throws"]} cgt=${t["caught"]} fb=${t["guarded-fallback"]} sil=${t["guarded-silent"]}`);
  say(`   thr ENDS THE FLOW today and would go on ending it with the member installed only if the receiver `
      + `implements the interface, which nothing here knows; sil is the §NO-STUBS hazard — installing flips `
      + `that test TRUE and takes the page off a branch that RUNS today.`);
  const dem = b.sites.filter((s) => s.verdict !== "throws");
  if (!dem.length) { say(`   no demoted site: every call is unguarded.`); continue; }
  say(`   ${dem.length} DEMOTED site(s), listed because a demotion's error is the one nobody finds by acting `
      + `on it, so each is here to be OPENED:`);
  for (const s of dem)
    say(`      ${s.verdict.padEnd(16)} recv=${s.receiver === null ? "(incomparable)" : s.receiver}  `
        + `${s.app}  ${s.file.slice(CORPUS.length)}@${s.offset}`);
}
