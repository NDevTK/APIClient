/* THE FIXTURE'S OWN REACHABILITY LADDER, READ OFF A RUN'S CAPTURED OUTPUT.
 *
 * engine/host/test_forced.c prints a stream marker at the head of every selftest row it answers, and a marker
 * with no reader reads zero for ever — CLAUDE.md §MEASURE-WHAT-THE-SHIPPED-PATH-WRITES states the pair this
 * closes: an ABSENT count and a ZERO count are DIFFERENT FACTS and must never be averaged. A row whose numbers
 * are asserted in C is oracled for "the value is wrong" and is silent about "the function never ran", because
 * §AN-ABSENT-CRASH-IS-NOT-A-CORRECT-VALUE holds of a CHECK exactly as it holds of an abort: a check that never
 * executes is indistinguishable from one that passed. The marker's PRESENCE is the reachability witness for
 * every assertion beneath it, and §THE-SAME-HOLE-SWALLOWS-A-PREDICTION says a witness is what turns "no abort
 * fired" from a reassurance into a scored result. This reader is the thing that consumes those witnesses.
 *
 * WHAT IT ASSERTS AND WHAT IT REFUSES TO ASSERT. It does NOT assert that every marker appears: a run legitimately
 * stops early — at its CPU budget, at an abort, or because the host drove a shorter document — and an invariant
 * requiring every rung would fire on every correct refusal, which is §AN-INVARIANT-OVER-A-GATED-OPERATION's
 * defect built into the instrument. What it asserts is the two things a short run cannot produce:
 *   A HOLE — a rung absent while a LATER rung is present. main() calls these in one fixed order, so a later
 *     answer with an earlier one missing is not a short run, it is a row that did not answer.
 *   A PARTIAL rung — a function that printed SOME of its own markers and not the rest. That is the strongest
 *     reading a stream can carry: the function was entered and did not finish.
 * Everything else is REPORTED: the reach, the rungs beyond it, and the conditional markers. A short run is a
 * fact about the run and this prints it rather than grading it.
 *
 * THE LADDER IS DERIVED AND NOT LISTED, per §AN-AUDITOR-DERIVES-THE-RULE. The order comes from main()'s own
 * call order in the producer's bytes; each rung's required markers come from that function's own `printf`s at
 * the top level of its body. Nothing here types a rung, so a selftest added over there joins the ladder on the
 * first run after it lands. What IS spelled is the ROSTER of marker names, and it is spelled for one reason
 * only: engine/fieldgate.mjs reads a marker's readers by the NAME appearing in a scanned file, and it has no
 * derived-reader channel for the marker namespace as it has for field names — so a purely derived reader would
 * leave every marker in that gate's WRITTEN-with-no-reader band, which is the chronic red CLAUDE.md
 * §A-VERDICT-THAT-IS-RED-ON-EVERY-RUN says nobody reads the body of. The roster is NOT the banned second copy:
 * §AN-AUDITOR-DERIVES-THE-RULE's objection is that a restated rule DRIFTS, and this one is asserted equal to
 * the derivation on every invocation and names what to add or drop when it is not. Behaviour is decided by the
 * derivation alone. RETIREMENT: the roster goes when that gate credits a marker read to a consumer that derives
 * its set from the producer, because the spelling is then buying nothing.
 *
 * WHAT DECIDES A RUNG'S CONDITIONALITY, IN TWO PLACES AND FOR TWO REASONS.
 *   THE CALL: a call reached through `if`, `return`, a loop or a ternary is CONDITIONAL and never required —
 *     the ABI arm of this fixture is `if (arg_has(argc, argv, "--abi")) return abi_main(...)`, and requiring its
 *     marker would refuse every run of the other arm.
 *   THE EMISSION: a `printf` below the top level of its own function body is CONDITIONAL and never required. A
 *     marker printed only when some row reads 0 is a line that appears on the bad day, and the producer says so
 *     of that row in its own words; demanding it would be the same gated-operation defect one level down.
 * Both are read off brace structure rather than asserted here, so neither is a judgement anybody has to keep.
 *
 * THE RESOLUTION LIMIT, STATED BECAUSE A LADDER READS AS FINER THAN IT IS. Several rungs share one marker, so
 * that marker's presence cannot say which of them answered. A rung whose required markers are all required by
 * an EARLIER rung is INDISTINGUISHABLE and takes no part in the reach or the hole test; it is printed as such.
 * The ladder resolves at the granularity of the marker namespace and not of the call.
 *
 * WHAT A ZERO FROM THIS READER DOES NOT MEAN. The observation channel is a marker at the START of a line,
 * because a build log quotes marker names in prose — this project's own record-field audit prints them in its
 * findings — and counting those would be §THE-REFUTATION-IS-INSIDE-THE-TEXT-YOU-ARE-ABOUT-TO-QUOTE arriving in
 * an instrument. So a run whose stream was captured through something that reformats lines reads as having
 * reached nothing, and the reach is then a fact about the capture. Every log is reported by PATH and per file,
 * never summed, for §AND-A-SCRATCH-DIRECTORY-HOLDS-RUNS-OF-SEVERAL-SUBJECTS' reason: a directory holds runs of
 * several subjects and a value quoted out of the directory belongs to a document nobody is discussing.
 *
 * IT REFUSES RATHER THAN MEASURING A SUBSET. A producer whose main() cannot be delimited, a ladder with no
 * rungs, a marker whose enclosing function cannot be named, a roster that disagrees with the derivation: each
 * throws, because a reader that silently answered about part of the producer would report a shorter ladder and
 * a shorter ladder reads as a cleaner one.
 *
 * AND A RUNG'S ABSENCE HAS TWO READINGS THAT NO LOG CAN SEPARATE, so the verdict is SCORED ONLY AGAINST AN
 * ARTIFACT. A rung absent while later rungs answer is `the row did not answer`, and it is equally `the binary
 * that wrote this log was built before that row existed` — CLAUDE.md
 * §AND-THE-IDENTITY-IS-CHECKED-PER-COUNTER's pair, where a build with no such counter emits nothing and a run
 * today reads zero, and scoring the second as the first refutes a row whose subject did not exist. `--artifact`
 * asks the built bytes: a rung whose markers are not in them is NOT COMPILED, excluded from the reach and from
 * the hole test, and printed. WITHOUT one the reader prints the same observations and REFUSES TO SCORE them,
 * because an unscored reading is a real result and a wrong verdict about a hypothesis is not. The artifact probe
 * is itself controlled — at least one roster marker must be FOUND and an invented one must be ABSENT, or the
 * probe is refused rather than answering that every rung is uncompiled, which is the direction that would read
 * as a clean bill. RETIREMENT: the unscored arm goes when a run's own stream states the revision it was built
 * at, because the rung set is then derivable from the log without a second file.
 *
 * Usage: node engine/fixturereach.mjs [--artifact <built-file>] <captured-run-log>...
 *        node engine/fixturereach.mjs --ladder          (the derivation alone, no run)
 * Exit: 0 nothing scored against it; 1 a hole or a partial rung, scored against an artifact; 2 refused. */

import { readFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const PRODUCER = join(ROOT, "engine", "host", "test_forced.c");

/* THE MARKER NAMESPACE THIS PRODUCER PRINTS. Asserted equal to the derivation below; see the header for why it
   is spelled at all and for the condition that retires it.
   NAMED RESIDUAL — NOTHING IN THIS TREE INVOKES THIS READER, so the equality below is checked only when
   somebody runs it by hand, and a roster this file does not run is a SPELLING rather than a reader.
   NOT COVERED: a marker landed in the producer leaves the roster short, and the only thing that fires is a
   refusal inside a tool no stage calls.
   WHAT THE NEXT DIFF BUILDS: this reader on engine/build.mjs's own stage list, with exit 2 read as a failure
   rather than as silence. `grep -c fixturereach engine/build.mjs` answers 0 at origin/main, which is why this
   is outstanding rather than already met.
   HOW ITS ABSENCE SHOWS: every invocation of this file exits 2 naming the markers to ADD, for as long as
   nobody invokes it — while engine/fieldgate.mjs goes on crediting each roster name as a READ, because for the
   marker namespace a name in a scanned file IS the construct. So that gate's WRITTEN-with-no-reader band reads
   clean over a reader that refuses to run at all, which is a state this roster has already been in. */
const ROSTER = [
  "@A2ENTER", "@A2OK", "@A2REALM", "@CANVAS2D", "@COLDPARK", "@COLDRESUME", "@FACE", "@FLEX", "@GCOMP",
  "@GLYF", "@GLYPHMARK", "@H", "@HGATED", "@HUNASKED", "@HWORK", "@IMGMARK", "@INLINEBOX", "@INLINEBREAK",
  "@LOGICALWM", "@OCENSUS", "@PAGEERR", "@PAGEERR-EXPLORED", "@PAGEERR-RETRACTED", "@PAGEERR-STAGED",
  "@PAGEERR-STAGED-TOKEN", "@PAINT", "@PAINTTEXT", "@RASTER", "@RESULT", "@S", "@SCENSUS",
];

/* A REFUSAL EXITS 2 AND A FINDING EXITS 1, because a status that merged them would be the one shape this
   reader exists to undo: `I found a defect` and `I cannot decide this` take opposite work, and a caller reading
   one number for both learns neither. An uncaught throw exits 1 in node, so the exit is taken here rather than
   left to the runtime. RETIREMENT: this goes when nothing in this file can refuse. */
class Refused extends Error {}
const refuse = (why) => { throw new Refused("[fixturereach] " + why); };
process.on("uncaughtException", (e) => {
  process.stderr.write(String(e instanceof Refused ? e.message : (e && e.stack) || e) + "\n");
  process.exit(e instanceof Refused ? 2 : 3);
});

/* ---- the producer's bytes, with comments and literal interiors blanked ------------------------------------ */

/* A MASK AND NOT A STRIP, so every offset is an offset into the real source and a line number needs no
   translation. A marker lives INSIDE a literal, so the literal's delimiters survive and its interior does not:
   the emission is found in the raw text and confirmed against the mask, which is what tells a `printf("@X"` in
   code from the same characters inside a comment. */
function maskOf(src) {
  const m = src.split("");
  let i = 0;
  while (i < src.length) {
    if (src[i] === "/" && src[i + 1] === "*") {
      let e = src.indexOf("*/", i + 2);
      e = e < 0 ? src.length : e + 2;
      for (let k = i; k < e; k++) if (m[k] !== "\n") m[k] = " ";
      i = e;
      continue;
    }
    if (src[i] === "/" && src[i + 1] === "/") {
      let e = src.indexOf("\n", i);
      e = e < 0 ? src.length : e;
      for (let k = i; k < e; k++) m[k] = " ";
      i = e;
      continue;
    }
    if (src[i] === '"' || src[i] === "'") {
      const q = src[i];
      let k = i + 1;
      while (k < src.length) {
        if (src[k] === "\\") { k += 2; continue; }
        if (src[k] === q) break;
        k++;
      }
      for (let j = i + 1; j < Math.min(k, src.length); j++) if (m[j] !== "\n") m[j] = " ";
      i = Math.min(k + 1, src.length);
      continue;
    }
    i++;
  }
  return m.join("");
}

const lineOf = (src, off) => {
  let n = 1;
  for (let i = 0; i < off && i < src.length; i++) if (src[i] === "\n") n++;
  return n;
};

/* Every brace pair whose OPEN sits at file scope is a function body; nothing else in C is. */
function topLevelBodies(mask) {
  const out = [];
  let depth = 0;
  for (let k = 0; k < mask.length; k++) {
    if (mask[k] === "{") { if (depth === 0) out.push({ open: k, close: -1 }); depth++; }
    else if (mask[k] === "}") { depth--; if (depth === 0 && out.length) out[out.length - 1].close = k; }
  }
  return out;
}

/* THE DECLARATOR'S OWN IDENTIFIER, WALKED BACK FROM THE BODY'S BRACE — never the first identifier in the
   preceding text, which is the return type, a parameter type, or a macro. A body whose declarator does not end
   in a balanced parameter list is not a function and is skipped rather than guessed at. */
function declaratorName(mask, open) {
  let k = open - 1;
  while (k >= 0 && /\s/.test(mask[k])) k--;
  if (mask[k] !== ")") return null;
  let depth = 0;
  for (; k >= 0; k--) {
    if (mask[k] === ")") depth++;
    else if (mask[k] === "(") { depth--; if (depth === 0) break; }
  }
  if (k < 0) return null;
  let e = k - 1;
  while (e >= 0 && /\s/.test(mask[e])) e--;
  let s = e;
  while (s >= 0 && /[A-Za-z0-9_]/.test(mask[s])) s--;
  const name = mask.slice(s + 1, e + 1);
  return /^[A-Za-z_]\w*$/.test(name) ? name : null;
}

/* ---- the derivation ------------------------------------------------------------------------------------- */

const CONTROL = /\b(if|else|while|for|switch|case|do|return|goto)\b|&&|\|\||\?/;

/* Is the text from the previous statement boundary up to `at` free of anything that can decide whether the
   statement runs? A declaration with an initializer is not a decision; `if (…) return f()` is two of them. */
const unconditionalHere = (mask, from, at) => {
  let s = at - 1;
  while (s >= from && mask[s] !== ";" && mask[s] !== "{" && mask[s] !== "}") s--;
  return !CONTROL.test(mask.slice(s + 1, at));
};

/* AND THE SAME QUESTION ASKED OF EVERY BLOCK BETWEEN THE CALL AND THE BODY, because a bare `{ … }` groups and
   an `if (…) { … }` decides, and the call itself cannot tell them apart. Walking outward is what makes the
   answer about the statement's whole path rather than about its last line. */
function unconditionalIn(mask, body, at) {
  let cursor = at;
  for (;;) {
    let depth = 0, open = -1;
    for (let k = cursor - 1; k > body.open; k--) {
      if (mask[k] === "}") depth++;
      else if (mask[k] === "{") { if (depth === 0) { open = k; break; } depth--; }
    }
    if (!unconditionalHere(mask, open < 0 ? body.open + 1 : open + 1, cursor)) return false;
    if (open < 0) return true;
    cursor = open;
  }
}

const EMIT = /\b(printf|puts|fputs)\s*\(\s*"@([A-Z][A-Z0-9_]*(?:-[A-Z0-9_]+)*)/g;

function derive() {
  const src = readFileSync(PRODUCER, "utf8");
  const mask = maskOf(src);
  const bodies = topLevelBodies(mask);
  for (const b of bodies) b.name = declaratorName(mask, b.open);
  const main = bodies.find((b) => b.name === "main" && b.close > b.open);
  if (!main) refuse(`${relative(ROOT, PRODUCER)} has no delimitable main() body — the ladder's order comes from ` +
                    `that function's call order and there is nothing to read it out of`);

  /* Every emission, with the function it belongs to and its depth inside that function's body. */
  const sites = [];
  let m;
  EMIT.lastIndex = 0;
  while ((m = EMIT.exec(src))) {
    const q = src.indexOf('"', m.index);
    if (mask[q] !== '"') continue;                 /* the same characters inside a comment */
    const body = bodies.find((b) => b.open < m.index && m.index < b.close);
    if (!body || !body.name)
      refuse(`an emission of @${m[2]} at ${relative(ROOT, PRODUCER)}:${lineOf(src, m.index)} sits in no ` +
             `function this can name, so which rung it belongs to is undecidable — a ladder built past it ` +
             `would be short and a short ladder reads as a clean one`);
    let depth = 0;
    for (let k = body.open + 1; k < m.index; k++) {
      if (mask[k] === "{") depth++;
      else if (mask[k] === "}") depth--;
    }
    sites.push({ tag: "@" + m[2], fn: body.name, depth, line: lineOf(src, m.index) });
  }

  /* A LITERAL THIS PRODUCER COMPILES AND NEVER PRINTS — the longest assert message naming a roster marker,
     which is exactly the population fieldgate's own marker comment records as the majority of its write
     records and as verdict-irrelevant there. Here it is the one thing that is load-bearing about them. */
  let witness = null;
  const LIT = /"(?:[^"\\\n]|\\.)*"/g;
  let q;
  LIT.lastIndex = 0;
  while ((q = LIT.exec(src))) {
    if (mask[q.index] !== '"') continue;
    const body = q[0].slice(1, -1);
    if (!/@[A-Z][A-Z0-9_]/.test(body)) continue;
    if (/^@[A-Z][A-Z0-9_]*(-[A-Z0-9_]+)*([ \\{]|$)/.test(body)) continue;   /* the head of an emission */
    if (/[^ -~]/.test(body) || body.includes("\\")) continue;               /* one byte run, no escapes */
    if (witness === null || body.length > witness.length) witness = body;
  }

  const derived = [...new Set(sites.map((s) => s.tag))].sort();
  const declared = [...ROSTER].sort();
  const missing = derived.filter((t) => !declared.includes(t));
  const stale = declared.filter((t) => !derived.includes(t));
  if (missing.length || stale.length)
    refuse(`the spelled roster and the derivation disagree — ADD ${missing.join(" ") || "nothing"}; DROP ` +
           `${stale.join(" ") || "nothing"}. The derivation decides behaviour and the roster exists only so ` +
           `engine/fieldgate.mjs can see a reader for each name; a roster that drifts is a marker whose reader ` +
           `that gate cannot credit, which is the band this file was written to empty`);

  /* The rungs, in main()'s own call order. */
  const byFn = new Map();
  for (const s of sites) {
    if (s.fn === "main") continue;              /* the ladder's container is not one of its rungs */
    if (!byFn.has(s.fn)) byFn.set(s.fn, []);
    byFn.get(s.fn).push(s);
  }
  const rungs = [], offLadder = [];
  const mainText = mask.slice(0, main.close);
  for (const [fn, ss] of byFn) {
    const required = [...new Set(ss.filter((s) => s.depth === 0).map((s) => s.tag))].sort();
    const conditional = [...new Set(ss.filter((s) => s.depth > 0).map((s) => s.tag))].sort();
    const call = new RegExp(String.raw`\b${fn}\s*\(`, "g");
    call.lastIndex = main.open;
    let at = -1, gated = false;
    let c;
    while ((c = call.exec(mainText))) {
      if (unconditionalIn(mask, main, c.index)) { at = c.index; gated = false; break; }
      if (at < 0) { at = c.index; gated = true; }
    }
    const row = { fn, required, conditional, line: ss[0].line };
    if (at < 0) offLadder.push({ ...row, why: "no call in main()" });
    else if (gated) offLadder.push({ ...row, why: "every call in main() is reached through a decision" });
    else rungs.push({ ...row, at });
  }
  rungs.sort((a, b) => a.at - b.at);
  if (!rungs.length) refuse("no rung — every marker-emitting function is off the ladder, which means the call " +
                            "order was not read rather than that the producer has no rows");

  /* A rung adds nothing the ladder can resolve when an earlier rung already requires all of its markers. */
  const seen = new Set();
  for (const r of rungs) {
    r.resolves = r.required.some((t) => !seen.has(t));
    for (const t of r.required) seen.add(t);
  }
  return { src, rungs, offLadder, derived, witness };
}

/* ---- a run's captured output ----------------------------------------------------------------------------- */

const ABORT = /^@(?:WHY|E) (.*)$/m;

function readRun(path, derived) {
  const text = readFileSync(path, "utf8");
  const present = new Set();
  for (const line of text.split("\n")) {
    const t = /^(@[A-Z][A-Z0-9_]*(?:-[A-Z0-9_]+)*)\b/.exec(line);
    if (t && derived.includes(t[1])) present.add(t[1]);
  }
  const ab = ABORT.exec(text);
  let at = null;
  if (ab) {
    const j = /"at"\s*:\s*"([^"]*)"/.exec(ab[1]);
    at = j ? j[1] : ab[1].slice(0, 160);
  }
  /* ZERO MARKERS AND NO ABORT IS NOT A REACH OF ZERO — IT IS A FILE THIS PROBE CANNOT READ, and the two take
     opposite work. `compiledMarkers` already refuses an ARTIFACT that holds no marker of the roster, in those
     words and for this reason, and that refusal lives inside the artifact path, so it has never applied to a RUN
     LOG at any input: a log of another subject, an empty file, or one truncated before the fixture printed a
     byte was scored `REACH 0/N` with every rung listed as beyond the reach. That is the accusing direction, and
     it is the worst available for this file in particular, whose whole job is to say which rows a run could not
     answer for — so an unreadable input rendered identically to a total failure of the engine.
     THE DISCRIMINATOR IS THE RUN'S OWN ABORT LINE, AND IT IS TWO-SIDED: a run that really died before its first
     marker printed `@WHY` or `@E`, so zero markers WITH an abort is a genuine zero and is still scored; zero
     markers WITHOUT one is a file carrying no evidence either way.
     MEASURED BY ARMING THE CONTROL RATHER THAN BY READING THE CODE: a one-line file holding no marker scored
     `REACH 0/15 resolving rung(s)` with `no @WHY/@E line in this run` and exited 0, listing all fifteen rungs as
     beyond the reach — so the refusal this file documents for an artifact was unreachable for a log.
     RESIDUAL: a log holding SOME markers, no abort, and an unreached tail is either TRUNCATED or COMPLETE, and
     this cannot tell those apart. The next diff derives the producer's own terminal marker — the last
     unconditional rung's — and reports a run lacking it as truncated rather than as a reach. Its absence shows
     as a REACH printed for a run whose writer was killed, indistinguishable in this output from a run that
     reached the end and answered nothing further. */
  if (!present.size && at === null)
    refuse(`no marker of the roster occurs in ${path} and it carries no @WHY/@E line — so this is not a run of ` +
           `${relative(ROOT, PRODUCER)} that reached nothing, it is a file this probe cannot read, and scoring ` +
           `it would print REACH 0/N with every rung beyond the reach`);
  return { present, at };
}

/* ---- which rungs the built bytes can answer for at all ---------------------------------------------------- */

/* THE MARKER'S OWN TEXT IN THE ARTIFACT, which is the only thing that separates a row that did not answer from
   a row the binary never held. Read as bytes and searched as bytes: a wasm module is not text and decoding it
   would refuse whole artifacts for holding one invalid sequence. */
function compiledMarkers(path, derived, witness) {
  const hay = readFileSync(path).toString("latin1");
  /* THE ONE MISUSE THAT DISARMS THIS CHECK IN THE FLATTERING DIRECTION IS HANDING IT A RUN LOG. A log holds
     every marker the run printed, so the compiled set would equal the observed set, every absent rung would be
     excluded as uncompiled, and NOTHING would ever be scored — a silent pass reached by asking the log about
     itself. So the file must also hold a string the producer COMPILES AND DOES NOT PRINT, derived below from an
     assert message rather than spelled here. A run whose output does hold that string is a run that fired that
     assert, and refusing it is right for the same reason: the reading is then about the abort.
     RETIREMENT: this goes when the artifact states its own producer and revision in a form this can read, so
     `is this a build of that file` stops being answered by a witness string at all. */
  if (witness !== null && !hay.includes(witness))
    refuse(`${path} does not hold ${JSON.stringify(witness.slice(0, 60))}, which the producer compiles and ` +
           `never prints — so this file is not a build of ${relative(ROOT, PRODUCER)}, and a run LOG handed ` +
           `here would answer that every absent rung was uncompiled and score nothing at all`);
  const found = derived.filter((t) => hay.includes(t));
  if (!found.length)
    refuse(`no marker of the roster occurs in ${path} — this probe cannot read that artifact, and answering ` +
           `that every rung is uncompiled would exclude the whole ladder and read as a clean bill`);
  if (hay.includes("@ZZ-FIXTUREREACH-CONTROL-NEVER-PRINTED"))
    refuse(`${path} contains this probe's own negative control, so a hit in it is not evidence that a marker ` +
           `was compiled in`);
  return new Set(found);
}

/* ---- report ---------------------------------------------------------------------------------------------- */

const { rungs, offLadder, derived, witness } = derive();
const argv = process.argv.slice(2);
const say = (s) => process.stdout.write("[fixturereach] " + s + "\n");
const args = [], artifacts = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === "--artifact") {
    i++;
    if (i >= argv.length) refuse("--artifact with no path after it");
    artifacts.push(argv[i]);
  } else args.push(argv[i]);
}
const compiled = artifacts.length
  ? artifacts.map((p) => compiledMarkers(p, derived, witness)).reduce((a, b) => new Set([...a, ...b]))
  : null;

say(`producer ${relative(ROOT, PRODUCER)} — ${rungs.length} rung(s) in main()'s call order, ` +
    `${rungs.filter((r) => r.resolves).length} of them resolving a marker no earlier rung requires, ` +
    `${offLadder.length} marker-emitting function(s) off the ladder, ${derived.length} marker(s) in the roster`);

if (args.includes("--ladder") || !args.length) {
  for (const [i, r] of rungs.entries())
    say(`  ${String(i + 1).padStart(3)}. ${r.fn.padEnd(38)} requires [${r.required.join(" ")}]` +
        `${r.conditional.length ? ` conditional [${r.conditional.join(" ")}]` : ""}` +
        `${r.resolves ? "" : "  INDISTINGUISHABLE — an earlier rung requires every marker of it"}`);
  for (const o of offLadder)
    say(`  OFF  ${o.fn.padEnd(38)} ${o.why}; requires [${o.required.join(" ")}]` +
        `${o.conditional.length ? ` conditional [${o.conditional.join(" ")}]` : ""}`);
  if (!args.length) {
    say("no run given — the ladder above is the derivation and says nothing about any run. " +
        "Pass a captured run log to read one.");
    process.exit(0);
  }
}

let scored = 0;
for (const path of args.filter((a) => a !== "--ladder")) {
  const { present, at } = readRun(path, derived);
  /* A RUNG THE ARTIFACT DOES NOT HOLD IS NOT A RUNG THIS LOG COULD HAVE ANSWERED. Excluded and printed, never
     counted against the run — and where there is no artifact nothing is excluded and nothing is scored. */
  const uncompiled = compiled ? rungs.filter((r) => r.resolves && !r.required.some((t) => compiled.has(t))) : [];
  const resolving = rungs.filter((r) => r.resolves && !uncompiled.includes(r));
  const state = resolving.map((r) => {
    const hit = r.required.filter((t) => present.has(t));
    return { r, hit, all: hit.length === r.required.length, none: hit.length === 0 };
  });
  let reach = 0;
  while (reach < state.length && state[reach].all) reach++;
  const partial = state.filter((s) => !s.all && !s.none);
  const lastPresent = state.reduce((acc, s, i) => (s.hit.length ? i : acc), -1);
  const holes = state.slice(0, lastPresent + 1).filter((s) => s.none);

  say(`── ${path} ──`);
  if (uncompiled.length)
    say(`  NOT COMPILED into ${artifacts.join(", ")}, so this log could not have carried them: ` +
        uncompiled.map((r) => `${r.fn} [${r.required.join(" ")}]`).join(", "));
  say(`  REACH ${reach}/${state.length} resolving rung(s)` +
      (reach < state.length ? ` — the first unreached is ${state[reach].r.fn} [${state[reach].r.required.join(" ")}]` : "") +
      (at ? `; this run carries an abort at ${at}` : "; no @WHY/@E line in this run"));
  /* A SHORT RUN IS PRINTED AND NOT GRADED: see the header. What follows is the two things a short run cannot
     produce, and only those set the exit status. */
  for (const s of partial) {
    scored++;
    say(`  PARTIAL  ${s.r.fn} printed [${s.hit.join(" ")}] and not ` +
        `[${s.r.required.filter((t) => !present.has(t)).join(" ")}] — the function was ENTERED and did not ` +
        `finish, which no budget and no shorter document produces. ${at ? `The abort's own \`at\` is ${at}, ` +
        `and whether that file:line is inside ${s.r.fn} is what separates this function from the channel` :
        "There is no abort line in this run, so the channel and the block are not separated here"}`);
  }
  for (const s of holes) {
    scored++;
    say(`  HOLE     ${s.r.fn} printed none of [${s.r.required.join(" ")}] while a LATER rung answered — ` +
        `main() calls these in one order, so this is a row that did not answer rather than a run that stopped`);
  }
  const beyond = state.slice(reach).filter((s) => !partial.includes(s) && !holes.includes(s));
  if (beyond.length)
    say(`  beyond the reach, neither partial nor holed: ${beyond.map((s) => s.r.fn).join(", ")}`);
  const cond = rungs.flatMap((r) => r.conditional).filter((t) => present.has(t));
  say(`  conditional markers this run printed: ${cond.length ? [...new Set(cond)].join(" ") : "none"}`);
  if (!compiled && (partial.length || holes.length)) {
    scored -= partial.length + holes.length;
    say(`  UNSCORED — every PARTIAL and HOLE above is equally a rung the binary that wrote this log never ` +
        `held, and no log says which binary that was. Pass --artifact <built-file> to separate them; until ` +
        `then these are observations and not findings, and this reader does not grade them`);
  }
}

process.exit(scored > 0 ? 1 : 0);
