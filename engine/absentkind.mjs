/* WHAT AN ABSENT WEB IDL MEMBER COSTS A PAGE, DECIDED BY THE MEMBER'S KIND AND BY NOTHING ELSE.
 *
 * engine/idlgen.mjs answers WHAT IS MISSING and engine/absentrank.mjs orders the part of it a real corpus
 * NAMES. Between them sits a question neither asks and this file answers with no corpus at all: given that a
 * member is absent, WHAT DOES THE PAGE SEE? That is a fact about the member's KIND in the platform's own IDL
 * and about two ECMAScript abstract operations, so it needs no bundle, no network and no instant — which is
 * why it is a separate reading rather than a fourth channel of absentrank.
 *
 * THE THREE OUTCOMES, AND THEY TAKE OPPOSITE WORK:
 *   operation                a page's next token is `(`, so the absent member is `undefined(…)`. ECMAScript
 *                            §13.3.6.2 "EvaluateCall ( func , thisValueRef , argumentListNode , tailPosition )"
 *                            step 3 is "If func is not an Object, throw a TypeError exception." — so the flow
 *                            ENDS on that line. This is the SAME cost §NO-STUBS names for an absent global's
 *                            ReferenceError, reached through a member instead of through a name.
 *   attribute                ECMAScript §10.1.8.1 "OrdinaryGet ( obj , propertyKey , receiver )" returns
 *                            undefined and nothing throws, so what the absence costs is decided by whatever
 *                            CONSUMES that undefined a statement later. NO cost is claimed here and none is
 *                            invented — engine/absentrank.mjs's B(anchored) says exactly this and is right.
 *   event-handler attribute  an assignment to an absent IDL attribute creates an ORDINARY own property, reads
 *                            its own value back, and throws nothing — and the handler is then registered with
 *                            no algorithm that can ever fire it. No crash, no fork, no signal. That is the
 *                            SILENT loss §NO-STUBS is about, and it is the one outcome nothing else here sees.
 *
 * WHY THIS IS NOT A SECOND COPY OF absentrank's BAND. That band is a COST CLASS over absent GLOBAL NAMES and
 * is derived from a CORPUS — it says which of a page's uses are guarded. This is derived from the IDL and says
 * what the language does at an unguarded one. The two are orthogonal and neither substitutes: a corpus can
 * tell you a name is only ever feature-detected, and only the IDL can tell you that the unguarded uses of a
 * DIFFERENT member end the flow. A component in this tree reached the same split independently, at
 * core/canvas/canvas_rendering_context_2d.c's rect-painter residual, in its own words ("a page calling an
 * absent METHOD gets Web IDL's TypeError and its flow ends there … while a page ASSIGNING an absent attribute
 * creates an ordinary property on the context, reads its own value back and gets no error at all"), which is
 * the second derivation of the rule and the reason it is worth an instrument rather than a sentence.
 *
 * IT REPRODUCES idlgen's OWN PUBLISHED TOTALS BEFORE PRINTING ANY BREAKDOWN, AND THROWS OTHERWISE. A probe
 * that enumerates what another instrument counts is a SECOND implementation of that instrument's selector, and
 * the ways it can differ are invisible in its output. The calibration is free because the subject prints its
 * own header, and it has already earned its keep: the first version of this file asserted the parsed pair
 * against idlgen's "(M counting each interface that DECLARES one)" figure and THREW at 3477 against 686 —
 * because idlgen's per-interface ABSENT rows are each interface's WHOLE FLATTENED surface, so an inherited gap
 * prints on every row below its declarer and the rows sum to the THIRD published number. A run that printed a
 * plausible breakdown over the wrong denominator would have been unfalsifiable.
 *
 * IT PRINTS NO EXPECTED TOTAL, NO BASELINE AND NO THRESHOLD, and its exit code is 0 for any tree: this is an
 * instrument and not a gate. A count of what is missing shrinks as people do the work and its only reader is
 * the person about to invalidate it, so what this hands over is the SPLIT and the command, never a figure to
 * quote. It is non-zero ONLY when a calibration above fails, which is a statement about THIS FILE having gone
 * stale against its subject and never about the engine.
 *
 * A NAME IS NOT ONE KIND ACROSS THE PLATFORM, AND THE CLASSES THEREFORE DO NOT PARTITION THE NAMES. `length`
 * is an attribute on one interface and an operation's name on another, so a name declared as both prints under
 * a joined key rather than being assigned to whichever was seen first — an assignment would be a silent choice
 * this file has no basis to make. The PAIR breakdown has no such ambiguity in principle and is the number a
 * per-interface lane is cut from, so both are printed and neither is summed with the other.
 *
 *   node engine/absentkind.mjs [--host <tree>]
 *
 * §Testing: a reading that is to be quoted at a revision is taken over a FROZEN SUBJECT, because this checkout
 * is edited continuously and a scan of it answers about a program no revision contains. `--host` is how:
 *   git archive <sha> engine/host | tar -x -C <dir>   # then --host <dir>/engine/host
 * Only the SUBJECT moves. The IDL corpus stays relative to THIS FILE, which is why the tool is not copied.
 */
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
import { loadIdl } from "./idl_members.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const argOf = (flag, dflt) => {
  const i = process.argv.indexOf(flag);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt;
};
const HOST = resolve(argOf("--host", join(HERE, "host")));
const say = (s) => console.log(`[absentkind] ${s}`);
const die = (s) => { throw new Error(`[absentkind] ${s}`); };

/* THE SUBJECT IS RUN, NEVER RE-IMPLEMENTED. idlgen exits NON-ZERO whenever it has a finding, which is its
   normal state and must not be read as a failure here: what is read is its stdout, and a run that produced no
   parsable totals is the failure. It compiles nothing — it reads .idl and .c — so this spawn is not a build. */
let audit;
try {
  audit = execFileSync(process.execPath, [join(HERE, "idlgen.mjs"), "--host", HOST],
                       { encoding: "utf8", maxBuffer: 1 << 28, stdio: ["ignore", "pipe", "pipe"] });
} catch (e) {
  audit = (e && typeof e.stdout === "string") ? e.stdout : "";
  if (!audit) die(`engine/idlgen.mjs produced no stdout: ${e && e.message}`);
}
say(`subject ${HOST}`);

/* idlgen's own row format, read the way engine/absentrank.mjs reads it: `<Interface> (files): ABSENT n — a, b`
   with other segments separated by " | ". Only the ABSENT segment is read, and only its own stated arity is
   trusted — a segment whose list does not match its count means the row format moved and this parse is stale. */
const rows = [];
for (const line of audit.split("\n")) {
  const m = /^\[idl-audit\] ([A-Za-z0-9_]+)(?: \([^)]*\))?: (.+)$/.exec(line);
  if (!m) continue;
  for (const seg of m[2].split(" | ")) {
    const a = /^ABSENT (\d+) — (.+)$/.exec(seg.trim());
    if (!a) continue;
    const names = a[2].split(", ").map((s) => s.trim()).filter(Boolean);
    if (names.length !== Number(a[1]))
      die(`${m[1]}'s ABSENT segment says ${a[1]} and lists ${names.length} — the row format changed.`);
    rows.push([m[1], names]);
  }
}
const distinct = new Set();
let flatPairs = 0;
for (const [, ns] of rows) { for (const n of ns) distinct.add(n); flatPairs += ns.length; }

const pub = /(\d+) distinct spec members this engine does not install \((\d+) counting each interface that DECLARES one, and (\d+) across all interfaces/.exec(audit);
if (!pub) die("engine/idlgen.mjs printed no absent-member totals — its wording moved and this parse is stale.");
if (distinct.size !== Number(pub[1]) || flatPairs !== Number(pub[3]))
  die(`parsed ${distinct.size} distinct / ${flatPairs} flattened pair(s); engine/idlgen.mjs publishes ` +
      `${pub[1]} / ${pub[3]}. This file's parse of its rows has gone stale against it.`);
say(`calibrated — ${distinct.size} distinct and ${flatPairs} flattened (interface, member) pair(s) reproduce ` +
    `engine/idlgen.mjs's own published totals exactly; ${pub[2]} is its DECLARING-interface figure and is a ` +
    `different denominator, which is what the first version of this calibration threw on`);

/* THE KIND, FROM THE SAME CORPUS idlgen PARSES — engine/idl_members.mjs's own unmerged declaration list, so
   the partials and mixins that make a member belong to an interface are already folded in by the artifact that
   owns that question rather than by a second walk here. An `on…`-prefixed attribute is split out because its
   ABSENCE behaves differently from an ordinary attribute's: HTML §8.1.7.1 "Event handler attributes" makes the
   setter the thing that registers the handler, so an absent one silently accepts the assignment and registers
   nothing, where an ordinary absent attribute at least leaves a reader holding `undefined` that some later
   statement consumes visibly. */
const IDL = await loadIdl();
const kinds = new Map();
for (const d of IDL.declarations) {
  if (!Array.isArray(d.members)) continue;
  for (const mem of d.members) {
    if (!mem || !mem.name) continue;
    const k = mem.type === "operation" ? "operation"
            : mem.type === "attribute" ? (/^on[a-z]/.test(mem.name) ? "event-handler attribute" : "attribute")
            : mem.type;
    if (!kinds.has(mem.name)) kinds.set(mem.name, new Set());
    kinds.get(mem.name).add(k);
  }
}
const keyOf = (n) => {
  const ks = kinds.get(n);
  return ks ? [...ks].sort().join("+") : null;
};

const report = (label, tally, unknown, total) => {
  say(`${label} — ${total}:`);
  for (const [k, v] of [...tally].sort((a, b) => b[1] - a[1]))
    say(`  ${String(v).padStart(5)}  ${k}`);
  /* A NAME THE WALK DID NOT REACH IS PRINTED AND NEVER DROPPED. Dropping it would make every class above a
     silent under-count, which is the direction a reader reads as progress. */
  if (unknown) say(`  ${String(unknown).padStart(5)}  NOT REACHED by this walk — a FLOOR, printed rather than dropped`);
};

const nameTally = new Map(); let nameUnknown = 0;
for (const n of distinct) {
  const k = keyOf(n);
  if (!k) { nameUnknown++; continue; }
  nameTally.set(k, (nameTally.get(k) || 0) + 1);
}
const pairTally = new Map(); let pairUnknown = 0;
for (const [, ns] of rows) for (const n of ns) {
  const k = keyOf(n);
  if (!k) { pairUnknown++; continue; }
  pairTally.set(k, (pairTally.get(k) || 0) + 1);
}

say("WHAT A PAGE SEES AT AN UNGUARDED USE, by the kind(s) the platform's IDL declares the name as.");
say("  operation                → the call is `undefined(…)`; ECMAScript §13.3.6.2 \"EvaluateCall ( func , " +
    "thisValueRef , argumentListNode , tailPosition )\" step 3 throws a TypeError and the flow ENDS on that line.");
say("  attribute                → ECMAScript §10.1.8.1 \"OrdinaryGet ( obj , propertyKey , receiver )\" " +
    "returns undefined; NO cost is claimed, because it is decided by what consumes the undefined.");
say("  event-handler attribute  → the assignment succeeds onto an ordinary own property and no algorithm can " +
    "ever fire the handler: SILENT, which is the loss §NO-STUBS names and the one nothing else here sees.");
say("  A joined key is a name the platform declares as more than one kind, so these classes do NOT partition " +
    "the names and are not summed with each other.");
report("distinct absent NAME(s)", nameTally, nameUnknown, distinct.size);
report("flattened absent (interface, member) PAIR(s) — the denominator a per-interface lane is cut from",
       pairTally, pairUnknown, flatPairs);

/* THE PER-INTERFACE SPLIT, ORDERED BY THE ONE CLASS THAT ENDS A FLOW — AND SPLIT AGAIN BY WHOSE WORK IT IS,
   WITHOUT WHICH THE ORDER IS MISLEADING RATHER THAN MERELY COARSE. idlgen's ABSENT rows are each interface's
   whole FLATTENED surface, so Element's absent `setPointerCapture` prints on every HTML element row below it
   and a rank over the flattened operation count is dominated by ONE inherited gap repeated forty times. The
   first version of this ranking did exactly that: its top rows were HTMLTableElement, HTMLSelectElement and
   HTMLOutputElement at 29, 26 and 23 absent operations, of which about 23 each were Element's and HTMLElement's,
   and a reader cutting a lane from it would have cut it at the wrong interface. So OWN is printed beside it,
   and OWN is asked of engine/idl_members.mjs's own `ownMembers` rather than derived here: that function exists
   because subtracting a base's members BY NAME is wrong under Web IDL §2.3 Interface mixins and §3.7.3
   Interface prototype object (a derived interface may REDECLARE a name and gets its own property for it), and a
   second answer to that question here is the shape that drifts.
   IT IS NOT A QUEUE, which is the whole of what this column does not claim: it says which interfaces a page
   cannot get PAST, and says nothing about whether a page reaches them. That question needs a corpus and
   engine/absentrank.mjs is what asks it for the names it can anchor. An interface high here and absent from
   absentrank's lists is not thereby unimportant — absentrank's receiver map is derived from Window's
   interface-typed IDL attributes, so an interface a page reaches through the DOM or through an operation's
   RETURN VALUE (a 2D context off `getContext`, a range off `createRange`) is outside what it can rank at all,
   and this column is the only statement in this tree about those. */
const perIface = [];
for (const [iface, ns] of rows) {
  const own = new Set(IDL.ownMembers(iface));
  const c = { iface, n: ns.length, op: 0, at: 0, eh: 0, other: 0, ownOp: 0, ownN: 0 };
  for (const n of ns) {
    const k = keyOf(n);
    if (k === "operation") c.op++;
    else if (k === "attribute") c.at++;
    else if (k === "event-handler attribute") c.eh++;
    else c.other++;
    if (own.has(n)) { c.ownN++; if (k === "operation") c.ownOp++; }
  }
  perIface.push(c);
}
perIface.sort((a, b) => b.ownOp - a.ownOp || b.ownN - a.ownN || a.iface.localeCompare(b.iface));
const TOP = Number(argOf("--top", "25"));
say(`per interface, ranked by the FLOW-ENDING class this interface ITSELF declares — OWN absent operation(s) ` +
    `first, then OWN absent total. ${perIface.length} row(s), showing ${Math.min(TOP, perIface.length)} ` +
    `(--top N for more). The flattened figure counts an inherited gap on every row below its ` +
    `declarer and is therefore NOT what a lane is cut from:`);
for (const r of perIface.slice(0, TOP))
  say(`  ${r.iface.padEnd(30)} OWN ${String(r.ownOp).padStart(3)} operation of ${String(r.ownN).padStart(3)} ` +
      `absent | flattened ${String(r.n).padStart(3)} = ${String(r.op).padStart(3)} oper + ` +
      `${String(r.at).padStart(3)} attr + ${String(r.eh).padStart(3)} handler` +
      (r.other ? ` + ${r.other} other` : ""));
if (perIface.length > TOP)
  say(`  … ${perIface.length - TOP} further row(s) NOT PRINTED. This is a cut by RANK and the sort is ` +
      `own-operation-first, so a row below the cut may still carry every absent attribute of its interface.`);
