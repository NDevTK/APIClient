// wasm-symbolize.cjs — attribute wasm trap frames to SOURCE, from the SHIPPED binary.
//
// WHAT IT ANSWERS, AND WHAT IT CANNOT.
//   ANSWERS: which SOURCE FILE a frame's code came from, and what the code AT THE TRAP PC says —
//     read out of the binary's own data segments, by following the i32.const addresses each body
//     references into the strings they point at. It works on this project's artifacts because
//     check.h bakes __FILE__ and every DCHECK/DFAIL message into the binary as string constants,
//     and build.mjs's -ffile-prefix-map makes that __FILE__ REPO-RELATIVE. The engine's own
//     offensive-programming discipline is what makes the binary self-describing.
//   DOES NOT ANSWER: the FUNCTION NAME. There is none in the binary (see below) and this tool does
//     not invent one. It reports EVIDENCE — file paths and message strings — and never a name. Two
//     functions in one file share that file's path, and this tool says the file, which is the true
//     answer to the question it is being asked; a name would be a guess between them.
//   DOES NOT ANSWER: anything about a body that carries no assert. Roughly half of them do not —
//     the run prints the exact fraction for the binary in hand — and for those the report is
//     UNIDENTIFIED. A refusal is the point: a confidently wrong frame is worse than an
//     unsymbolized one, and five consecutive layout aborts were once diagnosed by guessing.
//   DOES NOT ANSWER: which INLINED callee the pc is in when a body holds code from several files.
//     At -O1 a static helper is inlined into its caller and contributes its strings to the caller's
//     body, so a body's string set is the UNION over its inline tree. Where that union names more
//     than one source file this tool says AMBIGUOUS and lists them; it does not pick. (Measured on
//     this tree's smoke artifact: 1 body of 10575, a `.h` static inlined into its own `.c`. Rare,
//     not impossible, and the day it stops being rare the report says so rather than degrading.)
//
// WHY IT READS STRINGS AND NOT A NAME SECTION. The shipped artifact has no name section: both
// extension/lib/qjs/qjs.wasm and engine/host/out/qjs.wasm carry exactly ONE custom section,
// `target_features` (148 bytes). emcc's wasm-ld strips the name section unless a name-emitting
// setting is on — the toolchain says so in its own words at
// engine/.work/emsdk/upstream/emscripten/tools/building.py: "wasm-ld can strip debug info for us.
// this strips both the Names section and DWARF, so we can only use it when we don't need any of
// those things", guarded by `not settings.EMIT_NAME_SECTION` among others — and engine/build.mjs
// sets none of them (it reads no process.env at all).
//
// THIS TOOL SHOULD BE DELETED THE DAY THE LINK CARRIES NAMES. `--profiling-funcs` sets
// EMIT_NAME_SECTION and nothing else (tools/cmdline.py: one line, no DEBUG_LEVEL change), so it
// stops the strip without changing codegen — and with a name section present V8 and node print the
// FUNCTION NAME in the frame itself (`at lb_box_fragments (wasm://…:wasm-function[7391]:0x8eff43)`),
// at which point nothing here has a question left to answer and this file goes. It would be a
// strictly better answer than string evidence — it names the function, it covers the bodies that
// carry no assert, and it is not defeated by inlining any worse than this is. This tool is the
// instrument for the tree as it is built today, not an argument against fixing the build.
//
// THIS REPLACED a name-section reader that could not run on anything this tree produces, and it is
// deleted rather than kept beside this: its documented workflow was
// `WASM_NAMES=1 node engine/build.mjs cow` against engine/qjs/qjs_worker.wasm. That env var is read
// by nothing (it belonged to the PRE-FORK engine/build.mjs, deleted whole in 203e76fb "fresh fork:
// … delete wrong-logic host"), that file does not exist, and engine/cow-barrier.mjs — the binaryen
// round-trip its comment blamed for the strip — went in the same commit. It announced a REMEDY, so
// nobody looked further; the reader who tried it got a build with no names and a symbolizer that
// printed "NO name section". And in the one world where it could have run it was already
// unnecessary, because a module with names needs no symbolizer: the runtime prints them.
//
// USAGE:  node testing/wasm-symbolize.cjs <file.wasm> [frames…]
//   Frames are `INDEX:0xOFFSET` pairs, or just paste the raw stack — any line containing
//   `wasm-function[N]:0xM` is parsed. Frames may also come on stdin:
//     node testing/wasm-symbolize.cjs engine/host/out/qjs.wasm < trap.txt
//
// THE OFFSET IS REQUIRED, AND IT IS NOT DECORATION — IT IS THE PROOF THAT THIS BINARY PRODUCED
// THIS STACK. A frame index means nothing across two builds, and symbolizing a stack against the
// wrong artifact is the failure this tool must not have: it would name source files confidently and
// be wrong. Every wasm frame V8 prints carries the module-relative code offset, and that offset
// must fall inside the body of the function at that index. It is a sharp discriminator and not a
// plausibility check: measured on this tree's two artifacts, all 17 frames of a smoke-build stack
// fall INSIDE their bodies in the smoke binary and all 17 fall OUTSIDE them in the ABI binary. So a
// mismatch REFUSES THE WHOLE RUN, before anything is printed that could be read first.
//
// KNOWN IMPRECISION, STATED RATHER THAN HIDDEN: the body scan looks for the 0x41 opcode byte and
// decodes an LEB after it. That is a byte scan, NOT a disassembly, so a 0x41 occurring inside some
// other instruction's immediate can be misread as an i32.const. Such a misread rarely resolves to a
// plausible string (it must land inside an active data segment AND yield printable bytes with no
// control characters), but it is why the local window below is reported with signed distances for a
// human to judge, and why a lone unexpected string is evidence and not a verdict.
"use strict";
const fs = require("fs");
const path = require("path");

const wasmPath = process.argv[2];
if (!wasmPath) {
  console.error("usage: node testing/wasm-symbolize.cjs <file.wasm> [INDEX:0xOFFSET …]   (or pipe the stack on stdin)");
  process.exit(2);
}

/* FRAMES COME FROM THE STACK AS IT WAS PRINTED, so the parser accepts the printed form verbatim
   rather than asking a reader to retype indices — retyping is where a frame gets dropped or a digit
   changes, and this tool's whole guarantee rests on the offset that would go with it. */
let raw = process.argv.slice(3).join("\n");
if (!process.stdin.isTTY) { try { raw += "\n" + fs.readFileSync(0, "utf8"); } catch (e) { /* no stdin */ } }
const parsed = [];
for (const m of raw.matchAll(/wasm-function\[(\d+)\]:(0x[0-9a-fA-F]+|\d+)/g)) parsed.push([+m[1], Number(m[2])]);
for (const m of raw.matchAll(/(?:^|\s)(\d+):(0x[0-9a-fA-F]+)(?!\d)/g)) parsed.push([+m[1], Number(m[2])]);
if (!parsed.length) {
  console.error("REFUSED: no frames given. This tool needs INDEX:0xOFFSET pairs — a bare function index\n" +
                "cannot be checked against this binary, and an unchecked frame is exactly the confidently\n" +
                "wrong answer this tool exists not to give. Paste the wasm frames of the stack.");
  process.exit(2);
}
/* De-dup while keeping stack order: one (index, offset) printed twice is one frame. The SAME index
   at two offsets is two frames and both are kept — a recursive function appears that way. */
const seenFrame = new Set();
const stack = parsed.filter(([i, o]) => { const k = i + ":" + o; if (seenFrame.has(k)) return false; seenFrame.add(k); return true; });

const b = fs.readFileSync(wasmPath);
if (b.length < 8 || b.readUInt32LE(0) !== 0x6d736100) { console.error("not a wasm file (bad magic): " + wasmPath); process.exit(2); }

let p = 8;
function u32() { let r = 0, s = 0, x; do { x = b[p++]; r |= (x & 127) << s; s += 7; } while (x & 128); return r >>> 0; }
function s32() { let r = 0, s = 0, x; do { x = b[p++]; r |= (x & 127) << s; s += 7; } while (x & 128); if (s < 32 && (x & 0x40)) r |= (~0 << s); return r | 0; }

let importedFuncs = 0, codeStart = -1, dataStart = -1;
const customs = [];
while (p < b.length) {
  const id = b[p++], len = u32(), st = p;
  if (id === 0) { const q = p; const nl = u32(); customs.push(b.toString("utf8", p, p + nl)); p = q; }
  if (id === 2) {
    const n = u32();
    for (let i = 0; i < n; i++) {
      const ml = u32(); p += ml; const nl = u32(); p += nl;
      const k = b[p++];
      if (k === 0) { u32(); importedFuncs++; }        // a function import shifts every index below
      else if (k === 1) { p++; const lf = b[p++]; u32(); if (lf) u32(); }
      else if (k === 2) { const lf = b[p++]; u32(); if (lf) u32(); }
      else if (k === 3) { p++; p++; }
    }
  }
  if (id === 10) codeStart = st;
  if (id === 11) dataStart = st;
  p = st + len;
}
if (codeStart < 0) { console.error("no code section in " + wasmPath); process.exit(2); }

p = codeStart;
const nBodies = u32();
const bodies = [];
for (let i = 0; i < nBodies; i++) { const sz = u32(); bodies.push([p, p + sz]); p += sz; }

/* ACTIVE data segments only — a passive segment has no linear address, so a constant cannot be
   pointing into one and a string that resolves nowhere is simply not reported. */
const segs = [];
if (dataStart >= 0) {
  p = dataStart;
  const nseg = u32();
  for (let i = 0; i < nseg; i++) {
    const flags = u32();
    let addr = 0, active = (flags === 0 || flags === 2);
    if (active) {
      if (flags === 2) u32();
      const op = b[p++];
      if (op === 0x41) addr = s32(); else active = false;   // a non-constant offset: not addressable here
      while (p < b.length && b[p] !== 0x0b) p++;
      p++;
    }
    const sz = u32();
    if (active) segs.push([addr, addr + sz, p]);
    p += sz;
  }
  segs.sort((a, x) => a[0] - x[0]);
}

function readStr(addr) {
  for (const [lo, hi, off] of segs) {
    if (addr < lo || addr >= hi) continue;
    const start = off + (addr - lo), end = off + (hi - lo);
    let j = start;
    while (j < end && b[j] !== 0) j++;
    if (j === start) return null;
    const s = b.toString("utf8", start, j);
    /* A control character means this was not text — the byte scan misread something, and printing
       it as a message would be manufacturing evidence out of a decoding accident. */
    if (/[\x00-\x08\x0b\x0c\x0e-\x1f]/.test(s)) return null;
    return s;
  }
  return null;
}

/* A SOURCE PATH IS THE ONE PIECE OF EVIDENCE THAT IS SELF-IDENTIFYING. -ffile-prefix-map makes
   check.h's __FILE__ repo-relative, so it is a path this repo can be asked about, and it is the
   answer a reader of a trap actually wants. Everything else is a message: useful, but a message can
   be shared between sites, moved, or copied, so it is reported as evidence and never as identity. */
const SRC = /^[A-Za-z0-9_./+-]+\.(?:c|h|cc|cpp)$/;

/* One pass over a body, returning every (position, string) the body's constants point at. The
   position is what makes the pc window below possible, and it is why this returns pairs rather
   than a set. */
function evidence(lo, hi, stopAtFirstSource) {
  const out = [];
  const save = p;
  for (let i = lo; i < hi; i++) {
    if (b[i] !== 0x41) continue;
    p = i + 1;
    let v;
    try { v = s32(); } catch (e) { continue; }
    const consumed = p - i;
    if (consumed < 2 || v < 1024) continue;       // a 1-byte immediate is a small int, not an address
    const s = readStr(v);
    if (!s || s.length < 4) continue;
    if (stopAtFirstSource) { if (SRC.test(s)) { p = save; return [[i, s]]; } continue; }
    out.push([i, s]);
  }
  p = save;
  return out;
}

const stamp = (() => {
  const dir = path.dirname(path.resolve(wasmPath));
  let found = null;
  for (const f of fs.readdirSync(dir)) {
    if (!f.endsWith(".build.json")) continue;
    try { found = [f, JSON.parse(fs.readFileSync(path.join(dir, f), "utf8"))]; } catch (e) { /* unreadable */ }
  }
  return found;
})();

/* THE CALIBRATION IS COMPUTED, NOT WRITTEN DOWN. A coverage figure in a comment is a claim about a
   tree that moves; this one is a fact about the binary in the reader's hand, and it costs ~90 ms.
   It is what tells a reader whether an UNIDENTIFIED frame below is unusual or ordinary. */
let named = 0;
for (const [lo, hi] of bodies) if (evidence(lo, hi, true).length) named++;

console.log("binary: " + wasmPath + "  (" + b.length + " bytes, " + importedFuncs + " imported funcs, " +
            nBodies + " defined funcs, custom sections: " + (customs.length ? customs.join(", ") : "none") + ")");
console.log("evidence coverage: " + named + " of " + nBodies + " bodies (" + (100 * named / nBodies).toFixed(1) +
            "%) reference a source path; the other " + (nBodies - named) + " this tool must refuse.");
if (customs.includes("name")) {
  console.log("NOTE: this binary HAS a name section, so the runtime prints function names in the stack itself.\n" +
              "      Read them off the trace — that is the better answer, and it means this tool can go.");
}
/* THE STRINGS BELONG TO A REVISION, NOT TO THE WORKING TREE. A message a peer has since edited or
   moved still reads exactly as it read when this binary was linked, so the revision is part of the
   answer — without it a reader maps a string onto a source file that has moved under it, which is
   how a symbolized frame sends someone to the wrong line of a file it named correctly. */
if (stamp) console.log("build stamp: " + stamp[0] + " head=" + (stamp[1].head || "?") + " qjs=" + (stamp[1].qjsHead || "?") + " at=" + (stamp[1].at || "?") +
                       (Array.isArray(stamp[1].dirty) && stamp[1].dirty.length ? "  DIRTY=" + stamp[1].dirty.length + " files, so this binary is at NO revision" : ""));
else console.log("build stamp: NONE beside this artifact — the revision these strings belong to is UNKNOWN.\n" +
                 "             Read every path below at whatever revision produced this binary, not at origin/main.");

/* PASS ONE: PROVE THE BINARY. Nothing about any frame is printed until every frame has been
   checked, because a partial report from the wrong artifact would be read before the refusal that
   follows it. */
const bad = [];
for (const [gi, off] of stack) {
  const di = gi - importedFuncs;
  if (di < 0 || di >= bodies.length) continue;   // out of range is a per-frame report, not a binary mismatch
  const [lo, hi] = bodies[di];
  if (!(off >= lo && off < hi)) bad.push([gi, off, lo, hi]);
}
if (bad.length) {
  console.log("\nREFUSED — THIS BINARY DID NOT PRODUCE THIS STACK.");
  console.log(bad.length + " of " + stack.length + " frames carry a code offset that is not inside the body of the");
  console.log("function at that index. A frame index is meaningless across two builds, so symbolizing this");
  console.log("stack here would name source files confidently and wrongly. Find the artifact that trapped.");
  for (const [gi, off, lo, hi] of bad.slice(0, 8)) {
    console.log(`  wasm-function[${gi}] pc=0x${off.toString(16)} but body #${gi - importedFuncs} is [0x${lo.toString(16)},0x${hi.toString(16)})`);
  }
  process.exit(1);
}

/* PASS TWO: REPORT. */
for (const [gi, off] of stack) {
  const di = gi - importedFuncs;
  console.log(`\n===== wasm-function[${gi}]  pc=0x${off.toString(16)} =====`);
  if (di < 0) { console.log("  IMPORTED — this frame is one of the " + importedFuncs + " imported functions; it has no body in this module."); continue; }
  if (di >= bodies.length) { console.log("  OUT OF RANGE — this module defines " + bodies.length + " functions; index " + gi + " names none of them."); continue; }
  const [lo, hi] = bodies[di];
  const ev = evidence(lo, hi, false);
  const srcs = [...new Set(ev.map((e) => e[1]).filter((s) => SRC.test(s)))];
  console.log(`  body ${hi - lo} bytes, pc at +${off - lo}`);

  if (srcs.length === 0) {
    console.log("  SOURCE: UNIDENTIFIED — this body references no repo-relative source path. It carries no");
    console.log("          DCHECK/DFAIL, or its asserts were compiled out. No evidence here, and no guess.");
  } else if (srcs.length === 1) {
    console.log("  SOURCE: " + srcs[0]);
  } else {
    console.log("  SOURCE: AMBIGUOUS — this body holds code from " + srcs.length + " files (static callees inlined at -O1).");
    console.log("          Which one the pc is in is NOT decided by strings. Candidates:");
    for (const s of srcs) console.log("            " + s);
  }

  /* THE PC WINDOW IS THE SHARP HALF. A 16 KB body references dozens of strings and listing them all
     is what made a whole-body dump hard to act on; the constants materialized within a few dozen
     bytes of the trap are the ones the trap is about. Measured on this tree: the abort message that
     actually fired sat 132 bytes before the pc of the frame beneath abort(). */
  const win = ev.filter(([pos]) => Math.abs(pos - off) <= 256).sort((a, x) => Math.abs(a[0] - off) - Math.abs(x[0] - off));
  if (win.length) {
    console.log("  AT THE PC (signed byte distance; nearest first — a WINDOW, not a claim about which one fired):");
    for (const [pos, s] of win.slice(0, 6)) {
      const d = (pos < off ? "-" : "+") + Math.abs(pos - off);
      console.log(`    ${d.padStart(6)}  ${s.length > 400 ? s.slice(0, 400) + " …" : s}`);
    }
  } else {
    console.log("  AT THE PC: no string constant within 256 bytes of the trap.");
  }

  const rest = [...new Set(ev.map((e) => e[1]))].filter((s) => !SRC.test(s) && s.length >= 12 && !win.some((w) => w[1] === s));
  if (rest.length) {
    console.log(`  ELSEWHERE IN THIS BODY (${rest.length} other strings, first 8 — evidence about the body, not the pc):`);
    for (const s of rest.slice(0, 8)) console.log("    | " + (s.length > 160 ? s.slice(0, 160) + " …" : s));
  }
}
