/* WHICH AGENT-LIFETIME SLOTS core/agent_state.h HAS NEVER BEEN TOLD ABOUT.
 *
 * That registry's whole value is the assert at the end of the release column: every DECLARED slot is back at
 * its pre-init value, and a release that frees a value and forgets the handle crashes there naming the
 * component and the state. It can only ever ask about slots it was told about. A slot that is minted and
 * never declared is not weakly checked, it is OUTSIDE THE QUESTION -- no release could ever be caught
 * forgetting it, at any revision, by any run this project has ever made. That population is what this prints.
 *
 * IT IS A STATIC SWEEP AND THEREFORE A FLOOR, AND THE SPELLINGS IT SEARCHED ARE PART OF ITS OUTPUT. It reads
 * source text, so a mint written any way other than the two forms it names is invisible to it -- a helper
 * that wraps JS_NewClassID, an id assigned through a pointer, a slot handed out by a component's own factory.
 * CLAUDE.md rates a static derivation over text as a lower bound wearing a total's clothes, and the cure it
 * prescribes is to ask the RESULT rather than the source: the exact instrument is a runtime one, and it does
 * not exist yet. Naming it is part of this file's job, because a reader who takes these numbers for a total
 * will not build it.
 *   THE EXACT INSTRUMENT. A realm slot and a class id are THE SAME OBJECT -- realm_value_declare's body is
 *   JS_NewClassID plus JS_NewClass -- so every row here is a class id, and the runtime already counts them:
 *   `rt->js_class_id_alloc` less JS_CLASS_INIT_COUNT is exactly how many this agent minted. A check at the
 *   end of the declare column comparing that against what agent_state holds is a CONSERVATION IDENTITY over
 *   the one allocator, and it cannot be evaded by a spelling. What it needs that does not exist today is a
 *   way to tell a declared realm slot from a declared step or method id, since all three are SLOT_ID; the
 *   identity closes the moment a realm slot has a kind of its own.
 *   THE ROOT BEYOND THAT is a mint that DECLARES -- one door taking the slot's address and the component's
 *   row, so that an undeclared class id is unconstructible rather than merely reported. That is a signature
 *   change at every mint in the tree, which is why the count below is worth having first.
 *
 * WHAT THE THREE BANDS MEAN, AND WHY THEY ARE NOT ONE NUMBER. CLAUDE.md: N sites spelling one question wrong
 * are not N defects, and the discriminator is what stands UNDER each one.
 *   declared              -- the registry can ask. Nothing owed.
 *   UNDECLARED, hand-reset -- the slot IS put back, by a line somebody wrote and somebody must keep. Correct
 *                            today and unfalsifiable: nothing can report it if a later diff drops the line.
 *   UNDECLARED, NOT reset  -- carried into the next agent. For a class id that is not a style question:
 *                            JS_NewClassID in this fork opens `if (class_id == 0)` and otherwise RETURNS THE
 *                            NUMBER IT IS HANDED, so a carried id is never re-minted -- it names a class in a
 *                            runtime that is gone, while the new runtime's allocator restarts at
 *                            JS_CLASS_INIT_COUNT and hands the same number to somebody else.
 * A row's band is a fact about the tree. Whether it is a DEFECT is a question about that component, and this
 * file does not answer it -- a slot may be reset by a cascade this sweep cannot see, or belong to a component
 * that reasons about it at its own site. Read the row before repairing it.
 *
 * NO EXPECTED TOTAL IS WRITTEN HERE. A number in this header would be status by CLAUDE.md's own opening: it
 * would be true when written, wrong as soon as anybody did the work, and its only reader is the person about
 * to invalidate it. The derivation is the deliverable; run it.
 *
 *   node engine/agentstate.mjs [--rev <revision>] [--band <substring>] [--path <prefix>]
 *
 * With no --rev it reads the WORKING TREE, which in a shared checkout is a tree that moves under the scan --
 * so it prints which it read, and a number anybody quotes is taken with --rev. */
import { readFileSync, readdirSync, statSync } from "node:fs";
import { execFileSync } from "node:child_process";

const argv = process.argv.slice(2);
const opt = (n) => { const i = argv.indexOf(n); return i < 0 ? null : argv[i + 1]; };
const REV = opt("--rev"), BAND = opt("--band"), PATHPFX = opt("--path") ?? "engine/host/";

const listTree = () => execFileSync("git", ["ls-tree", "-r", "--name-only", REV, PATHPFX],
                                    { encoding: "utf8", maxBuffer: 512 * 1024 * 1024 }).split("\n").filter((f) => f.endsWith(".c"));
const listDisk = (d, out = []) => {
  for (const e of readdirSync(d)) {
    const p = d + "/" + e;
    if (statSync(p).isDirectory()) listDisk(p, out); else if (p.endsWith(".c")) out.push(p);
  }
  return out;
};
const files = REV ? listTree() : listDisk(PATHPFX.replace(/\/$/, ""));
const read = (p) => REV ? execFileSync("git", ["show", `${REV}:${p}`], { encoding: "utf8", maxBuffer: 512 * 1024 * 1024 })
                        : readFileSync(p, "utf8");

/* THE TWO SPELLINGS THIS SWEEP CAN SEE, named in the output because they bound it. */
const MINTS = [
  ["class", /JS_NewClassID\s*\(\s*[^,]+,\s*&\s*([A-Za-z_]\w*)/g],
  ["realm", /([A-Za-z_]\w*)(?:\s*\[[^\]]*\])?\s*=\s*realm_value_declare\s*\(/g],
];
const DECLARED = /agent_state_(?:id|flag|class|atom|value|ptr)(?:_at)?\s*\([^;]*?&\s*([A-Za-z_]\w*)/gs;

/* Top-level function bodies, by brace balance -- used only to ask whether a RELEASE resets a slot, so a
   miss here can only move a row into the louder band, never out of it. */
function bodies(s) {
  const out = new Map();
  for (const m of s.matchAll(/^(?:[A-Za-z_][\w \t*]*?)\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{/gm)) {
    let d = 0, i = m.index + m[0].length - 1;
    for (let j = i; j < s.length; j++) {
      if (s[j] === "{") d++;
      else if (s[j] === "}" && --d === 0) { out.set(m[1], s.slice(i, j)); break; }
    }
  }
  return out;
}

const rows = [], tally = new Map();
for (const p of files) {
  const s = read(p);
  if (!s) continue;
  const declared = new Set([...s.matchAll(DECLARED)].map((m) => m[1]));
  const rel = [...bodies(s)].filter(([k]) => k.includes("_free") || k.includes("_release"));
  for (const [kind, rx] of MINTS) {
    const seen = new Set();
    for (const m of s.matchAll(new RegExp(rx.source, "g"))) {
      const id = m[1];
      if (seen.has(id)) continue;
      /* a file static and not a local or a parameter -- the registry takes an address that outlives a call */
      if (!new RegExp(`^\\s*static\\s[^;=]*\\b${id}\\b`, "m").test(s)) continue;
      seen.add(id);
      const line = s.slice(0, m.index).split("\n").length;
      let band;
      if (declared.has(id)) band = `${kind}: declared`;
      else {
        const R = new RegExp(`\\b${id}(?:\\s*\\[[^\\]]*\\])?\\s*=\\s*(?:-1|0)\\b`);
        band = `${kind}: UNDECLARED, ${rel.some(([, b]) => R.test(b)) ? "hand-reset" : "NOT reset"}`;
      }
      tally.set(band, (tally.get(band) ?? 0) + 1);
      rows.push({ band, p, id, line });
    }
  }
}

if (!rows.length)
  throw new Error("agentstate: this sweep found no mint at all, which is not a clean tree -- either the two "
                + "spellings in MINTS have been renamed or the path prefix is wrong. A zero here is a "
                + "statement about this file and not about the engine.");

console.log(`agent-state coverage over ${files.length} files at ${REV ?? "the WORKING TREE (moves under the scan)"}`);
console.log(`spellings searched: ${MINTS.map(([k]) => k).join(", ")} -- anything else minted is invisible here`);
for (const k of [...tally.keys()].sort()) console.log(`  ${String(tally.get(k)).padStart(4)}  ${k}`);
for (const r of rows.sort((a, b) => (a.band + a.p).localeCompare(b.band + b.p)))
  if (r.band.includes("UNDECLARED") && (!BAND || r.band.includes(BAND)))
    console.log(`${r.band.padEnd(34)} ${r.p}:${r.line}  ${r.id}`);
