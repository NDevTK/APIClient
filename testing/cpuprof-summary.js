// Summarize a Chromium .cpuprofile — top self-time leaves with location.
//
// WHAT THIS FILE READS, AND WHO WROTE IT. Nothing in this tree writes a .cpuprofile: the input is always
// produced OUT OF BAND, by `node --cpu-prof`, by DevTools' "Save profile", or by saving a CDP `Profiler.stop`
// reply. So this file cannot name a producer in this repository — what it CAN name, and now does, is the
// FORMAT it accepts, which is the Chrome DevTools Protocol type `Profiler.Profile` (protocol 1.3). That is
// the whole point of the shape check below: a summarizer whose input arrives by hand from three different
// tools is exactly the consumer that must state its contract, because there is no producer to read it off.
//
// THE CONTRACT IS DERIVED, NEVER RESTATED. `assertShape` walks the PUBLISHED protocol JSON — the same
// `js_protocol.json` Chromium generates — so the required/optional split, the field names and the field types
// are read from the authority that owns them rather than from a list somebody typed here. Adding a field to
// the protocol therefore cannot leave this checker describing a shape that no longer exists.
//
// WHY THERE ARE NO `||` DEFAULTS LEFT. A default over a field the producer never writes turns an ABSENT
// measurement into a plausible datum, and this is a profiler: a plausible datum here is a performance number
// somebody acts on. Every read below is one of two things and never a third — an ASSERTED required field, or
// a POSITIVE statement about an optional one whose absence the protocol gives a meaning ("no samples were on
// top of the stack", "this node has no children"). The interval in particular used to fall back to a hardcoded
// 1000µs, which silently minted every millisecond column in the report out of a constant the input file never
// stated; it is now derived from the file's own required `startTime`/`endTime` and the banner says which
// source it came from.
//
// ASSERTION MECHANISM: a local always-fatal CHECK, in check.h's vocabulary and with its semantics — this is a
// data-integrity invariant about an input measurement, which check.h names as an always-fatal case, and there
// is no dev/release split in a CLI tool. `extension/check.js` is deliberately NOT loaded: it is an IIFE over
// `self` (undefined in a Node CJS realm), and `engine/level1.mjs` states this project's reason for keeping it
// out of a driver realm — "a driver silently gaining assert machinery is a difference between what it runs
// and what production runs."
const fs = require("fs");

function CHECK(cond, msg) { if (!cond) { console.error("@E CHECK failed: " + msg); process.exit(1); } }
function CHECK_FAIL(msg) { CHECK(false, msg); }

// ─── THE AUTHORITY ───────────────────────────────────────────────────────────────────────────────────────
// `devtools-protocol` is installed as puppeteer's own exact pin (puppeteer is a devDependency here); it is
// NOT declared in this project's package.json, so name it in the crash rather than degrade to a hand-written
// field list — a checker that cannot reach its authority must refuse, not guess.
let PROTOCOL;
try {
  PROTOCOL = require("devtools-protocol/json/js_protocol.json");
} catch (e) {
  CHECK_FAIL("cannot load `devtools-protocol/json/js_protocol.json`, which is the machine-readable authority " +
             "for the `Profiler.Profile` shape this file reads. It ships as puppeteer's exact pin " +
             "(devDependency `puppeteer`); run `npm install`. Underlying: " + e.message);
}

// "Domain.Type" → the published type declaration. Built from the protocol's own domain list, so the set of
// known types is whatever the installed protocol defines.
const PROTOCOL_TYPES = new Map();
// A domain that declares no `types` declares none — the positive read, not a `|| []` hole, by the same rule
// this file applies to the profiles it reads.
for (const d of PROTOCOL.domains)
  for (const t of (Object.hasOwn(d, "types") ? d.types : [])) PROTOCOL_TYPES.set(d.domain + "." + t.id, t);

// Walk a value against a published type declaration. Every message names the FULL PATH to the offending
// field and the protocol type that demanded it, so a wrong-shaped file is diagnosed at the door.
function assertShape(value, decl, owner, path) {
  if (decl.$ref) {
    const key = decl.$ref.includes(".") ? decl.$ref : owner.slice(0, owner.indexOf(".")) + "." + decl.$ref;
    const target = PROTOCOL_TYPES.get(key);
    CHECK(target, "protocol type `" + key + "` referenced at `" + path + "` is not defined by the installed " +
                  "devtools-protocol " + PROTOCOL.version.major + "." + PROTOCOL.version.minor);
    return assertShape(value, target, key, path);
  }
  const what = " (protocol " + owner + ")";
  switch (decl.type) {
    case "object": {
      CHECK(value !== null && typeof value === "object" && !Array.isArray(value),
            "`" + path + "` must be an object" + what + ", got " + describe(value));
      // An object type with no declared `properties` is an OPEN object: the protocol states nothing about its
      // members, so neither does this walk. That is a positive statement about the declaration, not a default.
      for (const p of (Object.hasOwn(decl, "properties") ? decl.properties : [])) {
        if (!Object.hasOwn(value, p.name)) {
          CHECK(p.optional === true,
                "`" + path + "." + p.name + "` is REQUIRED" + what + " and is absent. This file does not " +
                "default an absent required field — a default here would render as a real measurement.");
          continue;
        }
        assertShape(value[p.name], p, owner + "." + p.name, path + "." + p.name);
      }
      return;
    }
    case "array": {
      CHECK(Array.isArray(value), "`" + path + "` must be an array" + what + ", got " + describe(value));
      for (let i = 0; i < value.length; i++) assertShape(value[i], decl.items, owner, path + "[" + i + "]");
      return;
    }
    case "integer":
      return CHECK(Number.isInteger(value),
                   "`" + path + "` must be an integer" + what + ", got " + describe(value));
    case "number":
      return CHECK(typeof value === "number" && Number.isFinite(value),
                   "`" + path + "` must be a finite number" + what + ", got " + describe(value));
    case "string":
      return CHECK(typeof value === "string",
                   "`" + path + "` must be a string" + what + ", got " + describe(value));
    case "boolean":
      return CHECK(typeof value === "boolean",
                   "`" + path + "` must be a boolean" + what + ", got " + describe(value));
    default:
      return CHECK_FAIL("this checker has no rule for protocol type kind `" + decl.type + "` at `" + path +
                        "` — the installed protocol uses a construct this walk was not written for, so it " +
                        "cannot claim the shape is right. Extend `assertShape`.");
  }
}
function describe(v) {
  if (v === null) return "null";
  if (Array.isArray(v)) return "an array of " + v.length;
  if (typeof v === "object") return "an object with keys [" + Object.keys(v).slice(0, 6).join(",") + "]";
  return typeof v + " " + JSON.stringify(v);
}

// ─── ARGUMENTS ───────────────────────────────────────────────────────────────────────────────────────────
const file = process.argv[2];
if (!file) { console.error("usage: node cpuprof-summary.js <file.cpuprofile> [topN]"); process.exit(1); }
// topN is a genuinely optional ARGUMENT, so absence is a positive statement (use 30) — but a value that is
// PRESENT and unparseable is a typo, and `Number("abc") || 30` silently reported a different top-N than asked.
let topN = 30;
if (process.argv[3] !== undefined) {
  topN = Number(process.argv[3]);
  CHECK(Number.isInteger(topN) && topN > 0,
        "topN argument `" + process.argv[3] + "` is not a positive integer");
}

// ─── THE DOOR ────────────────────────────────────────────────────────────────────────────────────────────
const profile = JSON.parse(fs.readFileSync(file, "utf8"));
// The one wrong shape this file's provenance actually produces: a saved CDP `Profiler.stop` REPLY, which
// wraps the profile one construct further in. Name it, rather than let the generic walk report a missing
// `nodes` on a file that in fact contains a perfectly good profile.
if (!Object.hasOwn(profile, "nodes") && Object.hasOwn(profile, "profile"))
  CHECK_FAIL("`" + file + "` looks like a saved CDP `Profiler.stop` REPLY, whose profile is nested one level " +
             "in as `.profile`. Pass the inner object (`jq .profile`), not the reply envelope.");
assertShape(profile, { $ref: "Profiler.Profile" }, "Profiler.Profile", "profile");

const nodes = profile.nodes;
CHECK(profile.endTime >= profile.startTime,
      "profile.endTime (" + profile.endTime + ") precedes profile.startTime (" + profile.startTime + "); " +
      "every duration in this report is derived from that span");

// `ProfileNode.hitCount` and `.children` are OPTIONAL in the protocol, and the protocol states what their
// absence MEANS — hitCount is "Number of samples where this node was on top of the call stack" (absent: none
// were), children is "Child node ids" (absent: it has none). Both absences are read here as those positive
// statements, in one place each, rather than as a `||` hole at four separate reads.
const selfHits = n => (Object.hasOwn(n, "hitCount") ? n.hitCount : 0);
const childIds = n => (Object.hasOwn(n, "children") ? n.children : []);

const totalSamples = nodes.reduce((s, n) => s + selfHits(n), 0);

// The sample interval, and WHERE IT CAME FROM. `timeDeltas` is optional; when it is absent the interval is
// derived from `startTime`/`endTime`, which the protocol makes REQUIRED — the same quantity from stated
// facts, never a hardcoded constant standing in for a measurement the file does not contain. The denominator
// for that derivation is the sample count `samples` STATES when it is present: measured on a real
// `node --cpu-prof` profile, Σ hitCount (235) and samples.length (212) DISAGREE, so the two are not
// interchangeable and neither may be asserted equal to the other.
let sampleIntervalUs, intervalSource;
if (Object.hasOwn(profile, "timeDeltas") && profile.timeDeltas.length > 0) {
  sampleIntervalUs = Math.round(profile.timeDeltas.reduce((s, d) => s + d, 0) / profile.timeDeltas.length);
  intervalSource = "mean of " + profile.timeDeltas.length + " timeDeltas";
} else {
  const n = Object.hasOwn(profile, "samples") ? profile.samples.length : totalSamples;
  const from = Object.hasOwn(profile, "samples") ? "samples.length" : "Σ hitCount";
  if (n > 0) {
    sampleIntervalUs = Math.round((profile.endTime - profile.startTime) / n);
    intervalSource = "startTime/endTime span ÷ " + n + " (" + from + ")";
  } else {
    sampleIntervalUs = 0;
    intervalSource = "unmeasurable — this profile contains no samples";
  }
}
const intervalMs = sampleIntervalUs / 1000;
// `span` is a STATED fact (two required fields subtracted), so it is printed instead of the sample-count
// estimate that used to stand here: `Σ hitCount × interval` multiplied two quantities that do not correspond
// (312ms against a stated 282ms span on the profile above) and read as the profile's duration.
console.log(`samples=${totalSamples} (Σ hitCount)  interval≈${sampleIntervalUs}µs (${intervalSource})  ` +
            `span=${((profile.endTime - profile.startTime) / 1000).toFixed(0)}ms (startTime→endTime)`);

// Build id→node for inclusive-time reconstruction. Both invariants below are silent corruptions of the
// inclusive column if left unasserted: a duplicate id drops a whole subtree out of the map, and a child id
// naming no node used to be scored as zero by an `if (!node) return 0`.
const idToNode = new Map();
for (const n of nodes) {
  CHECK(!idToNode.has(n.id), "two profile nodes share id " + n.id + "; ids are unique per Profiler.ProfileNode " +
                             "and the second would silently replace the first in the inclusive-time walk");
  idToNode.set(n.id, n);
}
for (const n of nodes)
  for (const cid of childIds(n))
    CHECK(idToNode.has(cid), "node " + n.id + " names child id " + cid + ", which no node in this profile " +
                             "defines; scoring it as zero would silently shorten every inclusive time above it");

function inclusiveHits(node, seen) {
  if (seen.has(node.id)) return 0;   // a cycle: already counted on this walk
  seen.add(node.id);
  let h = selfHits(node);
  for (const cid of childIds(node)) h += inclusiveHits(idToNode.get(cid), seen);
  return h;
}

const top = nodes
  .filter(n => selfHits(n) > 0)
  .sort((a, b) => selfHits(b) - selfHits(a))
  .slice(0, topN);

console.log(`\ntop ${topN} self-time leaves:`);
console.log("self%   selfMs  inclMs  function  url:line:col");
for (const n of top) {
  // Every field below is REQUIRED by Runtime.CallFrame and has been asserted present and typed at the door,
  // so each read is a value and never a hole. The two conventions are V8's, and both are POSITIVE statements
  // the file makes rather than fields it omits: an empty `functionName` is an anonymous function, and an
  // empty `url` with a negative `lineNumber` is a frame with no script — `(root)`, `(program)`, `(idle)`,
  // `(garbage collector)`. The protocol types both as plain integers/strings, so the meaning is read off the
  // value here rather than asserted as a protocol guarantee it does not make.
  const fn = n.callFrame;
  // THE THREE THINGS THIS ROW DEPENDS ON, ASSERTED WHERE IT DEPENDS ON THEM. The door established that all
  // five members are present and typed; these are narrower claims the RENDERING makes, and each one is a row
  // this loop would otherwise print as nonsense. Measured across 178 frames of two `node --cpu-prof` profiles
  // (JSON, crypto, zlib, sort, backtracking regexp, vm native frames, GC): zero violations of any of them.
  CHECK(fn.functionName !== "" || fn.url !== "",
        "profile node " + n.id + " has neither a function name nor a script — the row would identify the " +
        "frame by nothing at all, and its samples would be attributed to `(anon) (no script)`");
  CHECK(fn.lineNumber < 0 || fn.url !== "",
        "profile node " + n.id + " states a script location (line " + fn.lineNumber + ") with an empty url; " +
        "the row would read `(no script):" + (fn.lineNumber + 1) + "`, which is a contradiction, not a location");
  CHECK(fn.lineNumber < 0 || fn.columnNumber >= 0,
        "profile node " + n.id + " states line " + fn.lineNumber + " with column " + fn.columnNumber + "; the " +
        "protocol types both as 0-based integers, so a negative column beside a real line would print as `:0`");
  const hits = selfHits(n);
  const selfPct = ((hits / totalSamples) * 100).toFixed(1);
  const selfMs = (hits * intervalMs).toFixed(0);
  const inclMs = (inclusiveHits(n, new Set()) * intervalMs).toFixed(0);
  const fname = fn.functionName === "" ? "(anon)" : fn.functionName;
  const url = fn.url === "" ? "(no script)" : fn.url.replace(/\\/g, "/").split("/").pop();
  const loc = fn.lineNumber >= 0 ? ":" + (fn.lineNumber + 1) + ":" + (fn.columnNumber + 1) : "";
  console.log(`${selfPct.padStart(5)}%  ${selfMs.padStart(6)}  ${inclMs.padStart(6)}  ${fname}  ${url}${loc}`);
}
