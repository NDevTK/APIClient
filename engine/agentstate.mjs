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
 * prescribes is to ask the RESULT rather than the source.
 *   THAT RUNTIME INSTRUMENT EXISTS NOW, AND THIS PARAGRAPH IS REWRITTEN RATHER THAN DELETED BECAUSE IT SAID
 *   IT DID NOT. It read `the exact instrument is a runtime one, and it does not exist yet`, and then named
 *   what it needed as `a way to tell a declared realm slot from a declared step or method id, since all
 *   three are SLOT_ID`. Both halves are retired by landings rather than overruled: a realm slot is its own
 *   kind (SLOT_REALM), and core/platform.c compares agent_state_class_id_count against quickjs.h's
 *   JS_ClassIDsMinted over the window the declare column brackets. An absence written in the present tense is
 *   the one direction CLAUDE.md rates worst, because its only reader is somebody deciding whether to BUILD
 *   the thing -- so a stale one argues for a second copy of an instrument that is already there.
 *   WHAT THE RUN SEES AND THIS SWEEP DOES NOT, which is why both are kept: the run's number is the
 *   ALLOCATOR'S, so it counts a mint written in any spelling at all, including the three this file names as
 *   invisible to it. What this sweep sees and the run does not is an ADDRESS -- the run has a count and a
 *   window, and the file and line of an offending mint are here. Neither is the other's floor; they answer
 *   different halves of one question, and the run is the one that cannot be evaded.
 *   THE ROOT BEYOND BOTH is a mint that DECLARES -- one door taking the slot's address and the component's
 *   row, so that an undeclared class id is unconstructible rather than reported by either. That is a
 *   signature change at every mint in the tree, which is what the identity exists to force.
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
/* THE CAPTURE ENDS AT THE ARGUMENT'S END, WHICH IS NOT PEDANTRY: `&f->class_id`, `&rec->slot` and `*slot =` are mints
   into a STRUCT MEMBER, and a pattern that stops at the first identifier captures the POINTER instead. That
   name then has to be tested for being a file static, and the test is a regex over `static ... \bname\b`,
   which a function PARAMETER of the same name satisfies -- so the row is admitted and reads as an undeclared
   slot in a file that has none. Measured once, on `JS_NewClassID(rt, &f->class_id)` against a `static JSValue
   ait_fulfil_result(..., const IdlAsyncIface *f, ...)` eight hundred lines away. It is the accusing direction
   CLAUDE.md says to suspect hardest, so the trailing `\s*\)` and `\s*[,)]` are load-bearing. */
const MINTS = [
  ["class", /JS_NewClassID\s*\(\s*[^,]+,\s*&\s*([A-Za-z_]\w*)\s*\)/g],
  ["realm", /(?<![>.*])\b([A-Za-z_]\w*)(?:\s*\[[^\]]*\])?\s*=\s*realm_value_declare\s*\(/g],
];
/* THE DECLARING KINDS ARE DERIVED FROM core/agent_state.h AND NOT RESTATED HERE. CLAUDE.md: an auditor
   derives the rule it checks from the code that owns it, because a restated rule is a SECOND COPY and the one
   that drifts is the copy nobody runs against reality. A kind added to that header and not to an alternation
   typed out here would move every slot declared through it OUT of `declared` and into `NOT reset` -- an
   accusation manufactured entirely by this file, against the components that had just been routed correctly.
   The entries that DECLARE a slot are exactly the ones taking a `what`; agent_state_undo_at and
   agent_state_reached_at take (component, file, line) and are not declarations. */
const HEADER = "engine/host/browser/core/agent_state.h";
const KINDS = [...read(HEADER).matchAll(/\bagent_state_(\w+)_at\s*\(\s*const char \*component,[^;]*?const char \*what\b/g)]
  .map((m) => m[1]);
if (!KINDS.length)
  throw new Error(`agentstate: no declaring entry was found in ${HEADER} -- this sweep's whole `
                + `\`declared\` band is derived from that list, so an empty one would report every declared `
                + `slot in the tree as undeclared. Either the header's entry shape changed or the path is wrong.`);
const DECLARED = new RegExp(`agent_state_(?:${KINDS.join("|")})(?:_at)?\\s*\\([^;]*?&\\s*([A-Za-z_]\\w*)\\s*[,)]`, "gs");

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
      /* A FILE STATIC AND NOT A LOCAL OR A PARAMETER -- the registry takes an address that outlives a call.
         THE EXCLUDED CHARACTER IS `(` AND IT USED TO BE `=`, which dropped every member but the FIRST of a
         comma-separated declaration list: `static int g_stepid = -1, g_driver_slot = -1;` puts an `=` before
         the second name, so the test failed for it and the row left this sweep entirely. That is the UNDER-
         counting direction, which nothing announces -- the slot is simply absent from every band. Measured at
         8de85780 on the realm channel alone: `rendering.c`'s g_driver_slot and `remote_op.c`'s g_apply_slot,
         both declared, both invisible. `(` keeps the protection the `=` was really buying, which is the one
         recorded in the MINTS comment above: a static FUNCTION whose PARAMETER shares the name cannot match,
         because a parameter is always preceded by the function's open paren. */
      if (!new RegExp(`^\\s*static\\s[^;(]*\\b${id}\\b`, "m").test(s)) continue;
      seen.add(id);
      const line = s.slice(0, m.index).split("\n").length;
      let band;
      if (declared.has(id)) band = `${kind}: declared`;
      else {
      /* THE PRE-INIT SPELLINGS THIS SWEEP CAN SEE, AND `JS_INVALID_CLASS_ID` IS ONE OF THEM. A realm slot's
         C type is JSClassID, so its pre-declaration value is quickjs's own reserved 0 and components spell it
         by that name rather than as a digit. Matching only `-1|0` would have moved every converted slot that
         IS hand-reset into `NOT reset`, which is the louder band and the accusing direction -- a line somebody
         wrote and keeps, reported as a line nobody wrote. It stays a list of SPELLINGS and not a value test
         because this is a text sweep; that is the floor the header already states. */
        const R = new RegExp(`\\b${id}(?:\\s*\\[[^\\]]*\\])?\\s*=\\s*(?:-1|0|JS_INVALID_CLASS_ID)\\b`);
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
console.log(`declaring kinds, read from ${HEADER}: ${KINDS.join(", ")}`);
for (const k of [...tally.keys()].sort()) console.log(`  ${String(tally.get(k)).padStart(4)}  ${k}`);
for (const r of rows.sort((a, b) => (a.band + a.p).localeCompare(b.band + b.p)))
  if (r.band.includes("UNDECLARED") && (!BAND || r.band.includes(BAND)))
    console.log(`${r.band.padEnd(34)} ${r.p}:${r.line}  ${r.id}`);
