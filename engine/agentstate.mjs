/* WHICH AGENT-LIFETIME SLOTS core/agent_state.h HAS NEVER BEEN TOLD ABOUT.
 *
 * That registry's whole value is the assert at the end of the release column: every DECLARED slot is back at
 * its pre-init value, and a release that frees a value and forgets the handle crashes there naming the
 * component and the state. It can only ever ask about slots it was told about. A slot that is minted and
 * never declared is not weakly checked, it is OUTSIDE THE QUESTION -- no release could ever be caught
 * forgetting it, at any revision, by any run this project has ever made. That population is what this prints.
 *
 * ONE ROW IS ONE (FILE, IDENTIFIER) PAIR. It is not an allocation, not an array element, and not a
 * declaration call. A component holding N slots in an array and minting or resetting them in a loop is ONE
 * row here. THIS IS PRINTED BESIDE THE COUNT AND NOT ONLY HERE, because the number this file emits sits next
 * to core/platform.c's `minted == declared` identity whose row is an ALLOCATION -- so the two may not be
 * differenced, and a reader who subtracts them is not measuring a gap, they are committing the category error
 * CLAUDE.md names: two instruments disagreeing because their rows count different things rather than because
 * either is blind. The two answer different halves of one question and neither is the other's floor.
 *
 * IT IS A STATIC SWEEP AND THEREFORE A FLOOR, AND THE SPELLINGS IT SEARCHED ARE PART OF ITS OUTPUT. It reads
 * source text, so a mint written any way other than the forms it names is invisible to it -- a helper that
 * wraps JS_NewClassID, an id assigned through a pointer, a slot handed out by a component's own factory.
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
 *   ALLOCATOR'S, so it counts a mint written in any spelling at all, including the ones this file names as
 *   invisible to it. What this sweep sees and the run does not is an ADDRESS -- the run has a count and a
 *   window, and the file and line of an offending mint are here. AND THE RUN'S IDENTITY IS OVER CLASS IDS
 *   ONLY: agent_state_class_id_count is SLOT_CLASS plus SLOT_REALM by its own header comment, so the id,
 *   flag, atom, value and ptr kinds have no runtime bracket at all and the RELEASE-RESET channel below is
 *   the only thing in this tree that speaks about them.
 *   THE ROOT BEYOND BOTH is a mint that DECLARES -- one door taking the slot's address and the component's
 *   row, so that an undeclared class id is unconstructible rather than reported by either. That is a
 *   signature change at every mint in the tree, which is what the identity exists to force.
 *
 * THE THREE CHANNELS, AND WHY THE THIRD IS DERIVED RATHER THAN A THIRD HARDCODED SPELLING. Two channels ask
 * `was this MINTED and never declared`, and a mint has a spelling only for the two kinds that have a minting
 * call -- a class id and a realm slot. An id, a flag, an atom, a value and a pointer are BORN BY ASSIGNMENT
 * and have no call to grep for, so no list of mint spellings can ever reach them, at any revision. Widening
 * this file by typing more spellings in would have been the hand-kept list CLAUDE.md forbids: a second copy
 * of a fact core/agent_state.h already owns, drifting the day a kind is added.
 *   WHAT THE HEADER ACTUALLY OWNS IS THE OTHER SIDE OF THE CONTRACT, and it is machine-readable: each
 *   declaring entry carries its own `pre-init:` annotation, and its signature names the C TYPE of the slot it
 *   accepts. The header's closing rule turns that into a population -- `A slot whose value is legitimately
 *   non-pre-init at the release is NOT agent state in this header's sense and must not be declared`. Read the
 *   other way round, A SLOT ITS OWN RELEASE PUTS BACK TO A PRE-INIT VALUE IS AGENT STATE BY THE COMPONENT'S
 *   OWN ACT, whatever its kind and however it was born. That is the third channel, and every spelling it
 *   searches for is read out of the header rather than written here.
 *   THE EVIDENCE IS THE COMPONENT'S OWN RESET LINE, WHICH IS WHY THIS CHANNEL IS NOT THE FALSE-ACCUSATION
 *   MACHINE A SWEEP OVER EVERY FILE STATIC WOULD BE. A process-lifetime cache is legitimately non-pre-init at
 *   the release -- that is what makes it process-lifetime -- so it is never reset and never appears here.
 *   MEASURED rather than argued, at the revision this was written: core/url/origin.c, whose tables are the
 *   stock example of such a cache, has NO release and NO declaration and contributes nothing to this channel.
 *   core/idl_args.c DOES appear, and reading it refutes the guess that it is the counterexample: idl_args_free
 *   and idl_args_pool_free are per-agent releases the hosts call at teardown, and core/rendering/
 *   animation_frame.c's own comment records that the pool `restarts at 0` for the next agent -- so its
 *   counters are agent state and the rows are true. A sample of twelve rows taken every twenty-fifth from the
 *   sorted output was read by hand and twelve of twelve were file statics whose declared initialiser is
 *   EXACTLY the header's pre-init value for their type. Zero false accusations found.
 *   THE PRICE WAS PAID IN A DIFFERENT COIN AND IT IS BANDED RATHER THAN HIDDEN: some slots this channel finds
 *   are UNDECLARABLE -- their C type is one no declaring entry accepts (`long`, `uint32_t`, `size_t`), so the
 *   accusation is true and nobody can act on it without a new kind in a header this file does not own. That is
 *   a band and not a finding, because CLAUDE.md rates a count that mixes `I found a defect` with `nothing can
 *   be done here` as the three-states-behind-one-answer shape.
 *
 * WHAT THE BANDS MEAN, AND WHY THEY ARE NOT ONE NUMBER. CLAUDE.md: N sites spelling one question wrong
 * are not N defects, and the discriminator is what stands UNDER each one.
 *   declared              -- the registry can ask. Nothing owed.
 *   UNDECLARED, hand-reset -- the slot IS put back, by a line somebody wrote and somebody must keep. Correct
 *                            today and unfalsifiable: nothing can report it if a later diff drops the line.
 *   UNDECLARED, NOT reset  -- carried into the next agent. For a class id that is not a style question:
 *                            JS_NewClassID in this fork opens `if (class_id == 0)` and otherwise RETURNS THE
 *                            NUMBER IT IS HANDED, so a carried id is never re-minted -- it names a class in a
 *                            runtime that is gone, while the new runtime's allocator restarts at
 *                            JS_CLASS_INIT_COUNT and hands the same number to somebody else.
 *   reset: UNDECLARED      -- the hand-reset band for a kind that has no mint spelling. Same fact, same
 *                            fragility, reached from the release instead of from the mint.
 *   reset: UNDECLARABLE    -- true, and not actionable here. See above.
 *   reset: declaration unread -- the row is REAL and its C type could not be parsed, so the kind question is
 *                            unanswered for it. Stated rather than dropped: a row absent from every band is
 *                            the under-count CLAUDE.md says nothing announces.
 * A row's band is a fact about the tree. Whether it is a DEFECT is a question about that component, and this
 * file does not answer it -- a slot may be reset by a cascade this sweep cannot see, or belong to a component
 * that reasons about it at its own site. Read the row before repairing it.
 *
 * NAMED RESIDUAL -- A SLOT WHOSE PRE-INIT VALUE THE HEADER STATES IN PROSE IS INVISIBLE TO THIS CHANNEL.
 *   WHAT IS NOT COVERED: the pre-init spellings are read from the `pre-init:` annotation each declaring entry
 *     carries, and a text sweep can only use one that is a C TOKEN -- an integer literal or an identifier.
 *     Two kinds fail that, for two different reasons that are the same gap: agent_state_ptr_at carries NO
 *     annotation at all (its pre-init is argued in the prose of the block comment above it, `compares the
 *     BYTES of a null pointer`), and agent_state_zeroed_at carries one whose value is the phrase `the zero
 *     bytes`. So neither a pointer put back to NULL nor an aggregate memset raises a row. HARDCODING NULL
 *     HERE IS THE ONE THING THAT MUST NOT BE DONE: it is exactly the second copy of a header fact this
 *     channel exists to avoid, and it would be the only spelling in this file not derived from the
 *     declaration the engine obeys.
 *   HOW ITS ABSENCE WOULD SHOW: the run prints the slot C types it read from the header, and `void` -- the
 *     type both of those kinds take -- is among them, while no row it bands is ever a pointer. A channel that
 *     names a type in its own banner and never bands one is the observation, and it needs no particular
 *     component to exhibit it. The run also prints by name every kind whose annotation it could not use, so
 *     the size of this hole is read off the output rather than believed from here.
 *   WHAT THE NEXT DIFF BUILDS: a `pre-init:` annotation whose value is a C token, in the trailing form the
 *     other entries already use -- `NULL` for agent_state_ptr_at. Nothing in this file changes; the
 *     derivation picks it up. A kind whose pre-init genuinely is not one token stays listed as unusable,
 *     which is the honest state and not a gap. The header is another lane's file, which is why this is a
 *     residual and not a diff.
 *   WHAT IT COSTS, MEASURED RATHER THAN ESTIMATED, at the revision this was written and as a record of a
 *     decision rather than a census: adding NULL to the alternation by hand moved `reset: UNDECLARED` by 58
 *     rows and `reset: declared` by 27. That is the size of what the missing annotation hides, and it is the
 *     reason this is a named residual rather than a shrug.
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

/* THE TWO MINT SPELLINGS THIS SWEEP CAN SEE, named in the output because they bound it. They are the whole of
   the MINT side and they always will be: they are the two kinds that HAVE a minting call. Everything else is
   born by assignment and is reached by the release-reset channel below instead. */
/* THE CAPTURE ENDS AT THE ARGUMENT'S END, WHICH IS NOT PEDANTRY: `&f->class_id`, `&rec->slot` and `*slot =` are mints
   into a STRUCT MEMBER, and a pattern that stops at the first identifier captures the POINTER instead. That
   name then has to be tested for being a file static, and the test is a regex over `static ... \bname\b`,
   which a function PARAMETER of the same name satisfies -- so the row is admitted and reads as an undeclared
   slot in a file that has none. Measured once, on `JS_NewClassID(rt, &f->class_id)` against a `static JSValue
   ait_fulfil_result(..., const IdlAsyncIface *f, ...)` eight hundred lines away. It is the accusing direction
   CLAUDE.md says to suspect hardest, so the trailing `\s*\)` and `\s*[,)]` are load-bearing. */
/* AN ARRAY SUBSCRIPT IS ADMITTED ON ALL THREE PATTERNS HERE AND ON `DECLARED` BELOW, AND THE REASON IS THAT
   THE THREE MUST TAKE THE SAME INPUTS. A component that holds N slots of one kind holds them in an array and
   mints them in a loop -- `g_fn_slot[i] = realm_value_declare(...)`, `JS_NewClassID(rt, &g_class[k])` -- and
   the realm mint below admitted that from the start while the class mint and the declared check did not.
   CLAUDE.md names the shape: an instrument's confirming path and its refuting path can admit different
   populations, and where the ACCUSING one is wider it certifies exactly what the other cannot refute. Here it
   was wider on the REALM channel, so a component that declared an array of realm slots correctly, in the only
   way C affords, was banded UNDECLARED and no spelling of a correct declaration could clear it -- the accusing
   direction, manufactured entirely by this file. It was LATENT rather than harmless: at the revision this was
   written, no file in the tree had yet declared a realm slot through `&x[i]`, so nothing had exercised it, and
   the first diff that did met seven rows it could not clear.
   THE CLASS CHANNEL WAS BLIND ON BOTH HALVES AND SO WAS QUIET, which is the other direction and is why it had
   to be fixed in the same breath rather than left: `JS_NewClassID(rt, &g_class[k])` matched neither the mint
   nor the declaration, so those slots appeared in NO BAND AT ALL -- not declared, not accused, absent from
   every total, which is the under-count CLAUDE.md rates as the one nothing announces.
   THE PRICE OF THE WIDENING WAS MEASURED AND NOT PREDICTED, AND THE PREDICTION WAS WRONG, which is recorded
   rather than quietly corrected because the wrong reasoning is what a reader re-derives. It said: two sites
   tree-wide, core/css/css_math_value.c's `g_class` and core/dom/abstract_range.c's `g_bounds_classes`, both
   already declared, so nothing would be accused. It was reached by grepping for array-element DECLARATIONS
   and assuming the mint side saw the same population -- the same two-halves confusion this paragraph is about,
   committed while writing it. `g_bounds_classes` is never handed to JS_NewClassID at all (it is filled by
   assignment from ids other components minted), so it stays invisible to the mint side and moved nothing; and
   the widening surfaced a site no grep for declarations could have found. RUN BOTH INSTRUMENT VERSIONS AGAINST
   ONE UNCHANGED REVISION, which is the confound-free form and needs no copy of the tree: `class: declared`
   98 -> 99 and `class: UNDECLARED, NOT reset` 82 -> 83, with the realm bands unmoved. The cleared row is
   css_math_value's; the new accusation is core/html/html_element.c's `g_iface_class`, a real array of class
   ids minted in a loop and declared to nobody, which this sweep had been structurally unable to see. One true
   accusation bought, none false, one row cleared -- which is the trade a widening has to show, and the point
   is that it was SHOWN.
   THE GRANULARITY IS THE IDENTIFIER AND NOT THE ELEMENT, on both sides, which is what makes this symmetric
   rather than generous: `seen` dedups a mint to one row per identifier, so a declaration of one element reads
   as a declaration of the array exactly as a mint of one element reads as a mint of the array. A component
   that declares only SOME elements of an array is a state this sweep cannot see and never could; the run-side
   instrument core/platform.c brackets the declare column with counts per SLOT and is what catches it.
   RETIREMENT: this record goes when the mint and the declaration are ONE call -- core/agent_state.h's own
   closing note names that root, a door taking the slot's address and the component's row -- because an
   undeclared class id is then unconstructible and no pattern here has two halves to disagree. */
const SUBSCRIPT = "(?:\\s*\\[[^\\]]*\\])?";
const MINTS = [
  ["class", new RegExp(`JS_NewClassID\\s*\\(\\s*[^,]+,\\s*&\\s*([A-Za-z_]\\w*)${SUBSCRIPT}\\s*\\)`, "g")],
  ["realm", new RegExp(`(?<![>.*])\\b([A-Za-z_]\\w*)${SUBSCRIPT}\\s*=\\s*realm_value_declare\\s*\\(`, "g")],
];
/* EVERY FACT THIS FILE HOLDS ABOUT THE REGISTRY IS READ OUT OF core/agent_state.h AND NOT RESTATED HERE.
   CLAUDE.md: an auditor derives the rule it checks from the code that owns it, because a restated rule is a
   SECOND COPY and the one that drifts is the copy nobody runs against reality. Three tables come out of that
   header and each would have been a hand-kept list:
     KINDS  -- which entries DECLARE a slot. A kind added to the header and not to an alternation typed out
               here would move every slot declared through it OUT of `declared` and into an accusation band --
               manufactured entirely by this file, against the components that had just been routed correctly.
               The entries that declare are exactly the ones taking a `what`; agent_state_undo_at and
               agent_state_reached_at take (component, file, line) and are not declarations.
     PREINIT -- the value each kind's release must put the slot back to, read from the `pre-init:` annotation
               each declaring entry carries. This REPLACED a hardcoded `-1|0|JS_INVALID_CLASS_ID`, and the
               A/B that CLAUDE.md requires of a widening was run against ONE unchanged revision with only this
               alternation differing: every band of both mint channels was IDENTICAL, to the row. The derived
               set is WIDER (it adds JS_ATOM_NULL and JS_UNDEFINED), so that identity is a fact about the
               population and not about the patterns -- no class id or realm slot in the tree is put back with
               an atom's or a value's spelling -- and it is what licenses the third channel to share one
               alternation with the first two rather than growing a second that could disagree.
     SLOTTYPES -- the C type each declaring entry accepts, read from its signature. A file static whose type
               is none of them cannot be declared at all without a new kind, which is a BAND and not a
               finding. `void` here means any pointer, which is how agent_state_ptr_at takes its slot. */
const HEADER = "engine/host/browser/core/agent_state.h";
const HDR = read(HEADER);
const KINDS = [...HDR.matchAll(/\bagent_state_(\w+)_at\s*\(\s*const char \*component,[^;]*?const char \*what\b/g)]
  .map((m) => m[1]);
/* A `pre-init:` ANNOTATION IS USED ONLY WHERE ITS VALUE IS A C TOKEN, AND THE REST ARE NAMED IN THE OUTPUT.
   An annotation may state a value this sweep cannot match -- agent_state_zeroed_at's is the phrase `the zero
   bytes`, which is a correct description of an aggregate's pre-init image and not a spelling any assignment
   in the tree carries. Admitting it would put a three-word phrase in the alternation: harmless today, because
   nothing matches it, and exactly the shape that makes a later annotation silently widen this file's accusing
   pattern. Rejecting it silently would be worse, because the kinds it drops are then absent from the output
   as well as from the bands. So it is FILTERED and the rejects are PRINTED, which is what makes the residual
   in the banner a number a reader takes off the run rather than a claim they take from here. */
const PREANN = [...HDR.matchAll(/\bagent_state_(\w+)_at\s*\([^;]*?\);\s*\/\*\s*pre-init:\s*([^*]+?)\s*\*\//g)]
  .map((m) => ({ kind: m[1], v: m[2] }));
const CTOKEN = /^(?:-?\d+|[A-Za-z_]\w*)$/;
const PREINIT = [...new Set(PREANN.filter((a) => CTOKEN.test(a.v)).map((a) => a.v))];
const PREPROSE = PREANN.filter((a) => !CTOKEN.test(a.v));
const UNANNOTATED = KINDS.filter((k) => !PREANN.some((a) => a.kind === k));
const SLOTTYPES = [...new Set([...HDR.matchAll(/\bagent_state_(\w+)_at\s*\(\s*const char \*component,\s*const\s+([A-Za-z_]\w*)\s*\*\s*slot/g)]
  .map((m) => m[2]))];
for (const [what, list] of [["declaring entry", KINDS], ["`pre-init:` annotation", PREINIT], ["slot C type", SLOTTYPES]])
  if (!list.length)
    throw new Error(`agentstate: no ${what} was found in ${HEADER}. Every band this sweep prints is derived `
                  + `from that header rather than restated here, so an empty table is a statement about this `
                  + `file and not about the engine -- it would report every declared slot in the tree as `
                  + `undeclared, or band every real row as undeclarable. The entry shape changed, or the path is wrong.`);
const PREALT = PREINIT.map((v) => v.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join("|");
const DECLARED = new RegExp(`agent_state_(?:${KINDS.join("|")})(?:_at)?\\s*\\([^;]*?&\\s*([A-Za-z_]\\w*)${SUBSCRIPT}\\s*[,)]`, "gs");
/* A RESET, AND THE `for (` GUARD IS A LATENT ONE WITH A MEASURED-EMPTY POPULATION. `for (g_cursor = 0; ...)`
   is an assignment of a pre-init value to a file static inside a release and is NOT a reset -- the loop
   leaves the variable at its bound, not at 0 -- so admitting it would be an accusation this file manufactured.
   Measured at the revision this was written: zero of the 612 raw matches in the tree were for-inits, so the
   guard changes no row today. It is written anyway for the reason the array-subscript record above gives:
   that hazard was LATENT rather than harmless, and the first diff that exercised it met rows it could not
   clear. The `(?<![>.*])` lookbehind is the same protection the realm mint carries -- `rec->g_x = 0;` is a
   write to a struct member and not to the static that shares its name. */
const RESET = new RegExp(`(?<![>.*])\\b([A-Za-z_]\\w*)${SUBSCRIPT}\\s*=\\s*(?:${PREALT})\\s*;`, "g");

/* Top-level function bodies, by brace balance, WITH THE OFFSET THEY START AT so a row found inside one can be
   given a line in the FILE rather than in the body.
   THIS USED TO SAY `a miss here can only move a row into the louder band, never out of it`, AND THAT IS NOW
   TRUE OF THE MINT CHANNELS ONLY -- it is kept rather than deleted because it is the reading a reader
   re-derives from the first use they meet. For a mint channel a release body is a BAND REFINEMENT: the row
   exists either way and a missed body only makes it read `NOT reset`. For the release-reset channel the
   release body IS THE POPULATION, so a function this misses drops its rows out of every band, which is the
   silent direction. The filter is therefore the floor of that channel and is named in the output. */
function bodies(s) {
  const out = new Map();
  for (const m of s.matchAll(/^(?:[A-Za-z_][\w \t*]*?)\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{/gm)) {
    let d = 0, i = m.index + m[0].length - 1;
    for (let j = i; j < s.length; j++) {
      if (s[j] === "{") d++;
      else if (s[j] === "}" && --d === 0) { out.set(m[1], { b: s.slice(i, j), at: i }); break; }
    }
  }
  return out;
}

/* THE DECLARATION STATEMENT THAT DECLARES `id`, AND THE C TYPE AT ITS HEAD. A comma-separated list carries the
   type ONLY on the first declarator -- `static int g_a = -1, g_b = -1;` -- so reading backwards from the name
   gives `-1,` as the type of the second one. That is the same defect the `(` exclusion below was written
   against, one clause over, and it is the accusing direction: a bogus type reads as UNDECLARABLE, which says
   a correct row cannot be acted on. So the statement is matched whole and the type taken from its HEAD.
   THE EXCLUDED CHARACTER IS `(` AND NOT `{`: an array initialiser (`static int g_id_set[4] = { -1, -1 };`) is
   an ordinary declaration and excluding `{` dropped nine real rows into an unbanded silence. What `(` still
   excludes is a function declaration -- and an X-MACRO initialiser with it, which is the one shape left that
   this cannot read; those rows are BANDED as `declaration unread` rather than dropped. */
function declOf(s, id) {
  for (const m of s.matchAll(/^[ \t]*static\s+[^;(]*;/gm)) {
    if (!new RegExp(`\\b${id}\\b`).test(m[0])) continue;
    const body = m[0].replace(/^[ \t]*static\s+/, "");
    const t = /^([\s\S]*?)\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|,|;)/.exec(body);
    if (!t) continue;
    return { type: t[1].trim(), ptr: new RegExp(`\\*[\\s\\w]*\\b${id}\\b`).test(body) || /\*\s*$/.test(t[1]) };
  }
  return null;
}

const rows = [], tally = new Map();
const add = (band, p, id, line) => { tally.set(band, (tally.get(band) ?? 0) + 1); rows.push({ band, p, id, line }); };
/* A FILE STATIC AND NOT A LOCAL OR A PARAMETER -- the registry takes an address that outlives a call. It is
   asked by all three channels, which is what keeps them taking the same inputs.
   THE EXCLUDED CHARACTER IS `(` AND IT USED TO BE `=`, which dropped every member but the FIRST of a
   comma-separated declaration list: `static int g_stepid = -1, g_driver_slot = -1;` puts an `=` before
   the second name, so the test failed for it and the row left this sweep entirely. That is the UNDER-
   counting direction, which nothing announces -- the slot is simply absent from every band. Measured at
   8de85780 on the realm channel alone: `rendering.c`'s g_driver_slot and `remote_op.c`'s g_apply_slot,
   both declared, both invisible. `(` keeps the protection the `=` was really buying: a static FUNCTION whose
   PARAMETER shares the name cannot match, because a parameter is always preceded by the function's open paren.
   IT IS LOAD-BEARING FOR THE RESET CHANNEL IN A WAY IT NEVER WAS FOR THE MINTS, and measured: without it that
   channel admitted 150 rows named `i`, every one a `for (i = 0; ...)` loop counter in a release. A mint
   spelling is its own evidence that the name is a slot; an assignment is not, so this test is the only thing
   standing between the reset channel and every local in the tree. */
const isStatic = (s, id) => new RegExp(`^\\s*static\\s[^;(]*\\b${id}\\b`, "m").test(s);

for (const p of files) {
  const s = read(p);
  if (!s) continue;
  const declared = new Set([...s.matchAll(DECLARED)].map((m) => m[1]));
  const rel = [...bodies(s)].filter(([k]) => k.includes("_free") || k.includes("_release"));
  const minted = new Set();
  for (const [kind, rx] of MINTS) {
    const seen = new Set();
    for (const m of s.matchAll(new RegExp(rx.source, "g"))) {
      const id = m[1];
      if (seen.has(id)) continue;
      if (!isStatic(s, id)) continue;
      seen.add(id);
      minted.add(id);
      const line = s.slice(0, m.index).split("\n").length;
      if (declared.has(id)) { add(`${kind}: declared`, p, id, line); continue; }
      /* THE PRE-INIT SPELLINGS ARE THE HEADER'S, AND `JS_INVALID_CLASS_ID` IS ONE OF THEM. A realm slot's
         C type is JSClassID, so its pre-declaration value is quickjs's own reserved 0 and components spell it
         by that name rather than as a digit. Matching only `-1|0` would have moved every converted slot that
         IS hand-reset into `NOT reset`, which is the louder band and the accusing direction -- a line somebody
         wrote and keeps, reported as a line nobody wrote. It stays a list of SPELLINGS and not a value test
         because this is a text sweep; that is the floor the header already states. */
      const R = new RegExp(`\\b${id}${SUBSCRIPT}\\s*=\\s*(?:${PREALT})\\b`);
      add(`${kind}: UNDECLARED, ${rel.some(([, v]) => R.test(v.b)) ? "hand-reset" : "NOT reset"}`, p, id, line);
    }
  }
  /* THE RELEASE-RESET CHANNEL. A mint is excluded here so the channels PARTITION rather than double-count:
     a class id that is both minted and reset is one row, in the channel that can say more about it. */
  const seen = new Set();
  for (const [, v] of rel) {
    for (const m of v.b.matchAll(new RegExp(RESET.source, "g"))) {
      const id = m[1];
      if (seen.has(id) || minted.has(id)) continue;
      if (/\bfor\s*\(\s*$/.test(v.b.slice(Math.max(0, m.index - 8), m.index))) continue;
      if (!isStatic(s, id)) continue;
      seen.add(id);
      const line = s.slice(0, v.at + m.index).split("\n").length;
      if (declared.has(id)) { add("reset: declared", p, id, line); continue; }
      const d = declOf(s, id);
      if (!d) { add("reset: UNDECLARED, declaration unread", p, id, line); continue; }
      const base = d.type.replace(/\b(const|volatile|signed)\b/g, "").replace(/\*/g, "").trim().split(/\s+/).pop() ?? "";
      add(d.ptr || SLOTTYPES.includes(base) ? "reset: UNDECLARED"
                                            : `reset: UNDECLARABLE, no kind takes ${base || "this C type"}`, p, id, line);
    }
  }
}

if (!rows.length)
  throw new Error("agentstate: this sweep found no mint and no reset at all, which is not a clean tree -- either "
                + "the two spellings in MINTS have been renamed, or the header's pre-init annotations have, or "
                + "the path prefix is wrong. A zero here is a statement about this file and not about the engine.");

console.log(`agent-state coverage over ${files.length} files at ${REV ?? "the WORKING TREE (moves under the scan)"}`);
console.log(`ONE ROW IS ONE (FILE, IDENTIFIER) PAIR -- never an allocation and never an array element, so this`);
console.log(`  may not be differenced against core/platform.c's minted==declared identity, whose row is a CLASS`);
console.log(`  ID ALLOCATION. Two instruments, two denominators, one question.`);
console.log(`mint spellings searched: ${MINTS.map(([k]) => k).join(", ")} -- anything else MINTED is invisible here`);
console.log(`release bodies searched: a top-level name containing _free or _release -- the reset channel's floor`);
console.log(`declaring kinds, read from ${HEADER}: ${KINDS.join(", ")}`);
console.log(`pre-init spellings, read from the same header: ${PREINIT.join(" | ")}`);
console.log(`  kinds whose pre-init this sweep CANNOT match, so whose resets raise no row: `
          + `${[...PREPROSE.map((a) => `${a.kind} (stated as "${a.v}")`), ...UNANNOTATED.map((k) => `${k} (no annotation)`)].join(", ") || "none"}`);
console.log(`slot C types, read from the same header: ${SLOTTYPES.join(", ")} (void = any pointer)`);
for (const k of [...tally.keys()].sort()) console.log(`  ${String(tally.get(k)).padStart(4)}  ${k}`);
for (const r of rows.sort((a, b) => (a.band + a.p).localeCompare(b.band + b.p)))
  if (/UNDECLAR/.test(r.band) && (!BAND || r.band.includes(BAND)))
    console.log(`${r.band.padEnd(44)} ${r.p}:${r.line}  ${r.id}`);
