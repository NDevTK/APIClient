/* WHICH ABSENT PLATFORM SURFACE THE DOCUMENTS THIS PRODUCT HAS TO RUN ACTUALLY TOUCH.
 *
 * engine/idlgen.mjs answers WHAT IS MISSING. It cannot answer WHICH MISSING THING TO BUILD, and its own count
 * is not a queue anybody can start on: it is sorted by interface surface, so it puts the widest base first and
 * says nothing about whether a page ever names it. This reads the SAME absent set against the committed
 * corpus of real frozen bundles and orders it by USE.
 *
 * IT SPLITS TWO POPULATIONS BECAUSE THEY TAKE OPPOSITE WORK AND ONE IS INVISIBLE TO THE MEMBER COUNT.
 * A member-list diff walks the interfaces it can find, so an interface that does not exist AT ALL contributes
 * ZERO to it — while a page naming one gets a ReferenceError on the first line that touches it, which is the
 * loudest failure available and is the top of the queue rather than absent from it. So:
 *   A. a platform GLOBAL NAME the corpus uses that this tree reaches on no global — build the component;
 *   B. a MEMBER absent on an interface that does exist — build the member in its real component.
 * They are never summed. A name in A makes its interface's B rows moot, so the overlap is printed as its own
 * line rather than double-counted.
 *
 * EVERY POPULATION IS DERIVED FROM THE ARTIFACT THAT OWNS IT, AND EACH DERIVATION REPRODUCES A TOTAL THAT
 * ARTIFACT ALREADY PUBLISHES BEFORE ANY BREAKDOWN OF IT IS PRINTED. A probe that enumerates what some
 * instrument counts is a SECOND implementation of that instrument's selector, and the ways it can differ are
 * invisible in its output — it walks what the tree declares where the subject walks what it installs, and
 * returns a plausible, larger, wrong population. The calibration is free because the subject already prints
 * its own header, so three of them are asserted here and a mismatch THROWS:
 *   - the absent members come from idlgen's own ABSENT lines, and the parse must reproduce its published
 *     "N distinct spec members ... (M across all interfaces)" exactly;
 *   - the platform's global names come from the committed generated browser/platform_names.h, and the count
 *     must reproduce idlgen's published "platform_names.h current — N global names";
 *   - what this tree puts on a global comes from idl_installed.mjs, and the Web IDL §3.8 record count must
 *     reproduce idlgen's published "§3.8 ... N identifier(s) this engine defines on a global".
 * There is no pasted member list and no pasted name list anywhere in this file. A hand list is the second copy
 * of a generated fact, and this project has been wrong about that list before; a probe that silently measures
 * a SUBSET reports a smaller absence and reads as progress, which is the one direction nothing here would
 * catch. Hence: the shape changed is a THROW, never a shorter table.
 *
 * IT PRINTS NO EXPECTED TOTAL AND NO EXPECTED COUNT OF ITS OWN. There is no baseline, no threshold and no
 * allowlist — every one of those is a number about nothing, and a count of what is missing shrinks as people
 * do the work while its only reader is the person about to invalidate it. The exit code is 0 for any corpus:
 * this is an instrument, not a gate. It is non-zero ONLY when a calibration above fails, which is a statement
 * about THIS FILE having gone stale against its subjects and never about the engine.
 *
 * THE CHANNELS ARE THE UNAMBIGUOUS ONES AND THE `.member` CHANNEL IS NOT ONE. In minified code a property
 * read carries no information about the platform: `.name`, `.length`, `.value` and `.replace` are what every
 * ordinary object in a bundle has, so a `.member` count is a count of the English word rather than of the
 * platform member, and weighting it does not help — a channel whose noise is indistinguishable from its
 * signal is discarded rather than scaled. It is still COMPUTED and printed, under its own heading, as a
 * DIAGNOSTIC that orders nothing, because the ban is an easy thing to re-derive as a good idea and the
 * cheapest refutation is the channel's own top rows.
 *
 * THE RECEIVER IS THE OTHER SIDE OF THE SAME DOT AND IS NOT THE SAME CHANNEL. `WebAssembly.instantiate` and
 * `CSS.supports` are bare global reads with no `new`, no `typeof` and no `window.` in front of them, so every
 * channel above misses them — and the identifier being read is the RECEIVER, not the property. The two sides
 * fail differently and the difference is measured rather than argued: over this corpus the property side is
 * 117552 occurrences whose top rows are `.length`, `.name` and `.value`, while the receiver side filtered to
 * names the platform owns is 50 occurrences across 14 names, and the receivers it excludes are the
 * single-letter minifier locals (`D`, `Q`, `C`, `S`) that cannot collide with a platform name at all. So the
 * receiver side is kept and the property side is not. Its measured price is the honest reason: it added NO
 * new row to list A and re-weighted three, moving one of them to the top — a widening worth taking because it
 * changed the answer, not because it enlarged the table.
 *
 * `instanceof` IS READ ON THE RIGHT. The platform interface in `x instanceof Y` is Y; naming it on the LEFT
 * asks whether an interface OBJECT is an instance of something, which no bundle does. Both are counted and
 * the left-hand one is printed as a CONTROL, so the reading is a measurement rather than an assertion.
 *
 * ITS BLIND SPOT IS THE AUDITED TREE'S BOUNDARY AND IT IS DECLARED, NOT HIDDEN. idl_installed.mjs walks
 * engine/host/browser, so a name the JS ENGINE installs is reported absent while a page reaches it perfectly
 * well. That is not hypothetical: `btoa` and `atob` are Window members defined in the submodule's own
 * quickjs.c, so they arrive in list A and are not gaps. A static derivation over source text is a LOWER BOUND
 * on what is installed wearing a total's clothes, so list A is a CEILING on absence, and every row carries a
 * `qjs` column — occurrences of the name as a quoted string under engine/qjs — so the rows this boundary
 * makes suspect are visible in the output instead of having to be remembered. A non-zero qjs column is a row
 * to READ before believing; it is not itself proof of an install, because a name can be a submodule module
 * export (`quickjs-libc.c`'s os `Worker`) that no Window realm ever sees.
 *
 *   node engine/absentrank.mjs [--host <tree>] [--corpus <dir>] [--top N]
 *
 * --host moves only the SUBJECT, for the reason idlgen gives: a number that is to be quoted at a revision is
 * taken over a frozen snapshot. The corpus and the IDL stay relative to this file. */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join, extname, resolve, relative } from "node:path";
import { loadEnvironment, installedMembers } from "./idl_installed.mjs";
import { loadIdl } from "./idl_members.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const argOf = (flag, dflt) => {
  const i = process.argv.indexOf(flag);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt;
};
const HOST = resolve(argOf("--host", join(HERE, "host")));
const CORPUS = resolve(argOf("--corpus", join(HERE, "..", "testing", "corpus", "mirror")));
const TOP = Number(argOf("--top", "20"));
const say = (s) => console.log(`[absentrank] ${s}`);
const die = (s) => { throw new Error(`[absentrank] CALIBRATION FAILED — ${s}`); };

/* ---- the subject speaks first ------------------------------------------------------------------------- */
/* idlgen EXITS NON-ZERO BY DESIGN: it is RED while any gap stands, which is every run. So a non-zero status
   is not an error here and must not be read as one — what is read is its stdout, and a run that produced no
   parsable totals is the failure. It compiles nothing (it reads .idl and .c), so this spawn is not a build. */
let audit;
try {
  audit = execFileSync(process.execPath, [join(HERE, "idlgen.mjs"), "--host", HOST],
                       { encoding: "utf8", maxBuffer: 1 << 28, stdio: ["ignore", "pipe", "pipe"] });
} catch (e) {
  audit = (e && typeof e.stdout === "string") ? e.stdout : "";
  if (!audit) die(`engine/idlgen.mjs produced no stdout: ${e && e.message}`);
}

const pub = (re, what) => {
  const m = re.exec(audit);
  if (!m) die(`engine/idlgen.mjs no longer prints ${what} — this file parses a format that has changed, and a ` +
              `silently smaller population would read as progress. Re-read its output and fix the parse.`);
  return m;
};
const totals = pub(/(\d+) distinct spec members this engine does not install \((\d+) across all interfaces/,
                   "its absent-member totals");
const PUB_DISTINCT = Number(totals[1]), PUB_PAIRS = Number(totals[2]);
/* TWO THINGS SILENCE THIS LINE AND THE MESSAGE NAMES BOTH, because one of them is not a format change and a
   reader sent to fix the parse would find nothing wrong with it: idlgen prints `platform_names.h current — N`
   only while the checked-in table MATCHES the corpus, and prints `platform_names.h STALE — ...` otherwise.
   Measured by removing one name from the table: this guard is what fires, ahead of the count assertion below,
   and the table was stale rather than reshaped. Either way the population this file would rank is not the
   population the engine compiles against, which is the one outcome that must never be a quiet shorter list. */
const PUB_NAMES = Number(pub(/platform_names\.h current — (\d+) global names/,
                             "`platform_names.h current — N global names` (it prints STALE instead when the " +
                             "checked-in table disagrees with the corpus — regenerate it with `node " +
                             "engine/idlgen.mjs --regen` before ranking anything against it)")[1]);
const PUB_G38 = Number(pub(/(\d+) identifier\(s\) this engine defines on a global/, "the Web IDL §3.8 census")[1]);

/* ---- population B: the absent members, from the auditor's own rows ------------------------------------- */
/* A row is `<Interface> (files): ABSENT n — a, b, c`, with other segments (CONDITIONAL, UNPROVEN) separated
   by " | ". Only the ABSENT segment is read, and only its own stated arity is trusted: a segment whose list
   length disagrees with its own count is a parse this file got wrong, not a finding. */
const absentBy = new Map();
const ROW = /^\[idl-audit\] ([A-Za-z_][\w]*)(?: \([^)]*\))?: (.+)$/;
for (const line of audit.split("\n")) {
  const m = ROW.exec(line);
  if (!m) continue;
  for (const seg of m[2].split(" | ")) {
    const a = /^ABSENT (\d+) — (.+)$/.exec(seg.trim());
    if (!a) continue;
    const names = a[2].split(", ").map((s) => s.trim()).filter(Boolean);
    if (names.length !== Number(a[1]))
      die(`${m[1]}'s ABSENT segment says ${a[1]} and lists ${names.length} — the row format changed.`);
    absentBy.set(m[1], names);
  }
}
const pairs = [...absentBy.values()].reduce((n, v) => n + v.length, 0);
const distinct = new Set([...absentBy.values()].flat());
if (distinct.size !== PUB_DISTINCT || pairs !== PUB_PAIRS)
  die(`parsed ${distinct.size} distinct / ${pairs} pairs from the ABSENT rows and engine/idlgen.mjs publishes ` +
      `${PUB_DISTINCT} / ${PUB_PAIRS}. This file is reading a subset of its subject.`);

/* ---- population A: the platform's global names, minus what this tree reaches on a global ---------------- */
const HDR = join(HOST, "browser", "platform_names.h");
const blk = /static const char \*const PLATFORM_NAMES\[\] = \{([\s\S]*?)\n\};/.exec(readFileSync(HDR, "utf8"));
if (!blk) die(`${relative(HERE, HDR)} no longer holds a PLATFORM_NAMES[] block this file can read.`);
const PLATFORM = new Set([...blk[1].matchAll(/"([^"]+)"/g)].map((m) => m[1]));
if (PLATFORM.size !== PUB_NAMES)
  die(`read ${PLATFORM.size} names out of platform_names.h and engine/idlgen.mjs publishes ${PUB_NAMES}.`);

/* WHAT A PAGE CAN REACH OFF THE GLOBAL IS TWO KINDS OF NAME AND platform_names.h HOLDS BOTH, so a denominator
   built from one of them reports the other kind absent wholesale. Web IDL §3.8 `define the global property
   references` puts INTERFACE OBJECTS there (`IntersectionObserver`), and Window's own flattened member list
   puts MEMBERS there (`location`, `localStorage`, `addEventListener`) — and §3.8 is the only one idl_installed
   marks, because it is the only one that is not a member. Reading §3.8 alone reported `location`,
   `addEventListener`, `localStorage`, `document`, `navigator`, `requestAnimationFrame` and `matchMedia` as
   absent globals, all of which this engine installs, and it put them at the TOP of the ranking because they
   are the names a bundle touches most. The member half is therefore taken as well: an install whose target is
   Window — or any interface Window inherits from, the chain read from the corpus rather than named here — is
   a name a page reaches on the global. */
const idl = await loadIdl();
const chain = ["Window"];
for (let n = "Window"; ;) {
  const base = idl.inheritanceOf.get(n);
  if (!base || chain.includes(base)) break;
  chain.push(base); n = base;
}
if (!chain.includes("EventTarget"))
  die(`Window's inheritance chain read as ${chain.join(" -> ")} and does not reach EventTarget. Window's ` +
      `event members are reached through it, so a chain without it reports addEventListener absent.`);

const env = loadEnvironment(HOST);
const world = installedMembers([...env.sources.keys()].filter((p) => p.endsWith(".c")), env);
const onGlobal = new Set(chain);
const REACHED = new Set();
let n38 = 0, nMember = 0;
for (const r of world.records) {
  if (r.globalRef) { REACHED.add(r.name); n38++; continue; }
  if ([...(r.ifaces || []), ...(r.candidates || [])].some((i) => onGlobal.has(i))) { REACHED.add(r.name); nMember++; }
}
if (n38 !== PUB_G38)
  die(`counted ${n38} Web IDL §3.8 global defines and engine/idlgen.mjs publishes ${PUB_G38}.`);
const ABSENT_GLOBAL = new Set([...PLATFORM].filter((n) => !REACHED.has(n)));

/* ---- the corpus ---------------------------------------------------------------------------------------- */
const files = [];
(function walk(d) {
  for (const e of readdirSync(d)) {
    const p = join(d, e);
    if (statSync(p).isDirectory()) { walk(p); continue; }
    if ([".js", ".mjs", ".html", ".htm"].includes(extname(p).toLowerCase())) files.push(p);
  }
})(CORPUS);
if (!files.length) die(`no .js/.html under ${CORPUS} — a corpus that is not there reads as a corpus with no uses.`);
let bytes = 0;
const parts = [];
for (const f of files) { const t = readFileSync(f, "utf8"); bytes += Buffer.byteLength(t); parts.push(t); }
const SRC = parts.join("\n;/*absentrank-file-break*/;\n");

/* Occurrences, never lines: a minified bundle is ONE enormous line, which is exactly where the two diverge. */
const CHANNELS = {
  "new X(":        /\bnew\s+([A-Za-z_$][\w$]*)\s*\(/g,
  "window.X":      /\bwindow\.([A-Za-z_$][\w$]*)/g,
  "self.X":        /\bself\.([A-Za-z_$][\w$]*)/g,
  "globalThis.X":  /\bglobalThis\.([A-Za-z_$][\w$]*)/g,
  "instanceof X":  /\binstanceof\s+([A-Za-z_$][\w$]*)/g,
  "typeof X":      /\btypeof\s+([A-Za-z_$][\w$]*)/g,
  'global["X"]':   /\b(?:window|self|globalThis)\[\s*["']([A-Za-z_$][\w$]*)["']\s*\]/g,
  "X.member":      /(?:^|[^\w$.])([A-Z][\w$]*)\s*\.\s*[A-Za-z_$][\w$]*/g,
};
const CONTROL = {
  "X instanceof (control)":   /\b([A-Za-z_$][\w$]*)\s+instanceof\b/g,
  ".member (discarded)":      /\.([A-Za-z_$][\w$]*)/g,
  "X.member unfiltered (ctl)": /(?:^|[^\w$.])([A-Z][\w$]*)\s*\.\s*[A-Za-z_$][\w$]*/g,
};
const tally = (re) => {
  const out = new Map();
  for (const m of SRC.matchAll(re)) out.set(m[1], (out.get(m[1]) || 0) + 1);
  return out;
};

/* EVERY CHANNEL IS SHOWN MATCHING AND SHOWN NOT MATCHING BEFORE ANY OF ITS COUNTS ARE READ. A channel that
   reports 0 over the corpus is saying either "the corpus does not do this" or "my pattern cannot see it", and
   those render identically — `global["X"]` reads 0 here, and a zero from an unarmed pattern is the reading
   that would quietly shrink list A. So each pattern is run against a string that contains its form and a
   string that contains a near miss, and a pattern that fails either is a THROW rather than a quiet row. */
const ARM = {
  "new X(":        ['new AbsentRankPos(1)',           'renew AbsentRankNeg('],
  "window.X":      ['window.AbsentRankPos',           'mywindow.AbsentRankNeg'],
  "self.X":        ['self.AbsentRankPos',             'myself.AbsentRankNeg'],
  "globalThis.X":  ['globalThis.AbsentRankPos',       'xglobalThis.AbsentRankNeg'],
  "instanceof X":  ['x instanceof AbsentRankPos',     'x.instanceofAbsentRankNeg'],
  "typeof X":      ['typeof AbsentRankPos',           'mytypeof AbsentRankNeg'],
  'global["X"]':   ['window["AbsentRankPos"]',        'notwindow["AbsentRankNeg"]'],
  "X.member":      ['AbsentRankPos.someMember',       'q.AbsentRankNeg.someMember'],
};
for (const [k, re] of Object.entries(CHANNELS)) {
  const [pos, neg] = ARM[k] || die(`channel ${k} has no positive/negative control — add one before reading it.`);
  const got = (s) => [...s.matchAll(new RegExp(re.source, "g"))].map((m) => m[1]);
  if (!got(pos).includes("AbsentRankPos")) die(`channel ${k} did not match its own form in ${JSON.stringify(pos)} — its 0 would mean nothing.`);
  if (got(neg).includes("AbsentRankNeg")) die(`channel ${k} matched its near miss ${JSON.stringify(neg)} — it is counting something else.`);
}
const hits = new Map();          /* name -> channel -> occurrences */
const perChannel = new Map();
for (const [k, re] of Object.entries(CHANNELS)) {
  const t = tally(re);
  perChannel.set(k, t);
  for (const [n, c] of t) {
    if (!hits.has(n)) hits.set(n, new Map());
    hits.get(n).set(k, c);
  }
}
const controls = new Map();
for (const [k, re] of Object.entries(CONTROL)) controls.set(k, tally(re));
const uses = (n) => [...(hits.get(n) || new Map()).values()].reduce((a, b) => a + b, 0);
const shape = (n) => [...(hits.get(n) || new Map())].map(([k, v]) => `${k}=${v}`).join(" ");

/* WHAT AN ABSENCE COSTS IS NOT HOW OFTEN THE NAME APPEARS, AND ORDERING BY COUNT GETS IT BACKWARDS.
   A name a bundle FEATURE-DETECTS costs nothing when it is absent: `"undefined" != typeof X` answers false,
   the fallback branch runs, and a real browser without X answers the same way — so the read is correctly not
   forked and there is nothing to build for. A name a bundle USES WITHOUT DETECTING IT throws a ReferenceError
   on the line that touches it and takes the whole flow with it. The two are opposite outcomes and the counts
   do not separate them, so the channels are split by which they are evidence of: a GUARD channel reads the
   name in a position where absence is `undefined`, and a USE channel reads it in a position where absence
   THROWS (`new X(`, `X.member`, `x instanceof X` — every one of those evaluates X as a binding).
   The class is stated over the WHOLE corpus rather than per site, which is what makes it sound in the
   direction it is read: a name with NO guard hit anywhere cannot have a guarded use, so THROWS is a claim the
   text supports outright. `mixed` is the honest middle — some use and some guard exist and only reading the
   site says which covers which — and it is not promoted above THROWS on the strength of a bigger number. */
const GUARD_CH = new Set(["typeof X", "window.X", "self.X", "globalThis.X", 'global["X"]']);
const USE_CH = new Set(["new X(", "X.member", "instanceof X"]);
const partOf = (n, set) => [...(hits.get(n) || new Map())].filter(([k]) => set.has(k)).reduce((a, b) => a + b[1], 0);
const klass = (n) => {
  const u = partOf(n, USE_CH), g = partOf(n, GUARD_CH);
  if (u && !g) return "THROWS";
  if (u && g) return "mixed";
  return "detect-only";
};
const RANK = { THROWS: 0, mixed: 1, "detect-only": 2 };

/* The submodule string witness for the declared boundary above. Read as a REASON TO OPEN THE FILE. */
const QJS = join(HERE, "qjs");
let qjsSrc = "";
(function walk(d) {
  let ents; try { ents = readdirSync(d); } catch { return; }
  for (const e of ents) {
    const p = join(d, e);
    let st; try { st = statSync(p); } catch { continue; }
    if (st.isDirectory()) { walk(p); continue; }
    if ([".c", ".h"].includes(extname(p))) qjsSrc += readFileSync(p, "utf8");
  }
})(QJS);
const qjsHits = (n) => qjsSrc ? (qjsSrc.match(new RegExp(`"${n.replace(/[$]/g, "\\$")}"`, "g")) || []).length : -1;

/* ---- report --------------------------------------------------------------------------------------------- */
say(`subject ${HOST}`);
say(`calibration — absent members ${distinct.size} distinct / ${pairs} pairs, platform names ${PLATFORM.size}, ` +
    `Web IDL §3.8 defines ${n38}: all three reproduce engine/idlgen.mjs's own published totals`);
say(`this tree reaches ${REACHED.size} distinct name(s) on a global — ${n38} §3.8 define(s) and ${nMember} ` +
    `member install(s) on ${chain.join("/")} — leaving ${ABSENT_GLOBAL.size} platform global name(s) it does not`);
say(`corpus ${relative(join(HERE, ".."), CORPUS)} — ${files.length} file(s), ${bytes} byte(s)`);
for (const [k, t] of perChannel)
  say(`  channel ${k.padEnd(15)} ${String([...t.values()].reduce((a, b) => a + b, 0)).padStart(6)} occurrence(s), ` +
      `${String(t.size).padStart(4)} distinct identifier(s)`);
for (const [k, t] of controls) {
  const top = [...t].sort((a, b) => b[1] - a[1]).slice(0, 6);
  const plat = top.filter(([n]) => PLATFORM.has(n)).length;
  say(`  control ${k.padEnd(24)} ${String([...t.values()].reduce((a, b) => a + b, 0)).padStart(7)} occurrence(s); ` +
      `top ${top.map(([n, c]) => `${n}=${c}`).join(" ")} — ${plat} of those ${top.length} is a platform name`);
}

console.log("");
say(`── A. PLATFORM GLOBAL NAMES THE CORPUS USES THAT THIS TREE REACHES ON NO GLOBAL ──`);
say(`   A page naming one of these gets a ReferenceError on the line that touches it, so none of them is in ` +
    `the ${distinct.size}-member count: an interface that does not exist has no members to be missing.`);
say(`   The count is a CEILING. qjs = times the name occurs as a quoted string under engine/qjs, which is ` +
    `outside the audited tree; a non-zero qjs is a row to read before believing.`);
say(`   ORDERED BY WHAT THE ABSENCE COSTS, NOT BY VOLUME: THROWS (every use of this name is unguarded, so it ` +
    `raises a ReferenceError and ends the flow) before mixed (both forms present — read the site) before ` +
    `detect-only (the corpus only ever feature-detects it, so absence is the answer a browser without it gives).`);
const rankA = [...ABSENT_GLOBAL].filter((n) => hits.has(n))
  .sort((a, b) => RANK[klass(a)] - RANK[klass(b)] || uses(b) - uses(a) || a.localeCompare(b));
const nThrow = rankA.filter((n) => klass(n) === "THROWS").length;
say(`   ${rankA.length} of ${ABSENT_GLOBAL.size} absent global name(s) are used by this corpus at all; ` +
    `${nThrow} of those ${rankA.length} is/are unguarded.`);
for (const n of rankA.slice(0, TOP))
  say(`   ${klass(n).padStart(11)}  ${String(uses(n)).padStart(4)}  qjs=${String(qjsHits(n)).padStart(3)}  ${n.padEnd(24)} ${shape(n)}`);

console.log("");
say(`── B. INTERFACES THAT EXIST AND CARRY ABSENT MEMBERS, RANKED BY CORPUS USE OF THE INTERFACE NAME ──`);
say(`   Ordered by the unambiguous channels only. A bundle reaches an element through the DOM far more often ` +
    `than it names the interface, so this signal is THIN by construction and the numbers are small: read it ` +
    `as which interfaces the corpus names, never as how much of the page each one carries.`);
const rankB = [...absentBy.keys()].sort((a, b) => uses(b) - uses(a) || a.localeCompare(b));
for (const n of rankB.slice(0, TOP))
  say(`   ${String(uses(n)).padStart(4)}  ${n.padEnd(26)} ABSENT ${String(absentBy.get(n).length).padStart(3)}  ${shape(n) || "(named nowhere in the corpus)"}`);

const overlap = rankB.filter((n) => ABSENT_GLOBAL.has(n));
console.log("");
say(`── OVERLAP — an interface in BOTH populations ──`);
say(`   ${overlap.length} interface(s) carry absent members AND are reached on no global here, so their ` +
    `member rows are moot until the name resolves: ${overlap.length ? overlap.map((n) => `${n}(${absentBy.get(n).length}, used ${uses(n)})`).join(", ") : "none"}`);

const dot = controls.get(".member (discarded)");
console.log("");
say(`── DIAGNOSTIC, ORDERS NOTHING — the .member channel over the ${distinct.size} absent member names ──`);
say(`   Printed to be refuted, not used. A property read in minified code carries no information about the ` +
    `platform, so these numbers are an upper bound contaminated by every ordinary object in the bundle.`);
for (const [n, c] of [...distinct].map((n) => [n, dot.get(n) || 0]).sort((a, b) => b[1] - a[1]).slice(0, 12))
  say(`   ${String(c).padStart(5)}  .${n}`);
