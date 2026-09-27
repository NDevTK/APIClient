/* DOES THIS DECLARED THING HAVE A CONSUMER? — a probe that answers it by MUTATING THE SOURCE and letting the
   COMPILER name every translation unit that reaches the thing, rather than by searching text for a spelling.
   WHY IT IS NOT A GREP, WHICH IS THE WHOLE OF ITS REASON TO EXIST. CLAUDE.md
   §AND-THE-SYMBOL-CAN-BE-THE-WRONG-QUESTION-ENTIRELY: components integrate through hook tables, vtables,
   registries and `X()`-macro lists, none of which spell the callee at the use, so a grep over a CALLING
   component for a CALLEE's name answers zero at every revision forever and that zero is the architecture
   answering rather than an absence. Remove the DECLARATION instead and every reference becomes a hard error
   that names itself — a call, an address taken for a table install, a designated initializer, a field
   assignment. That is a construction and not a search: with `-Werror=implicit-function-declaration` in the
   build's own flags there is no spelling of a reference that survives the removal quietly.
   TWO QUESTIONS, AND THEY ARE NOT THE SAME QUESTION AS A SYMBOL TABLE'S. `llvm-nm`'s `U` channel answers
   `does ANOTHER TU reference this`, over a coherent single-revision object set — the condition that holds
   inside a build and nowhere else — and it is blind to an INTRA-TU reference, which for a hook body installed
   in the file that defines it is the normal case. This probe answers `does ANYTHING reference this at all`,
   from SOURCE, revision-coherent by construction. Neither is better; they answer different things, and a
   struct MEMBER has no symbol-table form at all because members are not symbols.

   THE FIVE DEFECT CLASSES THIS INSTRUMENT WAS BUILT OUT OF. Each one produced a confident wrong answer in the
   DESTRUCTIVE direction — an entry reading as dead, which argues for deleting a live capability — and each is a
   property of the EVIDENCE CHANNEL rather than of the subject, so this is where a reader meets them.
     (1) A DIAGNOSTIC LIST HAS A CAP YOU DID NOT SET. clang stops at 20 errors by default, so a TU referencing
         many members reports a PREFIX of them and the rest read as unreferenced. Measured: one TU reported 12
         names at the default and 13 at `-ferror-limit=0`, and that one hidden pair was an entire false orphan.
         Closed here: `-ferror-limit=0` on every compile, no exceptions.
     (2) A QUOTED `#include "x.h"` RESOLVES TO THE INCLUDING FILE'S OWN DIRECTORY FIRST, so an `-I` shadow can
         never reach a TU that sits beside the real header — and this is not a flag anybody can raise. Measured:
         a harness TU adjacent to the header compiled EXIT 0 against the shadow, having never seen it. Closed
         here: the sweep set EXCLUDES the header's own directory and the probe PRINTS those files by name, so a
         reader knows exactly which ones it must read by hand instead of assuming the sweep covered them.
     (3) ONE CONSTRUCT CAN HAVE SEVERAL MESSAGE FORMS, and a probe keyed on one is blind to the others. Measured:
         a field assignment gives `no member named 'x' in 'struct S'` while a designated initializer gives
         `field designator 'x' does not refer to any field in type 'S'` — two forms, and keyed on the first
         alone the hook sweep reported ONE installed member and would have convicted every other one. Closed
         here: the forms are a list per probe, and the control below REFUSES to pass unless every population
         member was named through some form, so a missing form is a startup failure rather than a quiet floor.
     (4) A CONTROL CAN ARM PERFECTLY AND BE A CONTROL FOR A DIFFERENT PROPOSITION. The control that missed (3)
         used field assignments — the minority construct, one site out of nineteen — while the population it was
         calibrating is designated initializers. It spoke on every row and calibrated nothing. Closed here: each
         probe's control is written in the construct its POPULATION actually uses, and says so at its site.
     (5) AN INSTRUMENT CAN FAIL SILENTLY AND SUCCESSFULLY. binutils `nm` on a WASM object prints
         `file format not recognized` and EXITS 0, so a pipeline over it reads zero for every row including the
         positive control. Closed here by the same construction as (3): nothing is believed until the control
         has spoken, and a control that says nothing is a refusal rather than a pass.

   AND THE HAZARD THAT IS SPECIFIC TO THIS FILE: IT MUST NOT IMPORT `engine/build.mjs`. The include roots have
   to come from the build's own declaration rather than a copy — §AN-AUDITOR-DERIVES-THE-RULE — and the obvious
   way to get them is to import the module and read the export. `build.mjs` DOES ITS WORK AT IMPORT: CLAUDE.md
   records an agent that imported it, compiled hundreds of translation units and OVERWROTE the shared artifact
   every harness driver loads, leaving a program belonging to no revision where the next measurement would find
   it. So the roots are PARSED out of the declaration textually, and the parse THROWS when it cannot find what
   it expects rather than falling back to a hardcoded list — a fallback there would turn `I could not read the
   build` into a plausible datum, which is the defect §A-FIELD-A-CONSUMER-DEFAULTS is about. (`build.mjs` also
   answers `--list-include-roots`, which is the authoritative spelling for whoever is allowed to run it; a lane
   that may not build may not invoke it either, which is why this parses instead.)

   THE TWO ASSERTION REGIMES ARE TWO POPULATIONS, AND RUNNING ONLY ONE IS A PARTIAL ANSWER. A consumer can be
   DEV-ONLY — a diagnostic inside `#if APICLIENT_DEV`, which release compiles out — so a member consumed in dev
   and convicted at `release` is telling you WHERE its consumer is rather than that it has none. That is worth
   knowing rather than worth fixing: CLAUDE.md §THE-ARM-BENEATH-A-`DFAIL`-IS-A-SHIPPED-CODE-PATH rates the
   release arm as a population no gate here exercises, and this is one of the few readings that sees it. It was
   measured on the first release run of this file: one entry read as unconsumed because both of its host call
   sites are dev-only census reads, while its load-bearing consumer is an unconditional call in one of the
   adjacent files this probe prints and cannot see — which is the caveat doing its job rather than a defect.

   NO COUNTS IN THIS BANNER, DELIBERATELY. A figure in an instrument's header is the most authoritative place a
   stale number can sit, and every population this thing measures is a fact about a tree that moves. What is
   stated here is the DERIVATION; the run prints the number. */

import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, dirname, resolve, relative } from "node:path";
import { fileURLToPath } from "node:url";

const HERE   = dirname(fileURLToPath(import.meta.url));
const ROOT   = resolve(HERE, "..");
const ENGINE = join(ROOT, "engine");
const BUILD  = join(ENGINE, "build.mjs");
const HEADER = join(ENGINE, "qjs", "quickjs.h");

/* THE BUILD'S OWN FLAGS, PARSED FROM ITS OWN DECLARATION AND NEVER COPIED. Identifiers are resolved
   transitively through `const X = <expr>` and the only expressions admitted are a string literal, a bare
   identifier and `join(a, "lit")` — anything else THROWS, because a silently-unresolved root is an include path
   missing from every compile in the sweep and would read as a tree that does not compile. */
function buildFlags() {
  const src = readFileSync(BUILD, "utf8");
  const decl = (name) => {
    const m = new RegExp("^const\\s+" + name + "\\s*=\\s*([^;]+);", "m").exec(src);
    if (!m) throw new Error(`consumer_probe: engine/build.mjs no longer declares \`const ${name} = …;\` — the ` +
                            `derivation this probe rests on has moved, and guessing the value would put a ` +
                            `wrong include path into every compile. Re-read build.mjs and fix this parse.`);
    return m[1].trim();
  };
  const evalExpr = (e, depth = 0) => {
    if (depth > 8) throw new Error("consumer_probe: build.mjs path expression nests deeper than this parse goes");
    e = e.trim();
    let m;
    if ((m = /^"([^"]*)"$/.exec(e))) return m[1];
    if ((m = /^join\(\s*(.+?)\s*,\s*"([^"]*)"\s*\)$/.exec(e))) return join(evalExpr(m[1], depth + 1), m[2]);
    if ((m = /^resolve\(\s*(.+?)\s*,\s*"([^"]*)"\s*\)$/.exec(e))) return resolve(evalExpr(m[1], depth + 1), m[2]);
    if (/^[A-Za-z_]\w*$/.test(e)) {
      if (e === "ENGINE") return ENGINE;           /* build.mjs derives this from its own module URL */
      return evalExpr(decl(e), depth + 1);
    }
    throw new Error(`consumer_probe: build.mjs include root \`${e}\` is an expression this parse does not ` +
                    `admit. Extend the parse rather than hardcoding the path.`);
  };
  const rootsExpr = decl("ENGINE_INCLUDE_ROOTS");
  const inner = /^\[([\s\S]*)\]$/.exec(rootsExpr);
  if (!inner) throw new Error("consumer_probe: ENGINE_INCLUDE_ROOTS is no longer an array literal");
  const roots = [];
  let d = 0, cur = "";
  for (const ch of inner[1]) {                      /* split on top-level commas: join(HOST, "browser") is ONE */
    if (ch === "(") d++; if (ch === ")") d--;
    if (ch === "," && d === 0) { roots.push(cur); cur = ""; } else cur += ch;
  }
  if (cur.trim()) roots.push(cur);
  const quiet = decl("QUIET_WARNINGS");
  const warn = [...quiet.matchAll(/"([^"]+)"/g)].map((m) => m[1]);
  if (!warn.includes("-Werror=implicit-function-declaration"))
    throw new Error("consumer_probe: the build no longer promotes an implicit function declaration to an " +
                    "ERROR, so removing a declaration would leave a CALL as a warning and this probe's whole " +
                    "evidence channel is gone. Read build.mjs before trusting any result from this file.");
  return { roots: roots.map((r) => evalExpr(r)), warn };
}

const CC = process.env.CONSUMER_PROBE_CC || "clang";
const { roots: INCLUDE_ROOTS, warn: WARN } = buildFlags();
const CFLAGS = [...INCLUDE_ROOTS.flatMap((r) => ["-I", r]), ...WARN,
                "-D_GNU_SOURCE", "-DENABLE_DUMPS", "-fsyntax-only", "-ferror-limit=0"];   /* (1) */

function compile(file, extraIncludeFirst, dev) {
  const argv = [...(extraIncludeFirst ? ["-I", extraIncludeFirst] : []), ...CFLAGS,
                "-DAPICLIENT_DEV=" + dev, file];
  try { execFileSync(CC, argv, { stdio: ["ignore", "ignore", "pipe"] }); return { code: 0, err: "" }; }
  catch (e) { return { code: e.status ?? 1, err: (e.stderr || "").toString() }; }
}

/* THE TRANSLATION UNITS THIS PROBE CAN SEE, AND THE ONES IT CANNOT — derived, never typed. (2) A TU inside the
   header's own directory resolves `#include "quickjs.h"` to the real file whatever `-I` says, so it is
   EXCLUDED from the sweep and RETURNED separately for a reader to read by hand. */
function translationUnits() {
  const ls = execFileSync("git", ["-C", ROOT, "ls-files", "engine/host/*.c", "engine/host/**/*.c",
                                  "engine/qjs/*.c"], { encoding: "utf8" }).trim().split("\n").filter(Boolean);
  const hdrDir = relative(ROOT, dirname(HEADER));
  return { swept:   ls.filter((f) => dirname(f) !== hdrDir).map((f) => join(ROOT, f)),
           adjacent: ls.filter((f) => dirname(f) === hdrDir) };
}

/* HARVEST: which population members did the compiler name in this TU's diagnostics? (3) A construct may be
   reported through several forms, so a probe carries the FORMS as data and the control proves the set complete. */
const named = (stderr, forms, population) => {
  const hit = new Set();
  for (const re of forms)
    for (const m of stderr.matchAll(re)) if (population.includes(m[1])) hit.add(m[1]);
  return [...hit];
};

/* ───────── PROBE 1: the FLOW-API entries ─────────
   POPULATION AS A CONSTRUCT: every `JS_EXTERN` function whose name carries the prefix the codebase itself
   maintains for flow lifecycle, plus the installers that arm the flow machinery. Parsed from the header, so the
   set is whatever the header declares today. EXCLUDES every other `JS_EXTERN` in the header, and excludes
   file-local entries (a `static` has no declaration to remove and the compiler already enforces its use).
   CONTROL CONSTRUCT (4): the population is referenced in the tree by CALL and by ADDRESS (a hook installed into
   a table is an address), so the control takes each one's ADDRESS — which needs the declaration exactly as a
   call does, and is therefore the form that covers both. */
const flowProbe = {
  name: "flow-API entries",
  population(h) {
    const decls = [...h.matchAll(/JS_EXTERN\s+[^;{]*?\b(JS_[A-Za-z0-9_]+)\s*\(/gs)].map((m) => m[1]);
    const uniq = [...new Set(decls)];
    return uniq.filter((d) => /^JS_Flow/.test(d) ||
                              d === "JS_SetFlowControlHooks" || d === "JS_SetTimeTravelHooks");
  },
  mutate(h, pop) {
    let out = h;
    for (const n of pop) {
      const re = new RegExp("JS_EXTERN\\s+[^;{]*?\\b" + n + "\\s*\\([^;{]*?\\);", "s");
      if (!re.test(out)) throw new Error(`consumer_probe: could not remove the declaration of ${n}`);
      out = out.replace(re, `/* consumer_probe: ${n} declaration removed */`);
    }
    return out;
  },
  /* ONE CONTROL PER CONSTRUCT, AND EACH MUST NAME EVERY MEMBER ON ITS OWN. A single control that exercises
     several constructs is the shape that cannot fail for the reason it exists to catch: with both an address
     and a call in one control, deleting either message form still leaves every member named by the other, so
     the missing form passes. Measured on this very file — the designated-initializer form was deleted and the
     probe exited 0. Split per construct and a missing form is a member the construct's OWN control cannot get
     named, which is a throw. */
  controls: [
    { construct: "address taken (a table install needs the declaration exactly as a call does)",
      src: (pop) => ['#include "quickjs.h"', "#include <stdint.h>", "void probe_ctl_addr(void);",
                     "void probe_ctl_addr(void) {", "    volatile uintptr_t p = 0;",
                     ...pop.map((n) => `    p = (uintptr_t)&${n};`), "    (void)p;", "}"].join("\n") },
  ],
  /* …AND A FORM WHOSE CONSTRUCT CANNOT BE ARMED OVER THE POPULATION IS ARMED SYNTHETICALLY, WHICH IS A DIFFERENT
     QUESTION AND WAS WORTH SEPARATING. A population control asks `can every member be named`; a form control
     asks `does this regex match what the compiler says for THIS construct at all`. For a CALL the two come
     apart: a generic call control cannot exist, because calling each entry with no arguments is a hard error
     against the REAL header (wrong argument count) and so the control would fail the well-formedness check for
     a reason that has nothing to do with the mutation. The ADDRESS control generalises and carries the
     population; the call FORM is armed on a synthetic undeclared call whose expected name is known. Conflating
     the two is what let a missing form pass here once. */
  formProbes: [
    { construct: "called", expect: "probe_undeclared_callee",
      src: "void probe_form(void); void probe_form(void) { (void)probe_undeclared_callee(); }" },
  ],
  forms: [/undeclared function '([A-Za-z_]\w*)'/g, /undeclared identifier '([A-Za-z_]\w*)'/g],
};

/* ───────── PROBE 2: the HOOK-TABLE members ─────────
   POPULATION AS A CONSTRUCT: every member the two hook structs declare, parsed WITH COMMENTS STRIPPED FIRST and
   WITHOUT assuming a member is spelled as an inline function pointer. Both matter and both were measured wrong:
   a `(*name)` pattern MISSES a member whose type is a typedef'd function pointer, and failing to strip comments
   INVENTS members out of words inside them. EXCLUDES nothing within the two structs — a data member is swept
   like a hook, because `does anybody set this` is the same question — and excludes every other struct.
   CONTROL CONSTRUCT (4): the population is installed in the tree overwhelmingly by DESIGNATED INITIALIZER and
   in one place by FIELD ASSIGNMENT, so the control uses BOTH, which is what forces both message forms of (3) to
   be present before any sweep result is believed. */
const hookProbe = {
  tags: ["JSFlowControlHooks", "JSTimeTravelHooks"],
  name: "hook-table members",
  _body(h, tag) {
    const m = new RegExp("typedef struct " + tag + "\\s*\\{([\\s\\S]*?)\\}\\s*" + tag).exec(h);
    if (!m) throw new Error(`consumer_probe: the header no longer declares \`typedef struct ${tag}\``);
    return m;
  },
  /* PER TAG, AND THE ASSOCIATION IS KEPT RATHER THAN RE-DERIVED. An earlier form of this returned a flat list
     and recovered `which struct owns this member` by testing the NAME against the struct body — which matched
     words inside that body's COMMENTS, so the control tried to install JSFlowControlHooks members into
     JSTimeTravelHooks and could not compile against the real header. The control REFUSED rather than sweeping,
     which is the guarantee above working; the repair is that a fact derived once is carried rather than asked
     for twice, which is the same rule as stripping the comments in the first place. */
  byTag(h) {
    const out = {};
    for (const tag of this.tags) {
      out[tag] = [];
      const body = this._body(h, tag)[1].replace(/\/\*[\s\S]*?\*\//g, " ");   /* comments FIRST, or they invent */
      for (const st of body.split(";")) {
        if (!st.trim()) continue;
        const fp = /\(\s*\*\s*([A-Za-z_]\w*)\s*\)/.exec(st);                  /* inline function pointer */
        const pl = /\b([A-Za-z_]\w*)\s*$/.exec(st.trim());                    /* typedef'd pointer OR data */
        const n = fp ? fp[1] : (pl ? pl[1] : null);
        if (n) out[tag].push(n);
      }
    }
    return out;
  },
  population(h) { const b = this.byTag(h); return this.tags.flatMap((t) => b[t]); },
  mutate(h, _pop) {
    let out = h;
    for (const tag of this.tags) {
      const m = this._body(out, tag);
      const defines = m[1].split("\n").filter((l) => l.trim().startsWith("#define")).join("\n");
      out = out.slice(0, m.index) + `typedef struct ${tag} {\n${defines}\n` +
            `    int consumer_probe_members_removed;\n} ${tag}` + out.slice(m.index + m[0].length);
    }
    return out;
  },
  /* ONE CONTROL PER CONSTRUCT — see the flow probe's note, which this file demonstrated the hard way. The
     population is installed overwhelmingly by DESIGNATED INITIALIZER and in one place by FIELD ASSIGNMENT, and
     the two produce DIFFERENT diagnostics, so each needs its own control or the pair masks a missing form. */
  _ctl(fn) {
    const byTag = this.byTag(readFileSync(HEADER, "utf8"));
    const L = ['#include "quickjs.h"', "void probe_ctl(void);", "void probe_ctl(void) {"];
    for (const tag of this.tags) L.push(...fn(tag, byTag[tag]));
    L.push("}");
    return L.join("\n");
  },
  controls: [
    { construct: "designated initializer (what almost every install in the tree is)",
      src() { return hookProbe._ctl((tag, ms) =>
        [`    { ${tag} c = {` + ms.map((n) => ` .${n} = 0`).join(",") + ` }; (void)c; }`]); } },
    { construct: "field assignment (what one install in the tree is, and a different diagnostic)",
      src() { return hookProbe._ctl((tag, ms) =>
        [`    { ${tag} b = { 0 };`, ...ms.map((n) => `      b.${n} = 0;`), "      (void)b; }"]); } },
  ],
  forms: [/no member named '([A-Za-z_]\w*)'/g,
          /field designator '([A-Za-z_]\w*)' does not refer to any field/g],
};

/* ARM THE CONTROL, AND REFUSE TO SWEEP UNTIL IT HAS SPOKEN FOR EVERY MEMBER. This is the construction that
   closes (3), (4) and (5) together: a message form the probe does not know, a control written in a construct the
   population does not use, and a channel that fails silently all produce the SAME symptom here — a population
   member the control could not make the compiler name — and all three are a startup THROW rather than a sweep
   that quietly reports the member as having no consumer. */
function armControl(probe, pop, shadowDir, scratch, dev) {
  const armed = [];
  /* FORM ARMING FIRST: a regex that matches nothing the compiler ever says is a form that can never contribute,
     and it fails in the direction that reports a member as unconsumed. Each synthetic case must be NAMED by the
     probe's own forms, with no mutated header involved — the construct alone produces the diagnostic. */
  for (const fp of probe.formProbes || []) {
    const f = join(scratch, `form_${fp.expect}.c`);
    writeFileSync(f, fp.src);
    const r = compile(f, null, dev);
    const got = named(r.err, probe.forms, [fp.expect]);
    if (!got.includes(fp.expect))
      throw new Error(`consumer_probe: no form in this probe matches what the compiler says for the ` +
                      `"${fp.construct}" construct, so that construct contributes NOTHING to the sweep and a ` +
                      `TU that uses it reads as having no reference. What the compiler actually said:\n` +
                      r.err.split("\n").filter((l) => /error:/.test(l)).slice(0, 3).join("\n"));
    armed.push(`FORM "${fp.construct}": matched on a synthetic case (arms the regex, not the population)`);
  }
  for (const [i, c] of probe.controls.entries()) {
    const ctl = join(scratch, `control${i}.c`);
    writeFileSync(ctl, c.src(pop));
    const real = compile(ctl, null, dev);
    if (real.code !== 0)
      throw new Error(`consumer_probe: the "${c.construct}" control does not compile against the REAL header, ` +
                      `so it is not a control for anything. Its own errors:\n` +
                      real.err.split("\n").slice(0, 8).join("\n"));
    const mut = compile(ctl, shadowDir, dev);
    if (mut.code === 0)
      throw new Error(`consumer_probe: the "${c.construct}" control compiled against the MUTATED header, so ` +
                      `the mutation is not reaching it — the sweep would report every member as unconsumed.`);
    const spoke = named(mut.err, probe.forms, pop);
    const silent = pop.filter((n) => !spoke.includes(n));
    if (silent.length)
      throw new Error(`consumer_probe: the "${c.construct}" control could not make the compiler name ` +
                      `${silent.length} of ${pop.length} population members: ${silent.join(" ")}\n` +
                      `  Either a MESSAGE FORM for THIS construct is missing from \`forms\`, or the control is ` +
                      `written in a construct the population does not use. Both read as "no consumer" in a ` +
                      `sweep, which is why this refuses rather than reports. Each construct is armed SEPARATELY ` +
                      `for exactly this reason: a control exercising several at once lets one mask another's ` +
                      `missing form. The raw diagnostics are where the form is:\n` +
                      mut.err.split("\n").filter((l) => /error:/.test(l)).slice(0, 4).join("\n"));
    armed.push(`POPULATION via "${c.construct}": all ${pop.length} named`);
  }
  return armed;
}

function run(probe, dev) {
  const header = readFileSync(HEADER, "utf8");
  const pop = probe.population(header);
  if (!pop.length) throw new Error(`consumer_probe: the ${probe.name} population derived EMPTY — the header's ` +
                                   `shape has moved and an empty population reports no defect for every run.`);
  /* A SCRATCH DIRECTORY NAMED FOR THIS RUN, NOT FOR ITS ROLE. A role-named one (`shadow/`) collided with
     another lane's leftover header and shadowed a SECOND header, producing reproducible failures that read as
     this probe fixing three files — CLAUDE.md §AND-THE-SAME-RULE-IS-OWED-TO-EVERY-SCRATCH-FILE. It is asserted
     to hold exactly one file, because that is the property the collision broke. */
  const scratch = join(process.env.TMPDIR || "/tmp", `consumer_probe.${process.pid}.${probe.name.replace(/\W/g, "_")}`);
  rmSync(scratch, { recursive: true, force: true });
  mkdirSync(join(scratch, "shadow"), { recursive: true });
  const shadowDir = join(scratch, "shadow");
  writeFileSync(join(shadowDir, "quickjs.h"), probe.mutate(header, pop));
  if (execFileSync("sh", ["-c", `find ${shadowDir} -type f | wc -l`], { encoding: "utf8" }).trim() !== "1")
    throw new Error("consumer_probe: the shadow directory holds more than the mutated header");

  /* A REFUSAL DELIBERATELY LEAVES THE SCRATCH DIRECTORY BEHIND: the control and the compiler's own words are
     what a reader needs to find the missing form, and a probe that tidies them away on the one run that failed
     has hidden its own evidence. A successful run removes it. */
  const armed = armControl(probe, pop, shadowDir, scratch, dev);
  const { swept, adjacent } = translationUnits();

  const consumers = new Map(pop.map((n) => [n, []]));
  for (const f of swept)
    for (const n of named(compile(f, shadowDir, dev).err, probe.forms, pop))
      consumers.get(n).push(relative(ROOT, f));

  rmSync(scratch, { recursive: true, force: true });
  return { pop, armed, consumers, swept: swept.length, adjacent };
}

const which = process.argv[2] || "all";
const dev   = process.argv.includes("release") ? "0" : "1";
const probes = { flow: flowProbe, hooks: hookProbe };
const chosen = which === "all" ? Object.entries(probes)
                               : [[which, probes[which] ?? (() => { throw new Error(`consumer_probe: no probe \`${which}\` — flow | hooks | all`); })()]];
let unconsumed = 0;
for (const [key, probe] of chosen) {
  const r = run(probe, dev);
  console.log(`\n=== ${probe.name} (${key}) — APICLIENT_DEV=${dev} ===`);
  console.log(`  population derived from ${relative(ROOT, HEADER)}: ${r.pop.length}`);
  for (const c of r.armed) console.log(`  armed — ${c}`);
  console.log(`  translation units swept: ${r.swept}`);
  console.log(`  NOT swept, and NOT thereby clean — read these by hand (2): ${r.adjacent.join(" ") || "(none)"}`);
  for (const [n, tus] of [...r.consumers].sort())
    console.log(`  ${tus.length ? "  " : "!!"} ${n.padEnd(28)} ${tus.length} ${tus.join(" ")}`);
  const none = [...r.consumers].filter(([, t]) => !t.length).map(([n]) => n);
  unconsumed += none.length;
  if (none.length)
    console.log(`  NO CONSUMER IN ANY SWEPT TU: ${none.join(" ")}\n` +
                `    NOT a work queue. Ask of each, BEFORE proposing anything: does a residual, a DFAIL, a\n` +
                `    header sentence or CLAUDE.md name it as what a next diff builds? A deliberate seam awaiting\n` +
                `    a consumer reads identically here, and deleting one destroys work somebody reasoned out.\n` +
                `    Then check the hand-read files above, which this probe cannot see.`);
}
console.log(`\nmembers with no consumer in any swept TU: ${unconsumed}` +
            (unconsumed ? "  (each owes the second question above)" : "  (nothing convicted)"));
process.exit(0);   /* a finding is a REPORT and not a build failure: this probe cannot tell a dead entry from a
                      deliberate seam, and a gate that cannot make that distinction would be red on every run. */
