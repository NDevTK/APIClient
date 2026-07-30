/* Build-time RATCHET for the recognizer ban (CLAUDE.md §C-stack).
 *
 * The ban was written down and then violated four times in one session — recognizer -> "capability check" ->
 * magic-indexed table -> a `||` in a call gate — each time with the rule already in the file. A rule that only
 * works if someone re-reads it is exactly what CLAUDE.md rejects elsewhere ("Comments are not valid for follow ups
 * it must crash or be built"). So this is BUILT, not written: the count may only ever go DOWN.
 *
 * A recognizer is a per-builtin predicate deciding which builtins get the suspending path. It can only exist while
 * a legacy JS_Call-loop body survives for it to choose against, so each conversion (declare the capability at the
 * definition -> DELETE the legacy body -> the detector has nothing to choose) removes exactly one and lowers the
 * ceiling here. Raising it is not allowed: that means a legacy twin was reintroduced or a detector came back under
 * a new name.
 *
 * The declaration for a CONSUMING builtin is JS_CFUNC_CONSUME_DEF(name, length, ITERCONS_*): cproto
 * JS_CFUNC_consume with the sink in magic, read by tramp_consume_sink_of — the same shape JS_CFUNC_STEP_DEF
 * already had for step machines. Object.fromEntries was the first: its C body was ALREADY a DFAIL, which is the
 * tell that a recognizer has become pure residue — it was choosing against something that no longer exists.
 * Every other consume recognizer whose body is likewise a DFAIL can move the same way, one at a time.
 *
 * 13 -> 7 so far: Object.fromEntries; Array.from + Math.sumPrecise; Iterator.from; %TypedArray%.from + .of;
 * Iterator.prototype toArray/forEach/reduce/some/every/find; Set.prototype union/symmetricDifference/isSupersetOf.
 * The last two show that a walk can take a PARAMETER — the tag is a base plus the kind, the way a STEPDEF id
 * names one machine among many. That is a builtin declaring itself and its argument, not a table of who is special.
 * Array.from and Math.sumPrecise show the general case: a recognizer whose body is NOT yet a
 * bare DFAIL is still retirable when everything it declines is pure VALIDATION rather than iteration. Array.from
 * declined argc 0 and a present-but-uncallable mapfn; both are spec steps with no user code in them, so they moved
 * into the machine's prologue and the acquire, and the body became a DFAIL like the rest. What a recognizer may
 * NOT do is decline a case whose ALGORITHM still lives in C — that is the legacy twin the ban is about.
 */
import { readFileSync } from 'node:fs';

/* 6 -> 9, and UP is right here, for the reason the [[IsExtensible]] count below went up: the number was WRONG.
 * This matched `^static bool tramp_can_call_...`, so THREE predicates of exactly the shape it exists to count
 * were invisible because they are spelled `static inline`, and a fourth (gen_create) was counted only because it
 * happens to carry a non-inline forward declaration. A gate a storage class evades is a gate that flatters
 * itself — the same sentence this file already had to write about a rename. It now matches either spelling and
 * requires the open paren, so a declaration cannot be counted in place of a definition.
 *
 * What the three newly-visible ones are, and they are not all alike:
 *   the three body-entry ones  ROUTING. With plain they form tramp_body_entry, the ONE body-entry convergence
 *                               point, whose four arms are four different entry ALGORITHMS (plain frame / async
 *                               frame / generator create / async-generator create). Deleting anything would not
 *                               make TBE_GEN unnecessary, which is this file's own test for routing. They are
 *                               tramp_body_is_plain/_async/_gen/_agen now, out of this family by NAME — done as
 *                               its own deliberate act, after the measurement, not in the same breath as
 *                               discovering them. A rename that lowers a ratchet is exactly what this file
 *                               distrusts, so the claim it rests on is PINNED below rather than asserted: the
 *                               four are used at six sites and nowhere else. 9 -> 5.
 *   tramp_can_call              ROUTING now, and it was not when this entry was written. It was also a
 *                               recognizer at the TWO promise-job sites, measured rather than argued:
 *                               `Promise.resolve(1).then(loops.bind(null))` aborted with no flow base, because
 *                               js_promise_reaction_job ran a plain bytecode handler as a flow and dropped a
 *                               BOUND, proxied, C or step-machine one into the JS_Call beneath it;
 *                               PromiseResolveThenableJob asked the same of a thenable's `.then`. Both are one
 *                               CALL-ROOT flow now, whose base dispatches through the convergence point, so
 *                               neither asks. What is left of tramp_can_call is tramp_body_entry's TBE_PLAIN arm
 *                               and two DCHECKs, which assert rather than select.
 *
 * The two CONSUMING CONSTRUCTORS went next, and they were measured first: neither hid a live abort — every
 * construction shape already routed — so this was a declaration exercise, not a bug hunt, and it is recorded
 * that way rather than dressed up. They now declare their walk at their registration and the construct
 * convergence point asks ONE question instead of comparing the callee against two C function addresses in a
 * chain, where a new consuming constructor was a new LINK rather than a new declaration.
 *
 * The declaration is a FIELD on the function record (u.cfunc.consume_sink), not `magic`, and that is forced
 * rather than chosen: a constructor's magic is already structural — js_map_constructor's carries
 * MAGIC_SET/MAGIC_WEAK and doubles as a class offset, a TypedArray's IS the class id — and both bodies are also
 * called straight from C with a raw kind, so a sink cannot ride there. It is not a pointer in JSCFunctionType
 * either; that union holds function pointers, and data in it is the strict-aliasing violation that segfaults at
 * -O1. The field costs nothing: the struct is 20 bytes inside a 24-byte union. Base-plus-kind carries the
 * argument the walk needs, the way ITERCONS_SETOP_BASE and a STEPDEF id already do.
 *
 * What each arm still asks is about the ARGUMENT, never the callee: 24.1.1.1 step 2 and 23.2.5.1 step 6.a select
 * a DIFFERENT algorithm for a nullish or non-object source, one that iterates nothing. 5 -> 3.
 *
 * ZERO. The three promise ones went last, and the entry above claiming they were INHERENT was wrong.
 *
 * That entry said a promise-creating builtin which must reject-and-yield on an abrupt callback needs a dedicated
 * exception arm (CONT_PROMISE_EXEC / _ALL / _TRY) the generic CONT_STEP teardown cannot express, "so it cannot
 * route via the convergence point — the recognizer is inherent". The first half is true and the conclusion does
 * not follow: a bespoke CONT kind is a fact about the MACHINE, and a recognizer is about how the machine is
 * FOUND. The two are independent, which JS_CFUNC_ITERDRIVE_DEF had already shown — that cproto exists precisely
 * because the iterator drive's delivery has modes no step machine can express, and it is still a declaration.
 *
 * All three C bodies were residue: js_promise_try a bare DFAIL, the other two with no algorithm left. So the
 * address comparisons were choosing against nothing, which this file calls the state in which a recognizer is
 * pure residue — and says to delete. They declare themselves now (NATIVE_PROMISE_TRY / _EXEC / _ALL_BASE+magic)
 * on the u.cfunc field the consuming constructors added, whose meaning widened with them: it says WHICH MACHINE
 * drives this builtin, and a walk was only the first kind of answer it had to give.
 *
 * What each arm still asks is about the RECEIVER or the ARGUMENT, never the callee: 27.2.3.1 step 2's
 * non-callable executor and NewPromiseCapability's non-object constructor are different algorithms that iterate
 * nothing. 3 -> 0.
 *
 * The ceiling counts tramp_can_call* recognizers. It goes DOWN whenever a conversion deletes a legacy JS_Call-LOOP
 * body (the debt this ratchet exists to retire). It rises for EXACTLY ONE reason: a genuinely-new tramp-native
 * builtin whose semantics REQUIRE a custom CONT kind — a promise-creating builtin that must reject-and-YIELD on an
 * abrupt user callback (return a rejected promise, never raise). That reject-and-yield needs a dedicated
 * exception arm (CONT_PROMISE_EXEC / _ALL / _TRY), which the generic CONT_STEP teardown cannot express, so it
 * cannot route via the convergence point — the recognizer is inherent, not a drift-detector choosing against a
 * deletable body. 12 -> 13 for that one builtin (Promise.try, ES2025). A rise for any OTHER reason (a
 * re-introduced drift-detector, a legacy twin) remains banned.
 *
 * WHAT THAT ENTRY USED TO CLAIM, and why it was wrong: "bytecode fn -> tramp, C/bound fn -> the C path (correct,
 * no loop)". A BOUND function's target is bytecode and loops; a PROXY's apply trap is a function and loops; a
 * GENERATOR fn creates a coroutine. All three reached js_promise_try's JS_Call and aborted (two @WHY
 * preempt-in-a-non-coroutine, one drive-to-completion). "It has no preemptible body" is a claim about the CALLEE
 * KIND, which is exactly the question a recognizer is banned from answering — the convergence point owns it. The
 * receiver and arity conditions were pure validation and moved into the machine's prologue, js_promise_try is a
 * DFAIL, and what is left asks only WHICH machine, at the one place that asks it. */
/* tramp_can_call_iter_helper, and a claim in the entry above it that was WRONG. That entry said the capability
 * this recognizer waits on — a close of the drive target that reaches the convergence point whatever KIND the
 * target is — had to be built. It was already built: JSIterClose / CONT_ITER_CLOSE parks 7.4.9 with BOTH of its
 * operations on the tramp and resumes whichever machine asked, dispatched by outer_kind. The helper simply was
 * not one of the kinds it resumed. Naming a built mechanism as missing is the same defect as naming an unbuilt
 * one as safe, so it is corrected here rather than left standing.
 *
 * Two closes moved onto it. do_iter_helper_step's take-limit close asked `is the source a GENERATOR?` and closed
 * anything else inline with JS_IteratorClose — one more selector picking a C path, and its own comment admitted
 * the C path crashed. %IteratorHelperPrototype%.return did the same from js_iterator_helper_next under the
 * written claim that closing "iterates nothing, so no drive-to-completion"; it does, and it is a step machine
 * now. Neither could land without the other: the helper's close reaches a helper source, whose `return` had to
 * already be a machine, and that machine closes ITS source through the same parked 7.4.9. Landing them together
 * also deleted OP_iterator_close's 50-line generator-source special-case, which a step def on `return` makes
 * unreachable — the convergence point asks that question first.
 *
 * The exhausted helper's {undefined, true} moved into the drive next, so `it->done` and the KIND LIST are gone
 * from the recognizer. A case a recognizer hands to the other implementation is a case that implementation has
 * to keep existing for, whether or not any page code is in it — that is why answering it in the drive is the
 * removal and declining it was not.
 *
 * Then the identity half went the way every other one has. `.next` DECLARES itself — JS_CFUNC_ITERDRIVE_DEF,
 * cproto JS_CFUNC_iterdrive, no function pointer — and the convergence point reads the declaration instead of
 * comparing against js_iterator_helper_next's address. The drive needed a cproto of its own rather than
 * JS_CFUNC_step because its DELIVERY has modes (ITH_DIRECT / ITH_FOROF / ITH_ITERNEXT / ITH_CONSUME) that a
 * step machine's push-the-result cannot express.
 *
 * js_iterator_helper_next is DELETED, and what is left is not a recognizer: iter_helper_drive_ready asks only
 * whether a drive can BEGIN on this RECEIVER — 27.1.4's internal slot, and not-already-driving — with no
 * builtin identity in it. Its two declines are spec answers, given by js_call_c_function's iterdrive arm, and
 * a DCHECK there asserts that no THIRD reason can reach it. 7 -> 6.
 */
const CEILING = 0;              // tramp_can_call* — ZERO. Any rise is a recognizer coming back.
/* TWO, and they are the two OPERAND SHAPES a call can have — not one convergence point plus an exemption.
     do_generic_callee   every STACK-shaped call: the operands are the caller's, and the result is pushed.
     do_cont_dispatch    every SEQUENCE-shaped call: the operands are in the sequence's own buffer (a step
                         machine's cb_args, the ToPrimitive walk's cb) and the result goes back to that sequence.
   The second used to be do_step_step's private copy, with the ToPrimitive walk keeping a second copy of the same
   three-way question — and that copy had never learned to route a step machine, so `{valueOf: [].sort}` reached
   js_call_c_function's DFAIL while `arr.map(String)` did not: one callee answering differently depending on which
   sequence invoked it, which is the exact defect the ban exists for. They are one label now.
   A THIRD still fails here. There is no third operand shape, so a third would be a copy. */
const CONVERGENCE_POINTS = 2;

const src = readFileSync(new URL('./qjs/quickjs.c', import.meta.url), 'utf8');

/* STRUCTURAL INTEGRITY of the interpreter.
 *
 * Four separate times, a scripted deletion in quickjs.c (brace-walking to "the next line equal to `}`", or matching
 * a two-line forward DECLARATION instead of the definition) swallowed a neighbouring region — once 31,000 lines.
 * The compiler reports this as `label 'done' used but not defined` thousands of lines from the damage, which reads
 * like a subtle C error rather than "your edit ate a function". Name it here instead: these labels and entry points
 * are load-bearing, and if one vanishes the edit was destructive, not clever. Cheap, exact, and it fires before
 * anything is compiled. */
const REQUIRED = [
  'done_generator:', 'exception:', 'do_generic_callee:', 'do_step_tramp:', 'do_step_step:',
  'do_tramp_call:', 'do_apply_tramp:', 'do_construct_tramp:', 'do_construct_dispatch:', 'do_return:',
  'static JSValue JS_CallInternal(', 'static JSValue js_call_c_function(',
];
const missing = REQUIRED.filter(t => !src.includes(t));
if (missing.length) {
  console.error(`quickjs.c STRUCTURAL DAMAGE — these load-bearing anchors are gone:`);
  for (const m of missing) console.error(`    ${m}`);
  console.error(`An edit removed more than it named. Restore (git checkout -- quickjs.c) and redo the deletion`);
  console.error(`with EXACT-TEXT edits, one site at a time — never a script that guesses where a block ends.`);
  process.exit(1);
}

const names = new Set();
for (const m of src.matchAll(/^static (?:inline )?bool (tramp_can_call[a-z_0-9]*)\(/gm)) names.add(m[1]);

/* The other spellings the ban covers — a uniform predicate asked at a CALL SITE is the same thing wearing a
   different name, so count those too rather than let the shape migrate. */
const callSitePredicates =
  (src.match(/if \(tramp_step_def_of\(call_argv\[-1\]\)\)\s*(\{|goto)/g) || []).length;

if (callSitePredicates !== CONVERGENCE_POINTS) {
  console.error(`call-site step predicates: ${callSitePredicates}, expected exactly ${CONVERGENCE_POINTS}.`);
  console.error(callSitePredicates > CONVERGENCE_POINTS
    ? `  MORE than one means the question is being asked per call shape again — that is the recognizer allowlist
` +
      `  in its final costume. Every bodyless callee must converge on do_generic_callee.`
    : `  FEWER means the convergence point was removed; a bodyless callee now reaches js_call_c_function, whose
` +
      `  DFAIL is only a backstop.`);
  process.exit(1);
}
/* The CONSTRUCT side has the identical shape and produced the identical defect. The step-ctor question was
   written out THREE times — OP_call_constructor's operand reshape, OP_init_ctor's super(), and Reflect.construct's
   list form — and a fourth construct shape, a step machine asking for a Construct of its own (ArrayBufferSpecies-
   Create inside slice), never got a copy: it fell through to JS_CallConstructor and js_call_c_function's DFAIL.
   One builtin answering differently depending on how the construct was spelled, exactly as on the call side.
   It is ONE arm in do_construct_dispatch now, serving both construct operand SHAPES (-2 pops [func, new_target,
   args] off the caller stack, 0 pops nothing). Reflect.construct is the one exception and for the same reason
   Reflect.apply is: it is CALL-SITE-RESOLVED — its arguments come from a LIST that must never touch the operand
   stack — so it builds the state at the operator site rather than asking this question. */
const CONSTRUCT_CONVERGENCE_POINTS = 1;
const constructSitePredicates =
  (src.match(/tramp_step_ctor_def_of\(con_func\)/g) || []).length;
if (constructSitePredicates !== CONSTRUCT_CONVERGENCE_POINTS) {
  console.error(`construct-site step predicates: ${constructSitePredicates}, ` +
                `expected exactly ${CONSTRUCT_CONVERGENCE_POINTS}.`);
  console.error(constructSitePredicates > CONSTRUCT_CONVERGENCE_POINTS
    ? `  MORE than one means the construct-side question is being asked per construct shape again. Every
` +
      `  construct spelling must converge on do_construct_dispatch.`
    : `  FEWER means the construct convergence point was removed; a step-machine constructor now reaches
` +
      `  JS_CallConstructorInternal, whose js_call_c_function DFAIL is only a backstop.`);
  process.exit(1);
}

/* Construct-side TARGET-KIND questions live only at the convergence point. Each of these spellings used to be a
   copy — at OP_call_constructor for bound targets and for proxies, at Reflect.construct three times over — and
   each copy knew a different subset of kinds, so one builtin answered differently depending on how the construct
   was written. `new`, `super()`, the spread, a step machine's own Construct and Reflect.construct all reach
   do_construct_dispatch now, and the arms there rewrite a bound or trapless-proxy target and re-enter. */
const strayCall = [
  ['js_tramp_proxy_apply(', 3, 'proxy [[Call]] reshaped anywhere but the ONE arm at do_generic_callee ' +
                               '(the count is its declaration, its definition and that arm)'],
  /* The async-generator drive is asked in exactly TWO places: its definition and the call convergence point.
     4 -> 2. The two extra were the iterator-protocol opcodes (OP_iterator_next and OP_iterator_call), which
     recognized the receiver themselves so they could hand the drive an OPERAND SHAPE — and a shape enum is a
     per-call-site mode register, the thing that made `for (x of {next: g.next.bind(g)})` deliver into the wrong
     protocol slot. Both push a real call now and let the continuation say where the promise goes; OP_iterator_
     close's receiver-class arm went the same way, into 7.4.9's generic path that was already sitting under it. */
  ['tramp_agen_method_magic(', 2, 'the async-generator drive question asked anywhere but its definition ' +
                                  'and do_generic_callee'],
];
for (const [needle, want, what] of strayCall) {
  const got = src.split(needle).length - 1;
  if (got !== want) {
    console.error(`call-side probe \`${needle}\`: ${got}, expected ${want} — ${what}.`);
    console.error(`  It was written out at OP_call AND at do_forward_dispatch, and neither copy was on the`);
    console.error(`  .apply / spread route — so a trap with a loop in it ran to completion under p(...arr).`);
    process.exit(1);
  }
}

const strayConstruct = [
  ['tramp_bound_target(call_argv[-2]', 0, 'a bound construct target asked about at OP_call_constructor'],
  ['js_tramp_proxy_construct(ctx, call_argv[-2]', 0, 'a proxy construct reshaped at OP_call_constructor'],
  /* Reflect.construct has NO call-site resolution left: it is a step machine, and its operator-site reshape was
     the redundant twin of the machine's own argument block. 2 -> 0 with that deletion, and the predicate went
     with it — a recognizer with no call site is residue, which is what this line now pins. */
  ['tramp_is_reflect_construct', 0, 'a Reflect.construct recognizer at all — the machine answers every spelling'],
];
for (const [needle, want, what] of strayConstruct) {
  const got = src.split(needle).length - 1;
  if (got !== want) {
    console.error(`construct-side probe \`${needle}\`: ${got}, expected ${want} — ${what}.`);
    console.error(`  Every construct spelling must converge on do_construct_dispatch; a copy here is the`);
    console.error(`  recognizer allowlist in its construct-side costume.`);
    process.exit(1);
  }
}

/* BODY-ENTRY convergence. A callee that HAS a bytecode body has exactly four ways in (plain / async / generator
   create / async-generator create), and WHICH one is a property of the callee, never of the call spelling. That
   list was written out per call shape and the copies drifted: OP_call and OP_call_method asked all four,
   do_forward_dispatch asked three, and do_apply_tramp asked one — so an async generator reached on the tramp
   through `ag()` but drove its coroutine to completion through `method.call(o)` (Array.fromAsync) and
   `gen(...spread)` (the arguments-object spread tests). Same defect as the recognizer ban, different axis: one
   callee answering differently depending on how the call was written. The four predicates now live in
   tramp_body_entry and nowhere else; a re-appearing per-site copy fails here.

   ALL FOUR are pinned now, not two. They used to be spelled tramp_can_call_*, which put them in the recognizer
   ratchet above — a claim the NAME made and the code did not, since each answers which of four different entry
   ALGORITHMS a callee has rather than choosing against a legacy body. Renaming them to tramp_body_is_* lowered
   that ceiling, and a rename that lowers a ratchet is the move this file distrusts most, so the claim it rests
   on is enforced here: a fifth `if (tramp_body_is_*(x)) goto ...` at any call site — the shape the rename could
   otherwise have laundered — raises a count and fails. The expected numbers differ per predicate only because
   two of them carry a forward declaration and `plain` is also asserted on by two DCHECKs. */
const bodyPreds = [
  ['tramp_body_is_plain', 5],   /* fwd decl + definition + tramp_body_entry + two eval-closure DCHECKs */
  ['tramp_body_is_async', 2],   /* definition + tramp_body_entry */
  ['tramp_body_is_gen',   3],   /* fwd decl + definition + tramp_body_entry */
  ['tramp_body_is_agen',  2],   /* definition + tramp_body_entry */
];
for (const [p, want] of bodyPreds) {
  const uses = (src.match(new RegExp(`${p}\\(`, 'g')) || []).length;
  if (uses !== want) {
    console.error(`body-entry drift: ${p} appears ${uses} times, expected ${want}.`);
    console.error(`  A call site is asking the body-entry question itself again. Route that shape through`);
    console.error(`  TRAMP_BODY_DISPATCH (or, for an apply-shaped call, the apply-mode vector) instead — a site`);
    console.error(`  declares its OPERAND SHAPE, never which bodies it happens to know about.`);
    process.exit(1);
  }
}

/* PER-CALL-SITE MODE REGISTERS — the same defect on a third axis, and the one this ratchet did NOT catch.
 * A coroutine driver needs to know which DELIVERY it owes (for-of pair, iternext replace, direct push). That was
 * carried by a register the CALL SITE wrote on its way to the driver — and a call site can only write it if it
 * RECOGNIZED the callee itself, so every register write sits behind a per-site copy of the driver question.
 * The consequence is not an abort: a spelling the copy misses (a bound `next`, a proxied one, a Proxy over the
 * iterator) still reaches the driver through do_generic_callee, which resolves it correctly — but with the
 * register unset, so the driver delivers in the WRONG MODE. `for (x of {next: g.next.bind(g), …})` produced an
 * EMPTY loop, silently. A silent wrong answer is worse than the abort a missing route usually gives.
 *
 * The fix is uniform and removes the register with the copy: the mode is the CONTINUATION KIND the call already
 * carries (CONT_FOROF_NEXT / CONT_ITER_NEXT_OP), read at the driver. tramp_gen_forof went that way first and its
 * OP_for_of_next recognizer is deleted. The count is the number of NON-DEFAULT writes still standing; it may
 * only go down.
 *
 * 5 -> 3 -> 0. tramp_ith_mode went the same way as tramp_gen_forof, and its recognizer went with it — including
 * the OP_call_method copy that made `h.next.bind(h)()` reach js_iterator_helper_next's DFAIL while `h.next()` did
 * not. tramp_agen_shape was the last, and it took the AGEN_FIN_* enum with it: an async-generator drive is an
 * ordinary call whose promise goes wherever the CONTINUATION says. Zero is the resting state; any rise means a
 * call site is telling a driver what to do again. */
const MODE_REGISTER_WRITES = 0;
const modeWrites =
  (src.match(/tramp_ith_mode = ITH_(FOROF|ITERNEXT)/g) || []).length +
  (src.match(/tramp_agen_shape = AGEN_SHAPE_(CLOSE|ITERNEXT|ITERCALL)/g) || []).length;
if (modeWrites > MODE_REGISTER_WRITES) {
  console.error(`per-call-site mode registers: ${modeWrites} > ceiling ${MODE_REGISTER_WRITES}.`);
  console.error(`  A call site is telling a coroutine driver which delivery it owes again. Derive it from the`);
  console.error(`  CONTINUATION KIND at the driver and delete the recognizer that made the write possible.`);
  process.exit(1);
}
if (modeWrites < MODE_REGISTER_WRITES) {
  console.error(`per-call-site mode registers: ${modeWrites} < ceiling ${MODE_REGISTER_WRITES} — LOWER it in ` +
                `engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}

/* THE ITERATOR-PROTOCOL READS. 7.4.4 IteratorComplete and 7.4.5 IteratorValue read `done` and `value` off a
 * result object a page's own iterator built, and GetIterator/GetIteratorDirect read `next` off the iterator
 * itself. Every one of those is the page's code — an accessor, a Proxy `get` trap — and every one of them used to
 * run from C, where a loop inside it preempts in an activation with no flow base.
 *
 * They are gone one consumer at a time: the consume machine, the async-from-sync wrapper, the iterator helpers,
 * the Promise combinators, the for-of and async protocol tails, yield*'s delivery, the two for-of acquires, the
 * consumer and combinator acquires, `for await`'s wrap, flatMap's inner, the eager terminals, and Iterator.from.
 * ALL THREE have reached ZERO C-side reads and must stay there. `next` was the last, held by the iterator-helper
 * FACTORY, whose body was shared by two different declarations — take/drop a coerce-then-compute machine,
 * map/filter/flatMap plain C functions — so neither shape could route it; all five are one step machine now.
 *
 * Counted as JS_GetProperty/JS_HasProperty against the atom, which is the shape every one of them had. This used
 * to EXCLUDE js_obj_to_desc by body, because that function's `value` is a property DESCRIPTOR's rather than an
 * iterator result's — a live C-side page-code read, but one belonging to ToPropertyDescriptor. js_obj_to_desc is
 * DELETED, so the exclusion is gone and the count is over the whole file again, which is strictly stronger. */
const iterSrc = src;
const ITER_READS = [['JS_ATOM_done', 0], ['JS_ATOM_value', 0], ['JS_ATOM_next', 0]];
for (const [atom, want] of ITER_READS) {
  const got = (iterSrc.match(new RegExp(`JS_(Get|Has)Property\\(ctx, [A-Za-z_0-9>.\\-]+, ${atom}\\)`, 'g')) || []).length;
  if (got > want) {
    console.error(`C-side iterator-protocol reads of ${atom}: ${got} > ceiling ${want}.`);
    console.error(`  A consumer is performing 7.4.4 / 7.4.5 / GetIterator's read from C again. It is the page's`);
    console.error(`  code — issue it as a keyed-operation request whose outer continuation is that consumer.`);
    process.exit(1);
  }
  if (got < want) {
    console.error(`C-side iterator-protocol reads of ${atom}: ${got} < ceiling ${want} — LOWER it in ` +
                  `engine/check_recognizers.mjs so the gain cannot be given back.`);
    process.exit(1);
  }
}

/* THE DESCRIPTOR family, the same shape one property-set over. ToPropertyDescriptor (6.2.6.5) performs TWELVE
 * keyed operations on a descriptor object — HasProperty then, if present, Get, for each of six fields — and every
 * one of them can be an accessor or a Proxy trap. Object.defineProperty and Object.defineProperties do them as
 * phases through JSDescCursor, and so does 10.5.5 step 13's walk of a `getOwnPropertyDescriptor` TRAP's result —
 * which needed a capability that did not exist: a keyed request whose OUTER is another keyed OPERATION, so the
 * walk's twelve reads can issue from inside the delivery of the operation they belong to.
 *
 * IT IS ZERO NOW, and how it got there is the point. The count was 13 = js_obj_to_desc's twelve (six fields x
 * HasProperty + Get) plus one `enumerable` read; the note here twice predicted which conversion would remove the
 * twelve and was twice WRONG, because js_obj_to_desc's last caller was not a consumer at all — it was the C
 * [[GetOwnProperty]] HOOK, reachable only from C. What removed it was DELETING that hook's BODY, which the armed
 * DFAILs had already proven unreachable across the whole corpus. A count that will not fall to a conversion may be
 * waiting on a deletion instead.
 *
 * The ONE that remains is the `enumerable` read shared by both enumerable-key walks through
 * js_desc_object_is_enumerable, which is safe by construction — every GP_GETOWNPROP delivery rebuilds the record
 * through js_desc_to_object — and asserts it with js_read_is_page_code rather than claiming it in a comment. A
 * second walk needing the same read is a reason to share the site, never to raise this. It may only go down. */
/* THE UNROUTED CONSUMERS behind those twelve, counted rather than described. An own-keys walk with
 * JS_GPN_ENUM_ONLY has to ask each key's ENUMERABILITY, which on a Proxy is the `getOwnPropertyDescriptor`
 * trap — so every C caller that passes that flag runs the trap from C and never reaches the routed walk.
 * JS_GPN_SET_ENUM takes the SAME per-key path (JS_GetOwnPropertyFlagsInternal, one call per key); this counted
 * only ENUM_ONLY at first and so UNDERCOUNTED by one — for-in's slow path, which uses SET_ENUM. The number moved
 * up because it was wrong, not because ground was given: a count that flatters itself is worse than no count.
 *
 * 6 -> 5 -> 4. The first to convert was JSON.parse's reviver, which was already an explicit-stack DFS machine
 * and so only needed the walk to become resumable — that is JSEnumKeys, a shared cursor for
 * EnumerableOwnPropertyNames' key half, which the rest adopt the same way. The second was not a conversion at
 * all: internalize_json_property, the recursive C walker that machine had REPLACED, was still in the file with
 * only its own recursion calling it, so this count had been treating dead code as work to do. The third WAS the
 * hard one: JSON.stringify's SerializeJSONObject could not adopt the cursor while js_json_to_str was a C
 * recursion, because a resumable walk inside a non-resumable one suspends nothing — so stringify became a step
 * machine in the same diff, and its six OTHER C-driven page-code points (the `toJSON` read and call, the
 * replacer call, LengthOfArrayLike, every element and member read) became requests with it. The fourth was
 * for-in: its SET_ENUM prototype-chain walk and its [[GetPrototypeOf]] per link are requests now, the whole
 * collection driven from OP_for_in_start as a step machine.
 *
 * The fifth was import attributes, whose walk was not only C-driven but in the WRONG ORDER: 7.3.23 with kind
 * key+value reads each surviving key's value BETWEEN that key's descriptor and the next key's, and the C body
 * collected every descriptor and then every value. The cursor performs the whole operation now, `kind` as an
 * operand, so the sequence a Proxy sees is the algorithm's.
 *
 * TWO LEFT, and NEITHER is a live consumer. Both are for-in's FAST PATH, which is a different algorithm rather
 * than a fallback:
 * it computes the enumeration with no user code at all, and its precondition is that the receiver AND every
 * link above it is an ordinary object — decided by for_in_is_ordinary before anything is read from any of them.
 * (That guard used to be applied to each link's PROTOTYPE and never to the receiver, so a Proxy receiver ran
 * three traps from C and two of them twice; the conversion fixed that at the root.) They stay counted so that
 * weakening the guard fails here rather than silently. It may only go down from here.
 *
 * Neither may be retired with an assertion instead of the guard — the cheap answer, checked rather than
 * assumed: for_in_is_ordinary is what makes them unobservable, and it is a real test on a real receiver. */
/* A CALL SITE is the flag word handed to a names walk: it always pairs with JS_GPN_STRING_MASK. Matching that
   pair is what separates the callers from the flag TESTS inside the walk itself. */
const enumOnlyCallers =
  (src.match(/JS_GPN_STRING_MASK\s*\|\s*JS_GPN_(ENUM_ONLY|SET_ENUM)|JS_GPN_(ENUM_ONLY|SET_ENUM)\s*\|\s*JS_GPN_STRING_MASK/g) || []).length;
if (enumOnlyCallers !== 2) {
  console.error(`C own-keys walks asking for enumerability: ${enumOnlyCallers}, expected 2.`);
  console.error(enumOnlyCallers > 2
    ? `  A new C caller is asking a Proxy's getOwnPropertyDescriptor trap for enumerability from C.`
    : `  One was routed: LOWER the count in engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}

/* THE PROXY-TARGET family, named by the finding above and counted so it can only shrink. Every call of
 * JS_GetOwnPropertyInternal is a C-side [[GetOwnProperty]]: harmless on an ordinary object (a shape lookup),
 * and the page's `getOwnPropertyDescriptor` trap the moment the receiver is a Proxy. Most of these sites are
 * a proxy invariant reaching its own TARGET, which is exactly the nested case above.
 *
 * The named blocker is BUILT: a GP_GETOWNPROP carries a `want_flags` modifier — the same shape no_throw takes,
 * one operation with two answer shapes — so a request can be answered with the attribute BITS instead of a
 * descriptor object, and no invariant has to read six fields back off a record the engine had just built. On
 * top of it the ROUTED [[GetOwnProperty]] performs 10.5.5 steps 10 and 12 as requests on the target
 * (JSGopdDesc's GD_TARGET / GD_EXT), so a proxy-of-a-proxy runs the inner trap on the chain.
 *
 * This count did not move, and that is honest rather than disappointing: the site it counts is inside
 * js_proxy_gopd_pre, which the ROUTED path no longer uses at all — it survives for the C [[GetOwnProperty]]
 * HOOK, which is the unrouted path the other 14 sites reach. The number falls when those consumers route.
 *
 * The SIBLING is DONE too, out of the same primitives: 10.5.11's check is JSOwnKeysChk, a phased machine that
 * issues CreateListFromArrayLike's reads on the object the trap returned and then IsExtensible,
 * [[OwnPropertyKeys]] and one want_flags [[GetOwnProperty]] per key on the TARGET — six C-driven operations
 * that are six requests. js_proxy_ownkeys_result went with it; js_proxy_ownkeys_check survives ONLY for the C
 * hook, which is why these numbers still do not move. 15 = the call sites, excluding the two declarations of
 * the function itself.
 *
 * The four OBJECT-LEVEL invariants (10.5.1-10.5.4) followed, as ONE machine rather than four: they differ only
 * in which target facts they need — IsExtensible(target), and the target's own prototype for the pair that
 * compares one — and in what they conclude. Its skips are part of the spec, not an optimisation: a false trap
 * result answers before IsExtensible is reached, and an extensible target settles the prototype pair without
 * [[GetPrototypeOf]], so neither may run the target's trap for a step the algorithm does not take.
 *
 * The KEYED invariants split by what they need from the target. [[HasProperty]] (10.5.7 step 9) and [[Delete]]
 * (10.5.10 steps 9-12) need only its attribute BITS and its extensibility, so they are one more machine on the
 * primitives already here — and `has` was reading the target JSObject's storage bit where step 9.b.ii says
 * IsExtensible, the SECOND time that exact deviation has turned up, fixed by asking for the internal method.
 *
 * [[Get]], [[Set]] and [[DefineOwnProperty]] followed, and the blocker named for them was solved a BETTER way
 * than the one named. Reading the fields back off a rebuilt descriptor OBJECT was the plan; what the file's own
 * reasoning about the CAPABILITY request says instead is that a request may answer with a RECORD — "packing
 * them into the step's single result would make every consumer unpack a tuple the engine had just built". So
 * want_flags is DELETED and a [[GetOwnProperty]] request names a JSDescFacts to answer into, carrying the flags
 * AND the [[Value]] and the accessors. No descriptor object is built for a consumer to take apart, and the
 * ToPropertyDescriptor count above did not have to move at all.
 *
 * That also collapsed three C-side reads into one: the three invariants share js_proxy_facts_from_c, the single
 * unrouted read the C hooks reach. 15 -> 13.
 *
 * Object.hasOwn and Object.prototype.hasOwnProperty followed — ONE machine, the arg choosing which of the two
 * ORDERS the spec states, since hasOwnProperty coerces the key first and hasOwn the object. Both also interned
 * the key with JS_ValueToAtom rather than ToPropertyKey, so a key object toString ran from C as well. 13 -> 11.
 *
 * __lookupGetter__ and __lookupSetter__ followed — one machine, the arg choosing which accessor, walking the
 * prototype chain with a GETOWNPROP and a GETPROTO request per link. It is the first consumer to want the
 * RECORD from a STEP machine rather than from a continuation, because its answer IS desc.[[Get]], so the
 * header gained the field the DEFINE descriptor shape already models. 11 -> 10.
 *
 * Function.prototype.bind became a step machine (STEPDEF_FUNC_BIND): 20.2.3.2 step 5's HasOwnProperty and steps
 * 6.a and 8's Gets of `length` and `name` are requests. SetterThatIgnoresPrototypeProperties followed — ONE
 * machine for both of %Iterator.prototype%'s accessors, the key being the arg — which needed the half of the
 * accessor declaration that did not exist: JS_CGETSET_STEP_DEF, a SETTER that is a step machine. A setter is as
 * much the page's entry point as a method is, and until now no accessor could carry a machine at all.
 *
 * Between them the public JS_GetOwnProperty entry lost every caller inside the engine, and its three callers
 * OUTSIDE it — engine/host/solver/cow.c — were a real defect rather than an exemption: a delta is captured
 * inside a write hook and swapped between two flows, so a Proxy in it would have fired the page's
 * getOwnPropertyDescriptor trap from the scheduler, and an accessor slot's restore would have run a setter.
 * The entry is now JS_GetOwnSlot, which reads the SLOT and ASSERTS that neither shape is reachable — and
 * because every entry is read there before it is written back, that assert also guards the restore. The count
 * does not move (the slot read is still a JS_GetOwnPropertyInternal call) but the site stopped being a claim.
 *
 * What is left is the 10 CONSUMERS. Of those, SEVEN are not consumers at all and are why this number will not
 * reach zero: two are the routed request entry's OWN in-place answers (JS_CallInternal), where the proxy
 * branch is above and the receiver is provably ordinary; two are side-effect-free probes that read only own
 * DATA properties (tramp_walk_continues, data_method_at); and three are the C hooks' own reads. The genuinely
 * routable ones left are the public JS_GetOwnProperty and JS_HasProperty entries. js_prop_walk_step's
 * exclusion test is NOT one: its operand is a list the COMPILER built for a destructuring pattern, which no
 * page can reach — it is a seventh safe-by-construction site, and the way to retire it from suspicion is to
 * ASSERT that rather than to keep saying it. Each one that stops calling JS_GetOwnPropertyInternal (or
 * JS_GetOwnPropertyNamesInternal) on a possibly-Proxy receiver and asks for a request instead drops this count
 * by one, and when it reaches the C hooks' own sites those hooks — and js_obj_to_desc, and the C forms of every
 * invariant above — go with them. Every ROUTED path is done; what remains is reached only from C. */
const gopdFromC =
  (src.match(/JS_GetOwnPropertyInternal\(/g) || []).length
  - (src.match(/static int JS_GetOwnPropertyInternal\(/g) || []).length;
if (gopdFromC !== 8) {
  console.error(`C-side [[GetOwnProperty]] call sites: ${gopdFromC}, expected 8.`);
  console.error(gopdFromC > 8
    ? `  A new C caller can reach a Proxy's getOwnPropertyDescriptor trap with no flow base.`
    : `  One was routed: LOWER the count in engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}

/* The same family measured on the OTHER internal method the invariants reach from C. Every JS_IsExtensible is a
 * C-side [[IsExtensible]]: a flag read on an ordinary object, and the page's `isExtensible` trap the moment the
 * receiver is a Proxy — which for a proxy invariant's TARGET is exactly the nested case. The
 * [[GetOwnProperty]] invariant's is a GP_ISEXT request now; the ones that remain belong to the C hook and to
 * [[OwnPropertyKeys]]'s check, which is the next conversion.
 *
 * Counted over EVERY call rather than over `s->target` spelled that way: the first version of this matched the
 * literal `ctx, s->target`, and a probe passing a context named anything else walked straight past it. A gate a
 * rename evades is a gate that flatters itself.
 *
 * 13 -> 14 -> 15, and UP is right here. [[DefineOwnProperty]]'s C invariant read `p->extensible` — the target
 * JSObject's storage bit — where 10.5.6 step 16.a says IsExtensible, so for a Proxy target it consulted the
 * proxy's own flag instead of its answer. Fixing that turns a read this gate could not see into a call it can.
 * The number moved up because it was WRONG, not because ground was given, exactly as the enum-only count did.
 * (Third instance of that deviation; 10.5.5 step 11.c and 10.5.7 step 9.b.ii were the first two.) Acting on
 * that suspicion found a FOURTH straight away, and it was the same bug half-fixed: 10.5.7 step 9.b.ii had been
 * corrected on the ROUTED path when it became a machine, and left standing in js_proxy_has_invariant, which is
 * the C hook's half. Routing one path is not fixing the check — both halves are the check.
 * Object.seal / freeze / isSealed / isFrozen became ONE step machine next (7.3.15 SetIntegrityLevel and
 * 7.3.16 TestIntegrityLevel are the same [[OwnPropertyKeys]] walk with a [[GetOwnProperty]] per key, differing
 * in what they open with and do per key). They ran FOUR kinds of the page's operations from C, so
 * `Object.freeze(new Proxy(o, {ownKeys(){for(;;){}}}))` aborted — as did all four spellings. TestIntegrityLevel
 * also asks IsExtensible where the spec states it, step 1, so an extensible Proxy no longer sees an ownKeys
 * trap and a gopd per key that 7.3.16 never performs. The superseded bodies are deleted, and with them the
 * unused public JS_SealObject / JS_FreezeObject, which ran those same traps from C for any embedder that called
 * them. 15 -> 14.
 * 14 = the call sites, excluding the definition. */
const extFromC = (src.match(/JS_IsExtensible\(/g) || []).length
  - (src.match(/^int JS_IsExtensible\(/gm) || []).length;
if (extFromC !== 7) {
  console.error(`C-side [[IsExtensible]] call sites: ${extFromC}, expected 7.`);
  console.error(extFromC > 7
    ? `  A new C caller can reach a Proxy's isExtensible trap with no flow base.`
    : `  One was routed: LOWER the count in engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}

/* DONE — the [[Set]] RECEIVER path, CONT_SET_RECV. A sweep of 36 operations against looping Proxy traps found
 * ten live aborts on ordinary code; eight are now fixed and the four that shared this root are among them:
 *
 *     Reflect.set({a: 1}, "a", 2, new Proxy({}, {getOwnPropertyDescriptor(){for(;;){}}}))
 *
 * OrdinarySetWithOwnDescriptor step 3 is `Receiver.[[GetOwnProperty]]` and `Receiver.[[DefineOwnProperty]]`, and
 * JS_SetPropertyInternal2 ran both from C at its tail. What made it look like a tail is that it is written as
 * one; what it actually is, is a SEPARATE operation over (Receiver, P, V) that consults nothing the walk built —
 * which is why it could become a machine parked on the keyed entry with no state carried across from the walk.
 * JS_SetPropertyInternal2 now takes `recv_pending` and hands the completion back rather than performing it, and
 * the DCHECK on a NULL `recv_pending` holds every other caller to "my receiver cannot be a Proxy".
 *
 * That single tail was why Array.prototype.reverse / push / splice / copyWithin aborted on a Proxy: their Set
 * goes through the proxy's default `set`, which forwards with the proxy as RECEIVER, and step 3 then ran its
 * traps from C. It was also why `super.x = v` with a Proxy `this` aborted, which is why that opcode routes on a
 * RECEIVER test the other write opcodes have no need for.
 *
 * Two things fell out of building it, neither visible from the outside beforehand. The C tail had no half of
 * 7.3.4: 3.e's JS_DefineProperty was called with no throw flag, so a receiver whose `defineProperty` trap
 * returned false reported SUCCESS to strict code. And the trapless-proxy forward loop restored every operand of
 * the parked request except the ANSWER SHAPE, so a forwarded [[GetOwnProperty]] built a descriptor object for a
 * requester that had named a record — latent until a requester asked for a record through a trapless proxy,
 * which this completion is the first to do.
 *
 * RETRACTED — "the RegExp prototype methods are not GENERIC" was recorded here and is FALSE. The reduction it
 * named does throw, and throwing is what the spec requires:
 *
 *     "aaa".replace(new Proxy(/a/g, {}), "b")     // TypeError, and CORRECTLY so
 *
 * 22.2.6.11 step 7 is `ToString(? Get(rx, "flags"))`; 22.2.6.4 `get flags` is generic and its step 4 is
 * `Get(R, "hasIndices")`; 22.2.6.6 `get hasIndices` throws when R has no [[OriginalFlags]] slot and is not
 * %RegExp.prototype% — and a Proxy has no such slot. The engine performs exactly that sequence, measured on a
 * logging trapless proxy: `Symbol(Symbol.replace), flags, hasIndices` and then the TypeError. The methods
 * themselves are already generic, also measured: a Proxy whose handler answers `flags`/`exec`/`lastIndex` runs
 * .replace, .match, .matchAll, .search and .split to completion, and `flags.call({})` returns "" after the
 * eight [[Get]]s in spec order. The entry was written from the error message rather than from the spec, which
 * is the one way a ledger can be worse than empty. The "TypedArray slice index coercion" item beside it is
 * stale too: `new Int8Array(8).slice({valueOf(){for(;;){}}})` parks.
 *
 * A FRESH sweep over the builtin families found five live aborts. ALL FIVE ARE FIXED:
 *
 *   %TypedArray%.prototype.sort  — drove cutils' rqsort with js_TA_cmp_generic calling JS_Call, so the sort's
 *                                  state was the C stack and the comparator could not suspend at any depth.
 *                                  Now STEPDEF_TA_SORT / _TOSORTED, sharing the Array machine's merge — and
 *                                  snapshot-first, which is what SortIndexedProperties actually says.
 *   Date setters                 — coerced through __JS_ToFloat64Free from C. Now one machine, because step 2
 *                                  reads [[DateValue]] BEFORE the coercions and step 7 tests THAT value.
 *   String.prototype.localeCompare — ToString'd receiver and argument from C. Now STRRECV_LOCALECOMPARE.
 *   Atomics                      — ToIndex and the value coercions from C. Now one machine, because
 *                                  ValidateAtomicAccess step 1 reads the LENGTH before step 2's ToIndex.
 *   Promise species              — js_promise_then ran SpeciesConstructor's two reads and NewPromiseCapability's
 *                                  Construct from C, and .catch and .finally reached it because both are
 *                                  spelled as an Invoke of `then`. Now STEPDEF_PROMISE_THEN.
 *
 * What the conversions found that the sweep could not see, each measured rather than argued:
 *
 *   - test262.conf's only trailing comment was on the `Atomics` line and only a LINE-INITIAL '#' was treated as
 *     one, so the feature was skipped for every compiler and 283 tests never ran. The corpus is 43222 now.
 *   - `[1,2,3,4].sort(() => NaN)` came out REVERSED: SortCompare step 3.b makes a NaN v a +0 and the merge
 *     asked `v <= 0`, which is false for NaN.
 *   - `new Date(0).setUTCHours(Infinity)` returned NaN and left the Date VALID: one flag stood for both "the
 *     stored time value is NaN" (return, write nothing) and "an argument is non-finite" (write NaN).
 *   - `Set(O,P,V,true)` reported SUCCESS when a Proxy receiver's `defineProperty` refused.
 *   - `Math.atan2(Symbol(), {valueOf(){throw x}})` ran that valueOf and reported ITS throw: splitting
 *     ToPrimitive from the narrowing put every later argument's coercion before the Symbol's TypeError.
 *   - Atomics was missing ValidateTypedArray step 4 (a ~write~ mode rejects an immutable buffer) and 25.4.15
 *     step 5 (notify on a non-shared buffer returns +0).
 *   - Four `JS_Invoke(promise, "then")` sites were AWAITs — PerformPromiseThen on an engine-built promise, never
 *     Promise.prototype.then — so the DFAILs recording "hand its Invoke out" are discharged by there being no
 *     Invoke. The one genuine Invoke left is 27.2.5.3 step 6's, inside .finally's reaction closure.
 *
 * A SECOND sweep, over surfaces the first did not touch, found three more. All three are fixed:
 *
 *   ToPrimitive's method READS — 7.1.1 step 2.a's GetMethod(input, @@toPrimitive) and 7.1.1.1 step 2's
 *                                Get(O, "toString"/"valueOf"), all done with JS_GetProperty from C behind a
 *                                comment asserting a coercion method "is a data property in every real case".
 *                                The widest of the three, since every coercion in the engine passes through it.
 *                                Now CONT_TOPRIM_GET.
 *   Map.prototype.getOrInsertComputed — a continuation-holding builtin whose callbackfn ran through JS_Call.
 *                                Now STEPDEF_MAP_UPSERT*, one machine for Map and WeakMap.
 *   the Symbol constructor      — ToString(description) from C. Now the coerce-then-compute declaration, with
 *                                20.4.1.1 step 1's NewTarget test as the leading validation.
 *
 * A FOURTH sweep, and the first one whose METHOD is a DFAIL rather than reading code: a `DFAIL` at the entry of
 * each of the SEVEN Proxy exotic hooks (js_proxy_get / _set / _has / _delete_property / _get_own_property /
 * _define_own_property / _get_own_property_names), which are the only places the page's traps can be invoked from a
 * C activation with no flow base. Because the abort names the internal method and a gdb backtrace names the caller,
 * each firing IS a work item; because the corpus aborts at the FIRST one, the survey ran per-directory so the whole
 * queue was visible at once. Eight directories fired, five distinct internal methods, and the queue was:
 *
 *   [[DefineOwnProperty]] — 25.5.1.1 InternalizeJSONProperty's CreateDataProperty (a reviver runs bottom-up with
 *     `this` bound to the holder, so it can plant a Proxy on a key the walk has not reached); 15.7.10 DefineField
 *     (a class field's receiver is whatever the base constructor returned); B.2.2.2/3 __defineGetter__/Setter.
 *   [[Get]] — 10.1.13 step 2's `Get(newTarget, "prototype")` from js_create_from_ctor, reached from the
 *     interpreter's BASE-class construct and from the C constructors; 23.2.3.26.1 step 6.b's array-like source
 *     read in TypedArray.prototype.set; OP_get_length, which had no route AT ALL; and every PRIMITIVE-base read.
 *   [[Set]] — every PRIMITIVE-base write.
 *   [[GetOwnProperty]] — the error-stack accessor's setter; 20.1.3.4 propertyIsEnumerable.
 *   [[OwnPropertyKeys]] — 27.1.3.4 Iterator.zipKeyed's injected getOwnPropertyKeys helper.
 *
 * All of those are fixed, and the DFAILs LAND: the whole 43222-test corpus now runs with all seven armed and fires
 * none, which is a far stronger statement than any of the per-site comments that used to say a site was safe.
 *
 * AND THEN THE SEVEN BODIES WENT. An armed DFAIL that never fires across the whole corpus is not the end state —
 * it is the PROOF that the code under it is unreachable, and unreachable page-code-running C is exactly what this
 * project deletes. Each hook is now its DFAIL plus a visible InternalError for release, where the DFAIL compiles
 * out and there is deliberately nothing to fall back to; answering from the proxy's own empty shape would be a
 * WRONG answer, which is worse than a missing capability. What went with them, none of it reachable and all of it
 * running the page's code from an activation with no flow base:
 *   js_proxy_ownkeys_check   10.5.11 steps 5-11, including CreateListFromArrayLike on the TRAP RESULT
 *   js_proxy_gopd_check/_pre 10.5.5 steps 9-17 with the target's facts read from C
 *   js_obj_to_desc           6.2.6.5 ToPropertyDescriptor: six HasProperty/Get PAIRS on a page object
 *   js_proxy_has/_delete/_set/_define_invariant  four invariants over descriptors read from C
 * The routed path keeps every INVARIANT it shares (js_proxy_gopd_post and its three, js_proxy_get_check and its
 * siblings, js_proxy_get_invariant), so what was deleted is a WAY OF ASKING and never a check.
 *
 * The ratchets moved accordingly, and one of them says something the notes beside it had got wrong twice:
 * ToPropertyDescriptor C reads 13 -> 1, C-side [[GetOwnProperty]] 10 -> 8, [[IsExtensible]] 14 -> 8,
 * [[HasProperty]] 10 -> 3. The descriptor count had twice been predicted to fall to a CONVERSION (JSON.stringify,
 * then for-in); both were routed and it did not move, because js_obj_to_desc's last caller was not a consumer at
 * all. A count that will not fall to a conversion may be waiting on a DELETION instead.
 *
 * A FIFTH sweep armed the SIX Proxy entries the fourth had left alone, because they are not in the exotic-methods
 * table and so were never in that survey's reach: js_proxy_getPrototypeOf / _setPrototypeOf / _isExtensible /
 * _preventExtensions, which JS_GetPrototype / JS_SetPrototypeInternal / JS_IsExtensible / JS_PreventExtensions
 * dispatch to on class_id, and js_proxy_call / _call_constructor. Five of the six never fired. THE ONE THAT DID
 * named two callers, both in the Object prototype and both with the same shape — an accessor or a walk performing
 * an internal method on its RECEIVER:
 *
 *   B.2.2.1 Object.prototype.__proto__, BOTH halves. One machine, `arg` naming the internal method. It had to be
 *   installed WHERE THE INTRINSIC IS BUILT rather than in js_object_proto_funcs, because an accessor table entry
 *   can name ONE step id (JS_DEF_CGETSET_STEP, the setter's) and giving it a second would put another data field
 *   in a union that already holds function pointers — the strict-aliasing trap recorded further down this file.
 *   An accessor holds function OBJECTS, and a step-machine function object is reached through
 *   tramp_accessor_getter / _setter like any other callable, so nothing else had to change.
 *
 *   20.1.3.3 isPrototypeOf, a WALK of `? V.[[GetPrototypeOf]]()` — the page's code once per link, run as a C loop.
 *   Its C body also carried a js_poll_interrupts guard commented "avoid infinite loop (possible with proxies)",
 *   which is a BOUND: a proxy chain that keeps answering is unbounded work the scheduler preempts and pages, not
 *   something a builtin cuts short. Routing the read removed the reason the guard existed.
 *
 * TWO defects the conversion surfaced, neither of them in the machines:
 *   A step machine reached AS AN ACCESSOR by another request's GP_GET/GP_SET has its chain's outer set to
 *   CONT_GETPROP, and do_getprop_abandon's teardown walked that chain with tramp_step_chain_free, which does not
 *   own that kind — its DCHECK fired on a throwing `setPrototypeOf` trap under the new setter. The teardown now
 *   unwinds one more level through its own label. This is the shape to expect from every future accessor machine.
 *   B.2.2.1.2 step 5 — a FALSE status from [[SetPrototypeOf]] is the ACCESSOR's TypeError, not the internal
 *   method's (Reflect.setPrototypeOf yields the same boolean instead). Discarding it made `__proto__ = cycle` and
 *   a non-extensible receiver silently succeed; set-cycle.js and set-non-extensible.js caught it.
 *
 * ALL THIRTEEN Proxy C entries are now armed and the whole corpus fires none of them — so by the rule the seventh
 * through thirteenth established, THE OTHER SIX BODIES WENT TOO. What that removed, none of it reachable:
 * 10.5.1-10.5.4's four object-level traps with their invariants, 10.5.12/10.5.13's `apply` and `construct` traps
 * with the argument arrays they are handed, and proxy_resolve_trapless — the shared prologue of every C-driven
 * Proxy internal method, whose revoked check and TRAP READ are themselves the page's code (a handler can be a
 * Proxy or carry an accessor). The routed path reads a trap through CONT_TRAP_GET, which is that read as a request.
 * C-side [[IsExtensible]] 8 -> 7 with it.
 *
 * A SIXTH sweep, and the first one whose subject is not the Proxy: THE DRIVE-TO-COMPLETION OF AN ORDINARY BYTECODE
 * BODY. The only forcing function for one was the back-edge preempt DFAIL, which fires exactly when the body
 * happens to contain a LOOP — so a getter, a callback or an eval'd program with no back-edge ran to completion in
 * silence. g_sync_drive_to_completion is the same bulk detector g_drive_to_completion already is, one level wider,
 * and the harness reports it beside that one.
 *
 * Its condition is STRUCTURAL rather than a list of callers: the tramp never re-enters JS_CallInternal (it pushes a
 * heap frame and `goto restart`s), and the flow base enters through the JS_CALL_FLAG_GENERATOR branch, so ANY
 * arrival at the ordinary bytecode entry while a flow exists is a C-recursive drive by construction.
 *
 * THE FIRST VERSION OF THAT CONDITION WAS WRONG and the correction is the lesson: it also required
 * rt->current_stack_frame != NULL, which reads like "only count nested ones" and actually writes in an EXEMPTION
 * FOR THE HOST BOUNDARY — as if JS_Eval were allowed to run page code to completion. It is not. A host entry that
 * wants to run page code creates a flow (JS_FlowNew / JS_FlowResume) and runs it there, which is what
 * fork_preempt_eval does; only g_flow_base_gen == NULL is exempt, and that is baseline setup before any flow
 * exists. Dropping the extra condition took the whole-corpus count from 3926 to 4924 — UP, because the smaller
 * number was wrong, the same way the [[IsExtensible]] count moved up when it started asking the right question.
 *
 * FIXED first, because the detector named it immediately: OP_apply_eval's DIRECT eval ran the eval'd program with
 * JS_EvalObject's own JS_CallFree while `eval(x)` a few opcodes away compiled to a closure and ran it on the tramp.
 * One operation answering differently by how it was written, invisible unless the eval'd program had a loop in it.
 * Both spellings now share eval_direct_closure, which also puts the body-entry ASSERT in one place instead of one
 * per spelling — the ratchet on tramp_body_is_plain is what insisted on that rather than a second copy.
 *
 * NAMED, with its backtrace: js_bytecode_eval — the SELF-HOSTED builtins (Array.fromAsync, Iterator.zip,
 * Iterator.zipKeyed) are compiled bytecode run through JS_EvalFunction -> JS_CallFree from C, reached from
 * js_bytecode_autoinit, i.e. from a lazy property READ that can happen at any depth inside a flow. The body is
 * engine-authored, which is why it has never mattered — but "engine-authored" is a claim, and the read that
 * triggers it is the page's. Routing it means an AUTOINIT that can park, which is the mechanism to build.
 * The 4924 is the honest size of the surface; it is a ratchet to drive down, not a number to explain away.
 *
 * A SEVENTH sweep, driven by that number: 4924 -> 2128, and the survey that produced it is worth keeping as a
 * METHOD. A bulk counter can be run PER DIRECTORY, so the surface has a histogram before a single line changes —
 * TypedArray 1053, Function 423, Array 422, Iterator 344 — and the DFAIL probe then only has to name the top of
 * each column instead of the whole list. That is strictly better than reading call sites: it ranks the work.
 *
 * INDIRECT EVAL was the banned shape in its plainest form. `tramp_is_global_eval(call_argv[-1]) && argc >= 1 &&
 * JS_IsString(argv[0]) && js_same_value(callee, ctx->eval_obj)` sat at OP_call/OP_tail_call — a recognizer at a
 * CALL SITE, so `eval.call(null,src)`, `eval.apply`, `Reflect.apply(eval,…)`, a bound eval, an eval behind a
 * trapless Proxy and `[eval][0](src)` all missed it and drove the program to completion. Every clause of it was a
 * silent fallback, including a comment that declared the narrowing on purpose: "Same-realm only ... a cross-realm
 * (0,evalFromOtherRealm)(src) must keep the C path". js_global_eval is a step machine now, the callee carries the
 * capability, and step_realm supplies the realm the C body used to get from js_call_c_function — so the clause the
 * comment defended does not exist to narrow. The recognizer is deleted; 0/0 recognizers still holds.
 *
 * CreateDynamicFunction was declared PRIMARGS_DEF, and that declaration was FALSE. A coerce-then-compute
 * declaration asserts one thing — with its arguments primitive the body has no user code left to reach — and this
 * body then evaluated the synthesized `(function anonymous(…){…})` source (a BYTECODE BODY, by C recursion) and
 * finished with GetPrototypeFromConstructor's `Get(newTarget,"prototype")` (a [[Get]] a Proxy new.target traps).
 * The lesson is about the declarations themselves: a capability declared at a definition is a CLAIM about that
 * body, and a wrong one hides exactly as much as a fallback does. It is a machine over three requests now, and
 * built-ins/Function's 423 went to 6, TypedArrayConstructors' 283 and GeneratorFunction/AsyncFunction/
 * AsyncGeneratorFunction's 341 to 0 — all of them were the harness building subclasses with `new Function`.
 *
 * step_program_run is the sub-sequence both of them perform, which is why neither is a stage that re-spells it:
 * "evaluate this source as a program" is one operation, and a program's body is a body like any other.
 *
 * 10.1.14 GetPrototypeFromConstructor came OUT of step_create_from_ctor_run. 10.1.13 is 10.1.14 followed by
 * OrdinaryObjectCreate, and a dynamic function is created by the PARSER while still ending in the same read — two
 * callers of the read, one of which does not want the object, is what makes it its own sub-sequence.
 *
 * JS_SpeciesConstructor is DELETED. step_speciesctor_run has been the implementation of 7.3.22 for some time and
 * %TypedArray%.prototype.slice was the last site still performing the two reads from C — under a comment that said
 * "keep the observable reads where the spec puts them", which was true about their ORDER and silent about the fact
 * that both of them ran the page's getter off the tramp. TypedArray 1053 -> 0.
 *
 * Iterator.concat read `@@iterator` on every argument from C. Its conversion has the shape to expect from any
 * per-argument walk: 27.1.2.1 step 3 INTERLEAVES the object check with the read, so argument 1 is checked only
 * after argument 0 has been read, and a machine that validated all arguments up front would be observably wrong.
 * The cursor is (i, checked) for that reason and not a loop over a pre-validated vector.
 *
 * A PROCESS FAILURE worth more than any of the above. `git diff`/grep after a surprising result is the rule, and
 * it was followed — and it still reported that a deletion had not happened, because a STRAY COPY of quickjs.c had
 * been committed at the repo ROOT in an earlier turn (82ba30b) and a `cd` to the parent for the gate silently
 * pointed three greps and a `git diff` at THAT file. A path is only a tree-check if you know which root it is
 * under; `git -C <dir>` and an absolute path are, a bare filename after a `cd` is not. The stray file is deleted.
 * IT THEN HAPPENED A SECOND TIME, mid-edit, and the edit landed on the OLD file — which is why the routine is now
 * to check `git log --oneline -1` before an edit batch and `git ls-remote` after every push. The revert is the
 * environment's, not the repository's: origin has always held the real history.
 *
 * THE ASSERT MACROS ARE CHECK / CHECK_FAIL / DCHECK / DFAIL — all four, in all three mirrors. DCHECK and DFAIL
 * have been the pair they are since 81d3a2c, and engine/host/check.h and extension/check.js have always spelled
 * the always-fatal pair CHECK / CHECK_FAIL; a4ae809 added a prefixed QJS_CHECK_FAIL to quickjs.c alone, with no
 * conditional half. The failure mode is what makes this worth writing down: reaching that missing half, the
 * reflex was to COIN THE MATCHING PREFIX (QJS_CHECK) rather than to ask why the prefix was there at all. It was
 * there for nothing — no CHECK or CHECK_FAIL identifier exists anywhere in the submodule's sources to collide
 * with. One mechanism, three files, one spelling; only the EMIT differs, which is the part that has to. A
 * divergent name is not cosmetic: it is what makes a missing half look like something to extend instead of
 * something to fix, and the extension then legitimises the divergence.
 *
 * NAMED, and it is not a routing problem: THE THREE SELF-HOSTED BUILTINS. The residual 2128 is all of it —
 * Array.fromAsync (Array 204 + Function 6) and Iterator.zip/zipKeyed (Iterator 166) — and the fix is not an
 * autoinit that can park, which is what the sixth sweep's note guessed. It is CLAUDE.md's architecture line:
 * "Don't code browser features in JavaScript. Browser features are C engine components." builtin-array-fromasync.js,
 * builtin-iterator-zip.js and builtin-iterator-zip-keyed.js are three browser features written in JavaScript, and
 * every mechanism around them — js_bytecode_eval, js_bytecode_autoinit, JS_AUTOINIT_ID_BYTECODE, the qjsc blobs —
 * exists only to run them. Eager instantiation does NOT fix it either: `$262.createRealm` (an iframe) builds a
 * realm from page JS, so context setup itself happens below a live flow. Iterator.zip is the one to do first: it is
 * synchronous, and JS_CLASS_ITERATOR_CONCAT with STEPDEF_ITER_CONCAT_NEXT/RETURN is already the exact pattern its
 * result object needs. Array.fromAsync is the hardest (an await inside a loop) and comes last.
 *
 * ITS FIRST ORDERED SUBPROBLEM IS BUILT: ctx->pending_close_iter is a STACK (pending_close_iters). Six sites
 * deferred an IfAbruptCloseIterator to the interpreter's exception label and every one of them asserted the slot
 * was free with the same message — "a nested IfAbruptCloseIterator needs a queue" — which is a gap DOCUMENTED six
 * times instead of fixed once. 7.4.11 IteratorCloseAll closes `in reverse List order`, so a site that owes several
 * (Iterator.zip's IfAbruptCloseIterators over « inputIter » ++ iters) pushes them last-first and the drain pops.
 * The drain needs no loop: every route ends by re-entering the exception label, and each close under an abrupt
 * completion discards its own throw, so the completion the drain restores stays the one the first failure raised.
 * The queue is also MARKED by JS_MarkContext now — it was the one context-held value that was not, so a cycle
 * through an iterator awaiting its deferred close could not be collected while the unwind was in flight (safe,
 * because an unmarked reference reads as a root, but a missed collection all the same).
 *
 * The multiplicity has NO consumer until Iterator.zip lands, and that is stated rather than hidden: what this diff
 * proves is that the queue is behaviour-identical on the single-entry path the whole corpus exercises, and what it
 * removes is six identical assertions of an unbuilt capability. A fixture for the multi-entry path arrives with the
 * IteratorCloseAll that needs it.
 *
 * ITERATOR.ZIP IS AN ENGINE COMPONENT. builtin-iterator-zip.js, its qjsc blob, its autoinit case and its Makefile
 * rule are DELETED; 27.1.3.3 is three step machines over a JS_CLASS_ITERATOR_ZIP record, and every page-visible
 * step is a request: the `mode` and `padding` reads, GetIterator on `iterables`, GetIteratorFlattenable on each
 * element, the padding walk, and — in the zipper's own next/return — each input's `.next()`, its result's `done`
 * and `value`, and each IteratorClose. All 38 tests pass; the corpus stayed at 0/43222 and SyncDriveToCompletion
 * fell 2128 -> 2050.
 *
 * FOUR SHARED SUB-SEQUENCES rather than per-site copies, and the reason is ORDER in every case. GetIterator (7.4.2)
 * and GetIteratorFlattenable (7.4.3) differ only in what an absent @@iterator means, so one walk takes that as a
 * parameter. IteratorStepValue (7.4.8) is performed in three places and the padding walk reads `value` even on a
 * done result while the other two do not — a parameter, not a difference each copy would get to make. IteratorClose
 * under a NORMAL completion is one walk. Under an ABRUPT completion it is not a walk at all: it is the DEFERRAL,
 * because there every close discards its own throw, which is exactly what the queue's drain does.
 *
 * FOUR BUGS, each named by one test, and three of them are the same mistake in different clothes — STATE THAT MUST
 * SURVIVE A SUSPENSION CANNOT LIVE WHERE THE SUSPENSION DESTROYS IT:
 *   `done` lived in the CALLER's `bool done` local, which is re-initialised on every re-entry, so a `value` read
 *   that suspended came back reporting not-done. It lives in the PHASE now (IS_VALUE vs IS_VALUE_DONE), which is
 *   what makes the two reads one resumable operation. padding-iteration.js found it.
 *   WHICH input's step is in flight is known only to the machine that asked, and the teardown needs it: 7.4.8
 *   leaves an abrupt source [[Done]] and UN-CLOSED, so closing it anyway called `return` on an iterator whose
 *   `next` had just thrown. Four tests name that one by one as "unexpected call ... return". The same shape as
 *   JSIteratorHelperData's drive_pending, for the same reason.
 *   The padding iterator had to leave `pad_iter` BEFORE its close ran, or a throwing `return` was called a second
 *   time by the teardown (padding-iteration-iterator-close-abrupt-completion.js: two `padding return`s).
 * The fourth is a spec-reading error, not an ownership one: `strict`'s length mismatch is
 * IfAbruptCloseIterators(throw TypeError, iters) — the completion is ABRUPT before the closes run, so each close
 * discards its own throw and the TypeError propagates. Running the closes first and throwing after let a throwing
 * `return` win, which two tests report as "Expected a TypeError but got a Test262Error".
 *
 * AND THE CALL BUFFER IS BORROWED. cb[0]/cb[1] are a receiver and a method something else already owns, which is
 * step_getprop_run's cb_coerce convention, and do_cont_dispatch only READS the slots. Freeing them in the teardown
 * was a use-after-free that ASan caught on the first abrupt GetIteratorFlattenable. Two conventions exist for that
 * buffer — Iterator.concat OWNS its copy — and a machine has to say which it is using.
 *
 * THE OLD SELF-HOSTED ZIP HAD A REAL FIDELITY BUG the conversion removed: after an abrupt next() it left the
 * generator in `executing`, so the NEXT next() threw "running zipper" instead of answering {undefined, true}. The
 * teeth check found it — the fixture runs green on the new build and fails on the old one with that TypeError.
 * A generator's own abrupt completion makes it `completed`, and the teardown is what owes that.
 *
 * ITERATOR.ZIPKEYED FOLLOWED, and it cost far less than Iterator.zip did — all 44 tests passed on the first run,
 * because the hard part was already built. Its zipper is the SAME record and the SAME drive; the only difference
 * is that the record carries KEYS, and its presence is what makes each tuple a null-prototype object keyed by them
 * instead of an Array indexed by position (zip_put_result is the one place that asks). Its setup differs in three
 * ways and every one of them is a request the self-hosted version performed from C: [[OwnPropertyKeys]] on the
 * argument, a per-key [[GetOwnProperty]] for own-enumerability, and — for `longest` — a per-key `? Get(padding,
 * key)` rather than an iterator walk. Iterator's SyncDriveToCompletion went 90 -> 2 and the corpus 2050 -> 1960.
 *
 * THE GATE CAUGHT THE ONE DEFECT, which is the point of having it. The own-enumerability probe first asked for the
 * descriptor OBJECT and read `enumerable` back off it — a ToPropertyDescriptor walk performed from C, the exact
 * shape the descReads ratchet forbids, in code written minutes after the rule was reaffirmed. The request already
 * has a RECORD answer shape (hdr.desc_facts) for precisely this: the step wants ONE attribute bit, so no descriptor
 * object need be built at all. A ratchet that only ever counted old code would have been decoration.
 *
 * TWO DELETIONS FOLLOWED THE BLOB. js_hasOwnEnumProperty had already gone; what was left was HOE_ARGS, the
 * own-enum machine's SECOND operand shape, whose only caller was builtin-iterator-zip-keyed.js. A mode nothing
 * selects is the fallback this file forbids, so it went with the blob rather than staying as a shape with no
 * consumer — HOE_THIS (propertyIsEnumerable) is now the machine's only form and the `from_this` branch is gone.
 *
 * STRING.PROTOTYPE[@@ITERATOR] IS A STEP MACHINE, and the interesting part is not the 1 it took off the counter —
 * it is that converting ONE method surfaced THREE unbuilt delivery arms, because it is the first step machine ever
 * to be a consumer's or a loop's @@ITERATOR. GetIterator step 4's Call has four acquire shapes and each one had a
 * delivery written for a returned heap FRAME with no step-machine twin: CONT_CONSUME_GETITER's completion (the
 * DCHECK "unknown sequence kind" named it, and gdb gave the kind as 19), CONT_CONSUME_GETITER's ABRUPT arrival via
 * js_toprim_abandon — a chain ending in a requester that walk does not own, parked for the exception label through
 * the slot that already exists for exactly that — and CONT_FOROF_ACQUIRE / CONT_FORAWAIT_WRAP both ways, which an
 * EXISTING fixture found as `for (var c of "abc")` failing with "not a function".
 * That is the widening rule working as designed: each missing arm announced itself, none of them was predictable
 * from reading the call sites, and the fix for each was the shape the frame delivery already had. The tell that a
 * conversion is not finished is a delivery chain whose last arm is a DCHECK rather than a route.
 *
 * THE INCLUDES FAMILY (includes / startsWith / endsWith) HAS FOUR page-visible steps, not the one the backtrace
 * showed. The probe named JS_ToInt32Clamp on the position; reading the body found the receiver's ToString, IsRegExp's
 * `? Get(searchString, @@match)` — js_is_regexp performs that [[Get]] from C — and the search string's own ToString
 * beside it. Their ORDER is observable, so the stages ARE that order, and the string compare that follows is its own
 * function taking both strings already coerced. String 63 -> 48, corpus 1959 -> 1944.
 *
 * ITS ONE BUG IS A NAMING LESSON: the three builtins were registered as `STEPDEF_STR_INCLUDES + magic`, and the
 * enum's order is not the magic's — endsWith(2) landed on the STARTSWITH slot and the two swapped semantics, which
 * 15 tests reported as `"word".endsWith("d")` being false. An id computed by arithmetic from an unrelated number is
 * a coincidence waiting to break; each builtin names its own id now.
 *
 * isWellFormed / toWellFormed followed for one reason: they are the ZERO-ARGUMENT case step_thisstring_run's own
 * note already named ("even a ZERO-ARGUMENT one could not stay a C body"), and they had stayed one. Their only
 * page-visible step is the receiver's RequireObjectCoercible + ToString; the scan after it sees a JSString and
 * invokes nothing, so each body keeps its whole algorithm and takes the coerced string. String 48 -> 46,
 * corpus 1944 -> 1942. Small, and the point of doing it is that a note naming a gap is not the gap being closed.
 *
 * THE SURVEY AS IT STANDS, so the next pass starts from BUILDING rather than re-measuring. Each root below has a
 * backtrace and a shape; the counts are per-directory from the current build (corpus 1942).
 *
 *   language/ 1095 — js_inner_module_linking's `JS_Call(m->func_obj, JS_TRUE, 0, NULL)`, the module graph's
 *   "initialize the global variables" pass, run from C once per module. CONFIRMED SHAPE: the module body's
 *   compiled prologue is `OP_push_this; OP_if_false <body>` (instantiate_hoisted_definitions), so `this === true`
 *   runs the hoisted-definition pass and returns. This is NOT a caller fix: js_link_module is a C DFS and
 *   JS_FlowEvalModule drives compile -> link -> evaluate from C, so routing it means module evaluation becomes a
 *   FLOW. That is the "module support in JS_FlowNew" gap this file has named since the sixth sweep, and it is the
 *   one root that needs a new mechanism rather than a conversion.
 *
 *   TypedArrayConstructors 265 — and an ATTEMPT AT IT WAS REVERTED, which is worth more than the count. What the
 *   probe measures is 23.2.5.1 step 6.b's ToIndex on the byteOffset and the length, in the ARRAY BUFFER branch
 *   (`new Uint8Array(buf, {valueOf(){…}})`); the element-count form's argument is already primitive by the time
 *   the body sees it, because an object takes a different branch.
 *   THE WRONG SHAPE, tried and reverted: a step-ctor declaration in FRONT of the whole constructor. Its object
 *   branches are ALREADY fully routed — the constructor carries a native-machine declaration (ITERCONS_TA_CTOR_BASE
 *   -> do_ta_consume_tramp, gated by ta_consume_ready) and js_typed_array_constructor_obj is a PURE DFAIL — so a
 *   machine in front intercepts object arguments the dispatch owns and re-enters that DFAIL. Converting the callee
 *   also changes its CALL form: `Uint8Array()` without new stopped throwing at the constructor_magic check and
 *   reached js_call_c_function's declared-consume DFAIL instead.
 *   WHAT IS NOT THE OBSTACLE, since it looked like one: native_machine and the STEPDEF magic are SEPARATE fields,
 *   so a callee can carry both declarations. The obstacle is that the coercions belong to ONE BRANCH, and a
 *   declaration on the callee cannot say "only when the first argument is a buffer".
 *   THE SHAPE THAT FITS, AND IS NOW BUILT: a SECOND ENTRY under the declaration the constructor already carries.
 *   ta_consume_ready declines a buffer (its own comment called that "a separate unbuilt piece"), and the arm below
 *   it asks ta_buffer_ctor_ready and drives a small coercion machine by POINTER — no STEPDEF id, the way a
 *   DELEGATE names its inner machine. So the callee keeps ONE declaration, the spec's own step 6 branch test picks
 *   which entry runs, the constructor stays a constructor_magic (its call form unchanged), and the coercion
 *   machine is reachable ONLY with a buffer first argument — which a DCHECK at its prologue asserts, so the object
 *   branches the consume walk owns can never arrive there. TypedArrayConstructors 265 -> 97, corpus 1942 -> 1774.
 *   The general lesson: when a callee already declares a walk, a new branch of the same builtin is another ENTRY
 *   under that declaration, never a machine wrapped around the whole thing — the wrapper cannot see which branch
 *   applies, and the dispatch already can.
 *   SEPARATELY: js_create_from_ctor's `prototype` read is still C-side in all three branches, and the engine
 *   coerces BEFORE it while 23.2.5.1 step 3 puts AllocateTypedArray first. That ordering is its own fidelity
 *   question and must not ride along with the routing; the tail's comment ("Re-validate buffer after
 *   js_create_from_ctor which may have run JS code") is the acknowledgement that the read is user code.
 *   THAT READ IS NOW ROUTED TOO (TypedArrayConstructors 97 -> 49, corpus 1774 -> 1726), and the ONE test that
 *   failed while it was being built is the whole lesson:
 *   THE COMMIT THAT LANDED IT SAYS "97 -> 1" AND THAT IS WRONG — the 1 was the ERROR count on the run before the
 *   fix, not the drive count. The corpus delta of 48 was sitting right beside it and disagrees with 96. A number
 *   copied out of a run without checking it against the other number in the same output is a fabricated claim;
 *   the arithmetic check (per-directory delta must equal the corpus delta) is free and was skipped.
 *
 *   throw-type-error-before-custom-proto-access.js — `Reflect.construct(TA, [Symbol()], newTarget)` must throw
 *   ToIndex's TypeError BEFORE new.target's `prototype` getter runs. So the read cannot simply be lifted to the
 *   front of the body; the body had to SPLIT at the AllocateTypedArray boundary. js_ta_view_plan is everything
 *   23.2.5.1 orders before 10.1.14 (both ToIndex calls, the detached checks, the offset/length range checks, the
 *   buffer allocation) and it returns int; js_ta_view_finish is the tail that takes the created object,
 *   re-validates the buffer the getter may have detached or resized, and initialises the view. The plan is held
 *   ON THE MACHINE across the suspension (JSTAViewPlan, with a `planned` flag so js_ta_ctor_fini releases the
 *   buffer it owns if the read throws), and the finish takes it. The unrouted C entry calls the same two halves
 *   with js_create_from_ctor between them, so there is ONE implementation of each half, not a twin.
 *   TWO BUGS THIS SHAPE PRODUCED, both worth keeping: (1) the `have_length` fall-through label is ALSO where a
 *   resume lands, so its unconditional `cb_result = JS_UNDEFINED` reset discarded the value the prototype read
 *   had just delivered and 10.1.14's realm fallback then handed back the intrinsic prototype —
 *   `Reflect.construct(BigInt64Array, [], customProto)` silently ignored the custom one. State that must survive
 *   a suspension cannot be re-initialised on the path the suspension returns to; the reset is now guarded by the
 *   stage. (2) the two halves are defined after the machine that calls them, so with no forward declaration C
 *   implicitly declared them as non-static int-returning — which is exactly the two errors the build gave, and
 *   neither of them named the real cause. A split that moves a definition below its caller needs the
 *   declaration in the same diff.
 *
 *   String 46 -> 34, BUILT. js_str_replace_prologue is deleted and its four reads are stages SR_MATCH ..
 *   SR_REPLACER: IsRegExp's `? Get(searchValue, %Symbol.match%)`, replaceAll's `? Get(searchValue, "flags")`, the
 *   ToString on THAT result (a second suspension point, so the value rides the state as flags_val rather than a C
 *   local), and `? GetMethod(searchValue, %Symbol.replace%)`. check_regexp_g_flag went with it — a helper whose
 *   only caller became stages is not a helper, it is the old implementation, and leaving it compiled is the
 *   legacy-twin shape however dead it looks.
 *   THE ORDER IS THE SPEC AND IT IS OBSERVABLE, which is why the stages are in it: the flags read precedes the
 *   @@replace read, RequireObjectCoercible(flags) precedes it too, a falsy @@match skips the flags read entirely,
 *   and String.prototype.replace performs no IsRegExp check at all — only replaceAll does. A prologue that ran
 *   all four in one C call could not have been suspended between any of them.
 *
 *   THE 1095 ATTRIBUTED TO MODULE LINKING WAS NOT REAL, AND FINDING THAT OUT WAS WORTH MORE THAN ROUTING IT.
 *   The DFAIL probe on `JS_Call(m->func_obj, JS_TRUE)` fired, and the BACKTRACE had no JS_FlowResume frame in it
 *   at all: JS_FlowEvalModule <- eval_buf <- run_test_buf, straight off the harness thread. So the module link
 *   was reading a g_flow_base_gen belonging to a flow that had already been FREED — the harness evaluates the
 *   test's includes as script flows first, and JS_FlowFree never cleared the global. JS_FlowResume leaves the
 *   base installed on purpose (so the post-eval job drain is still measured), and that is sound only while the
 *   flow is alive; nothing ended it. Two consequences, and the metric was the lesser one: every read after the
 *   free was of freed memory, and `gen_state == g_flow_base_gen` is a POINTER IDENTITY test, so the first
 *   JSAsyncFunctionState the allocator places at that address answers TRUE and preempts as if it were the base
 *   the pump is driving. Clearing it in JS_FlowFree took the corpus 1726 -> 725.
 *   THE PROCESS LESSON, which is the point: 1095 had been sitting at the top of this histogram for several
 *   sweeps as "the one root that needs a new mechanism", and the mechanism was never the problem. A count is not
 *   evidence about its cause until a backtrace has named the cause; "the biggest number, therefore the biggest
 *   root" skipped that step. Probe the top of the histogram BEFORE planning against it — the probe is one sed
 *   and one gdb run, and here it deleted the work item instead of scoping it.
 *
 *   AT 451, TWO NEW ROOTS, both in the INTERPRETER rather than in a builtin — which is where this sweep has
 *   been heading since the Map/Set forEach conversion.
 *   PROMISE 53 -> 37, BUILT. 27.2.4.1 step 4's `? Get(constructor, "resolve")` was a JS_GetProperty in the
 *   block that BUILDS the machine's state — no stage to suspend in, the same shape js_str_replace_prologue had.
 *   The block splits at the read: do_promise_all_have_cap issues a GP_GET with the new CONT_PROMISE_ALL_RESOLVE
 *   kind and do_promise_all_have_resolve takes the delivered method. Four dispatch arms (in-place delivery,
 *   suspended delivery, abrupt, abandon) plus the two DCHECK lists — a new getprop kind costs exactly those six
 *   places, which is worth knowing before adding another.
 *   The ABRUPT arm needed no body of its own: it lands on the same label with UNDEFINED as the method it never
 *   got and the exception still pending, and the setup-failed branch already synthesizes GetPromiseResolve's
 *   TypeError when nothing is pending — so a throwing getter and a non-callable `resolve` take one path.
 *   `tramp_iter_getiter` had to stay on the state across the request; it is an interpreter register and does not
 *   survive a suspension, which is the same reason the acquire's method moved onto the state for the capability
 *   Construct two lines above.
 *
 *   AND THE FIXTURE FOUND A DIFFERENT UNROUTED ROOT ON ITS FIRST RUN: `Promise.all.call(new Proxy(Promise, …))`
 *   aborts in js_proxy_get's DFAIL, because js_promise_new -> js_create_from_ctor reads `prototype` off
 *   new.target from C (quickjs.c:80589). It predates this diff — nothing in the corpus constructs a promise
 *   through a proxied constructor — and it is why that case is commented out of the fixture rather than passing.
 *   IT IS NOT A CREATECTOR_DEF, despite being the same read: the caller is do_promise_exec_tramp, an INTERPRETER
 *   LABEL, not a builtin with a declaration to carry. So it wants a GP_GET on pe_ntgt with a new continuation
 *   kind and a resume label, the way the combinator's `resolve` read just got one.
 *   BUILT AND REVERTED — the SECOND revert in this area, and what it cost is worth more than the count. The
 *   whole shape worked: CONT_PROMISE_EXEC_PROTO, the state allocated first with every pe_* register parked on
 *   it, js_promise_new split into js_promise_init_from_obj plus a create, a promise_exec_state_abort for the
 *   throwing-getter path (10.1.14 PROPAGATES — `new Promise(fn)` with a throwing `prototype` getter RAISES, it
 *   does not evaluate to a rejected promise), and built-ins/Promise went to 0/640 with 37 -> 36. ONE test caught
 *   a real bug on the way: proto-from-ctor-realm.js, because the non-object fallback must take
 *   `? GetFunctionRealm(constructor)`'s intrinsic and not the RUNNING realm's — new.target therefore has to
 *   survive until the fallback has consulted it, which is the opposite of freeing it as soon as the proto
 *   arrives.
 *   WHAT KILLED IT: a LEAK, hundreds of objects across built-ins/Promise, found by the gc_obj_list walk with the
 *   suite at 0/640 and the corpus at 0/43222.
 *   THE FIRST SUSPECT WAS WRONG AND IS RETRACTED. This file said the shared fall-through case group the new kind
 *   was added to must be a per-struct teardown, and that a JSPromiseExec freed as another struct was the cause.
 *   It is not: that switch is cont_kinds_are_distinct, which exists ONLY to be compiled — it turns a duplicate
 *   CONT_* constant into "duplicate case value" at build time and has no body at all. Adding the kind there was
 *   correct and cannot leak anything. Recording a suspect as if it were a finding is the failure here; a
 *   backtrace or a read is what makes a cause, and neither had been done.
 *   SO A NEW KIND COSTS FIVE DISPATCH PLACES PLUS ONE COMPILE-TIME REGISTRATION, and none of them is a teardown.
 *   WHERE THE LEAK IS NOT, established by reading the reverted diff against the paths it replaced: the abandon
 *   registration (above), the delivery arms (identical in shape to CONT_PROMISE_ALL_RESOLVE's, which is green),
 *   and js_promise_init_from_obj (the create simply moved out of it).
 *   THE ABORT PATH IS RULED OUT, by measurement rather than by reading. On the CURRENT (reverted) tree, both
 *   `Reflect.construct(Promise, [fn], ntWithThrowingPrototypeGetter)` and the suite's own
 *   proto-from-ctor-realm.js run with ZERO leaked GC objects — so the throwing-getter shape does not leak today,
 *   and the hundreds of objects were introduced by the reverted diff.
 *   AND THEY ARE ON PATHS THAT PASS. built-ins/Promise was 0/640 WITH the leak present, so whatever retains
 *   those closures happens on ordinary successful construction, not on an error path — which rules out
 *   promise_exec_state_abort, the arm the previous note pointed at, as well.
 *   AND THEN THE WHOLE DESIGN TURNED OUT TO BE A DUPLICATE. CONT_CTOR_PROTO (65) ALREADY IS this read:
 *   "10.2.2 [[Construct]] step 5's OrdinaryCreateFromConstructor, whose step 2 is `? Get(newTarget,
 *   "prototype")`", issued UNCONDITIONALLY from the base-class construct, with JSCtorProto parking exactly the
 *   con_* registers the suspension would lose and the non-object fallback already taking the CONSTRUCTOR'S REALM.
 *   Every problem the reverted diff solved from scratch — where to park the registers, what the realm fallback
 *   must be, that the read cannot be skipped for an ordinary new.target — is solved there, in a comment that
 *   says so.
 *   IT ALSO EXPLAINS THE LEAK, or is the best candidate yet: JSCtorProto BORROWS the callee and new.target
 *   ("whatever owns them across the construct owns them across this suspension too"), and the reverted diff
 *   dup'd new.target onto its own state instead. Two ownership models for one read is exactly the seam this file
 *   keeps recording, and `new Promise(fn)` issues that read on EVERY construction — which is the volume needed
 *   to leak hundreds across one suite.
 *   THE NEXT ATTEMPT IS NOW A MECHANICAL EDIT, and the question the last note posed is answered by reading two
 *   places. The construct dispatch tests the native-machine arms FIRST (NATIVE_PROMISE_EXEC at the top) and only
 *   the generic BASE-CLASS BYTECODE arm below them issues the read — which is why the promise arm is reached
 *   instead of after it. And the read's delivery is generic: the CONT_CTOR_PROTO arm restores every con_*
 *   register from JSCtorProto and `goto do_construct_tramp`, replaying the dispatch with `con_proto` set. It
 *   assumes nothing about the callee, so a C callee replays exactly as a bytecode one does.
 *   RETRACTED: THE SHAPE BELOW IS A REPLAY, AND REPLAY IS NOT RESUME. CONT_CTOR_PROTO's delivery restores the
 *   con_* registers and `goto do_construct_tramp` — it re-enters a block from the TOP and re-decides with a
 *   discriminator (`JS_IsUninitialized(con_proto)`). For the base-class arm that is survivable, because what it
 *   re-executes is pure prologue: the bytecode header, is_derived_class_constructor, narg_alloc. Nothing
 *   observable.
 *   IT IS NOT SURVIVABLE FOR A NATIVE-MACHINE ARM. Those arms live in do_construct_dispatch, ABOVE the read, so
 *   a resume that re-enters there re-tests promise_exec_ready(ctx, con_args, con_argc) — and every arm beside it
 *   — AFTER the page's `prototype` getter has run. That getter can mutate the args, so the second pass can
 *   select a DIFFERENT ARM than the first. A resume that can take a different branch than the suspension did is
 *   not byte-identical, which is the razor this file already states for flows: a yield you cannot prove is
 *   lossless is a cap. Re-deciding from re-read operands is exactly the thing the scheduler forbids, and the
 *   fact that it appears here as an interpreter goto rather than as a scheduler policy does not change what it
 *   is.
 *   SO THE ARM MUST SUSPEND AND RESUME, NOT PARK-AND-REPLAY: its continuation resumes at the point AFTER the
 *   read, with the arm already chosen and its operands already captured, the way do_promise_all_have_cap resumes
 *   after its capability Construct rather than re-entering the combinator's dispatch. CONT_CTOR_PROTO is still
 *   the right REQUEST; what cannot be reused is its delivery's re-entry.
 *   AND THE BASE-CLASS ARM'S RE-ENTRY IS NOW ASSERTED RATHER THAN ASSUMED. It re-executes the prologue — the
 *   bytecode header, is_derived_class_constructor, narg_alloc — before reaching the read, which is a REBUILD and
 *   is only sound while every one of those is a pure function of state JSCtorProto parked. That was a comment;
 *   it is a DCHECK now: the park captures arg_count and is_derived, the resume recomputes them and crashes if
 *   they differ. A line added to that block that reads mutable state turns the rebuild into a replay, and the
 *   assert names it at the origin instead of letting a construct silently decide differently the second time.
 *   THE ASSERT FIRED ON ITS FIRST RUN AND THE CAUSE WAS MINE, WHICH IS THE POINT OF WRITING IT: CONT_CTOR_PROTO
 *   has TWO delivery sites — the in-place arm and the suspended one — and only the first had been given the
 *   captured values, so every resume through the second compared against zero. The same hand-copied-list failure
 *   this file records for construct-abandon, caught this time in one run by an assertion rather than in three
 *   turns by a leak. A new field on a parked state is an obligation at every delivery, not just the one being
 *   looked at.
 *   The fixture that exercises it makes the getter mutate everything it can reach — the argument array, the
 *   constructor's `length`, a re-entrant construct — and the rebuild recomputes identically through all of it,
 *   so the purity claim is now measured rather than asserted in prose.
 *
 *   (superseded, kept so the retraction has something to point at) THE SHAPE — no new kind, no new state:
 *       if (cmach == NATIVE_PROMISE_EXEC && promise_exec_ready(...)) {
 *           if (JS_IsUninitialized(con_proto)) { ...park a JSCtorProto exactly as the base-class arm does...
 *                                                goto do_getprop_tramp; }
 *           ...existing body, taking con_proto instead of calling js_create_from_ctor...
 *       }
 *   `con_proto` is the register that carries the answer back (UNINITIALIZED = not yet read, read+reset in the
 *   dispatch so a re-entry cannot re-read it), and JSCtorProto BORROWS func and ntgt, which is the ownership the
 *   reverted diff got wrong. js_promise_new still splits into js_promise_init_from_obj plus a create — that half
 *   of the reverted diff was right and is worth keeping.
 *   THE SWEEP IS MEASURED AND IT IS ONE SITE. Every native-machine arm above the read was checked, and both
 *   consume arms already route it: do_setmap_consume_tramp issues `gp_obj = sc->ntgt; gp_atom =
 *   JS_ATOM_prototype` itself, and do_ta_consume_tramp's comments have js_create_from_ctor in the PAST tense
 *   along with the two teardown bugs that conversion cost. NATIVE_PROMISE_EXEC is the only arm left performing
 *   10.1.13 step 2 from C — so the mechanical edit above does not open a family, it closes one.
 *   THE CHEAPER ORDER, worth keeping: this took one awk over the dispatch and one grep per arm. Doing it BEFORE
 *   the reverted diff would have found CONT_CTOR_PROTO immediately, because the setmap arm issues that exact
 *   request four screens above where the invented one was written.
 *   FOURTH TIME: reach for the existing machine before writing one. CREATECTOR_DEF, STRRECV, js_creatector_step,
 *   and now CONT_CTOR_PROTO — every one of them found AFTER building or half-building a replacement. The check
 *   costs one grep for the operation's name and it has never once come back empty.
 *   THE LESSON THAT ACTUALLY PAID: one ASan run on one file discriminated "mine" from "pre-existing" in a single
 *   command, after two written-down suspects had both been wrong. Measure the OLD tree before theorising about
 *   the new one.
 *   THE OBSTACLE IS THE REGISTERS, and it is the same one that shaped the Promise.all setup: pe_ntgt,
 *   pe_super_ref, pe_executor_own, pe_outer and the call shape are all interpreter locals consumed into
 *   JSPromiseExec AFTER js_promise_new returns, and none of them survives a suspension. The state has to be
 *   ALLOCATED FIRST with promise = UNDEFINED, every register parked on it, and only then the request issued —
 *   which is what the combinator's comment already says about its own capability Construct. js_promise_new then
 *   splits into "create from a given proto" plus the state init, the way js_ta_view_plan/_finish and
 *   js_ta_copy_finish split.
 *   Cost, measured on the read just built: SIX places per new getprop kind — the in-place delivery, the
 *   suspended delivery, the abrupt arm, the abandon switch, and the two DCHECK lists.
 *
 *   THREE FIXTURE BUGS COST MORE THAN THE DIFF DID, all the same mistake: asserting on state that a SUBCLASS
 *   keeps mutating. `.then` on a subclass instance runs the subclass constructor again, so an `order` array
 *   grows past what the assertion expected; and an assertion that throws inside an onFulfilled is not caught by
 *   the onRejected passed to the SAME .then, so the failure surfaced as "$DONE() not called" rather than as the
 *   assertion. Both cost a debugging cycle each against an engine that was already correct. Assert on a PREFIX,
 *   and put assertions in their own .then.
 *   TYPEDARRAYCONSTRUCTORS 37: JS_DefineProperty -> JS_SetPropertyValue -> JS_ToBigInt64Free -> ToPrimitive ->
 *   the page's valueOf, from the GP_DEFINE arm at quickjs.c:28010. A typed-array ELEMENT WRITE coerces its
 *   value, and for a BigInt64Array that coercion is ToBigInt — the page's code, run from C inside the define.
 *   This is not a builtin to convert: it is the element-write path itself, so the coercion has to be hoisted out
 *   of JS_SetPropertyValue and issued as a request before the write, the way every other operand coercion is.
 *   Both are named with their line numbers because both are one site, not a directory.
 *
 *   THE HONEST HISTOGRAM at 725 (built-ins, then language): Array 204, Promise 65, TypedArrayConstructors 49,
 *   String 46, Map 39, RegExp 33, AsyncDisposableStack 29, DisposableStack 28, Set 27, RegExpStringIterator 16,
 *   Uint8Array 9, AsyncFromSyncIterator 7; language/statements 69, language/expressions 28.
 *   Array 204 IS localised now, and it is what the earlier sweep guessed and could not prove: js_bytecode_autoinit
 *   -> js_bytecode_eval -> JS_EvalFunction -> JS_CallFree on qjsc_builtin_array_fromasync. Array.fromAsync is the
 *   last SELF-HOSTED builtin, so first touch of the property runs a compiled bytecode PROGRAM from C, below the
 *   live flow that read the property. Routing the autoinit would be the shortcut; the root is that a builtin is
 *   written in JavaScript at all.
 *
 *   BUILT NEXT, AND THE CAPABILITY MATTERED MORE THAN THE COUNT: `catches_abrupt` on a CALL request.
 *   It existed only for a GETPROP (one arm, at the property-read unwind), so an algorithm that CATCHES a call's
 *   throw had no way to be a step machine at all — the machine was torn down before its step could see it. That
 *   is why DisposeResources was still a JS_Call loop: 27.3.3.3 step 3.d folds each throwing dispose method into a
 *   SuppressedError and keeps disposing, which is precisely "the throw is a value to this algorithm". The arm now
 *   exists at the call unwind, drops the DRIVE's own operands the way the async-from-sync and combinator arms
 *   above it do, and re-enters step() with JS_EXCEPTION live.
 *   Its first consumer is js_dispose_sync_def, and the conversion found a bug the count would never have shown:
 *   the old loop iterated ds->resources IN PLACE while calling the page's code, and a dispose method can call
 *   move() or use() on the very stack being disposed — reachable from script. The machine STEALS the list at
 *   stage 0 (the stack keeps nothing) and its fini releases the tail, so re-entrancy has nothing to disturb and an
 *   abandoned flow leaks nothing. DisposableStack 28 -> 4, corpus 725 -> 684, 0/43222.
 *   THE ASYNC HALF WENT WITH IT, in the same shape: js_dispose_async_def for the builtin (its FIRST dispose call
 *   is synchronous — the Await comes after it and test262 observes the difference, so it cannot be folded into
 *   the chain) and js_async_dispose_link_def for each chain closure, declared through promise_closure_set_step.
 *   Both C bodies are DFAILs. AsyncDisposableStack 29 -> 8, corpus 684 -> 653.
 *   AND THEN THE FORCING FUNCTION FIRED, which is the part worth keeping. With the outer loop finally a request,
 *   the very first fixture aborted: "loop preempted in a NON-coroutine activation". js_sync_dispose_wrapper —
 *   GetDisposeMethod's sync fallback on an async stack — was calling the object's %Symbol.dispose% with JS_Call
 *   from C. It had been doing that all along and NOTHING could see it, because the loop that reached it was
 *   itself a drive-to-completion; a second one nested inside the first is invisible to a counter that only knows
 *   "a bytecode body was entered by C recursion". Converting the outer is what made the inner nameable. It is a
 *   step machine now (js_sync_dispose_wrap_def), and its two construction sites collapsed into one
 *   js_new_sync_dispose_wrapper, because building the closure IS declaring it a machine and a second site is a
 *   second chance to forget. 653 -> 610.
 *   THE GENERAL FORM: a drive-to-completion HIDES the ones below it. Converting a loop does not only remove its
 *   own count, it exposes the calls it was masking — so a directory's number going down by less than expected
 *   after a conversion is the signal to look for what the conversion revealed, not evidence the conversion was
 *   partial.
 *
 *   PROMISE 65 -> 57, TWO OF ITS THREE CLOSES ROUTED. 27.2.4.1's IfAbruptCloseIterator runs the iterator's
 *   `return` — the page's code — and three sites ran it with JS_IteratorClose by C recursion. Two are now parked
 *   on the chain exactly the way an async-from-sync close is (JSIterClose with outer_kind CONT_PROMISE_ALL, the
 *   pending completion as its saved exception) and land on ONE new do_promise_all_reject label, which both the
 *   no-close path and do_iter_close_finish reach. The generator-throw teardown reuses that label verbatim,
 *   because its close and its reject were the same two steps written twice.
 *   WHY THE DEFERRED-CLOSE QUEUE COULD NOT SERVE THIS, since that was the obvious idea: its drain is the
 *   interpreter's exception label, and neither arm goes there — Promise.all(x) RETURNS a rejected promise rather
 *   than raising. A close whose caller does not unwind needs a CONTINUATION, not a deferral.
 *   THE THIRD IS NAMED, AND SO IS THE WAY IT FAILED, because the failure is the whole of what the next attempt
 *   needs. promise_all_err inside the CONT_PROMISE_ALL step dispatch differs from the other two in what follows
 *   the close: not a teardown but the FINALIZE drive (fin_arg/fin_is_reject/finalizing, then st = 2).
 *   IT WAS BUILT, REVERTED, AND THEN BUILT CORRECTLY — Promise is 57 -> 53 and all three closes are on the
 *   chain. The first attempt resumed at do_promise_all_step and cost five tests
 *   (Promise/{all,allSettled,any,race}/invoke-then-get-error-*, all "$DONE() not called"): `finalizing` makes
 *   js_promise_all_step return DONE immediately, so re-entering the STEP skips the finalize drive and the
 *   aggregate never settles. The fix is a label ON THE DRIVE (do_promise_all_finalize) — a change to the
 *   dispatch's control flow, not to the close. The flag and the saved_exc handling were right first time
 *   (UNINITIALIZED, because the rejection is already parked in fin_arg and 7.4.9 discards the close's own throw
 *   under an abrupt completion).
 *   ONE MORE THING THE SECOND ATTEMPT GOT WRONG AND THE TESTS DID NOT CATCH: the finalize block reads `s`, a
 *   LOCAL of the dispatch block, and the new jump enters that block from outside — so `s` was indeterminate on
 *   the new path. It passed anyway, because the register happened to still hold it. A goto INTO a block must
 *   re-derive every local it lands on; the label now does `s = (JSPromiseAll *)cont_st`, which is what every
 *   other entry sets. A test passing is not evidence that a jump is well-defined.
 *
 *   (was: PROMISE 65 IS LOCALISED, and it is NOT a builtin that needs converting — it is one line of the
 *   interpreter.)
 *   Backtrace (Promise/all/invoke-resolve-error-close.js): JS_CallInternal's CONT_PROMISE_ALL abrupt arm ->
 *   JS_IteratorClose -> JS_CallFree -> JS_CallInternal. The arm runs 27.2.4.1's IfAbruptCloseIterator INLINE,
 *   so the iterator's `return` method — the page's code — is entered by C recursion below the live flow.
 *   THE OBSTACLE, named so the next attempt starts from it: iter_close_defer (the deferred-close QUEUE that
 *   already exists for exactly this) will NOT work here unmodified. Its drain is the interpreter's exception
 *   label, and this arm does not take the exception path — it rejects the aggregate and continues (BREAK /
 *   do_return), because Promise.all(x) RETURNS a rejected promise rather than raising. So the close has to run
 *   on the tramp and the REJECT has to happen after it: a two-step continuation, not a deferral. The pieces are
 *   CONT_ITER_CLOSE / CONT_ITER_CLOSE_CALL / do_iter_close_deliver, which already express "drive a close on the
 *   chain and come back"; what is missing is that arm handing them its own follow-up.
 *
 *   ASYNCDISPOSABLESTACK'S RESIDUAL 8 IS THE PROTOTYPE READ AGAIN — js_disposable_stack_constructor ->
 *   js_create_from_ctor -> JS_GetProperty(newTarget, "prototype"), the same 10.1.14 [[Get]] the TypedArray view
 *   constructor's conversion built step_proto_from_ctor_run for. NINETEEN js_create_from_ctor call sites remain;
 *   the seven passing JS_UNDEFINED read nothing (10.1.14 takes the intrinsic), so twelve are real.
 *   BUILT: both constructors are CREATECTOR_DEF_FULL declarations now (js_disposable_ctor_def /
 *   js_async_disposable_ctor_def, one per class because the class id rides `arg`), with a precheck for step 1's
 *   "Constructor requires 'new'" — which the machine runs BEFORE anything is created and therefore before the
 *   prototype read. JS_NewGlobalCConstructorStep is the two-line registration sibling of
 *   JS_NewGlobalCConstructor/Magic; only the cproto differs. DisposableStack 4 -> 2, AsyncDisposableStack 8 -> 6.
 *   AND IT LEAKED ON THE FIRST TRY, which is the part to keep. The post-create body CONSUMES the object
 *   js_creatector_step made — its return value IS the machine's result — and the new body opened with
 *   `JSValue obj = js_dup(obj_)`, so every construction leaked the step's own reference. Nothing failed: the
 *   corpus stayed 0/43222 and both suites stayed green. The gc_obj_list walk in JS_FreeRuntime is what caught it,
 *   as a wall of [gcleak] lines under the ASan fixture run. Two lessons: READ the ownership contract at the site
 *   you are joining (js_weakref_ctor_body says `JSValue obj = (JSValue)obj_;` and that cast IS the contract), and
 *   a passing suite proves nothing about ownership — only the leak walk does, so it is not optional.
 *
 *   CORRECTING WHAT THIS FILE SAID ONE ENTRY AGO — "what this one needs is a step CONSTRUCTOR registration, and
 *   JS_NewGlobalCConstructorMagic has no step-ctor form yet" is wrong, and the way it is wrong is the lesson.
 *   CREATECTOR_DEF / CREATECTOR_DEF_FULL already ARE the generic OrdinaryCreateFromConstructor machine
 *   (js_creatector_step): the class id and the body's declared argument count ride `arg`, the post-create body is
 *   a plain C function, `precheck` carries a leading validation such as "Constructor requires 'new'", and
 *   new.target UNDEFINED is already handled as the call form. Array declares it. So each of the twelve is a
 *   DECLARATION, not a build — and writing "needs a new form" without grepping for the existing one is how a
 *   solved mechanism gets re-invented. Check whether the primitive exists before recording that it does not.
 *
 *   MAP 39 AND SET 27 WERE ONE ROOT AND ARE NOW ZERO: js_map_forEach drove its callback with JS_Call from C.
 *   Two probes, one backtrace each, the same frame — which is the argument for probing every directory before
 *   planning against any of them: two entries in the histogram, 66 counts, one builtin, one conversion.
 *   js_map_foreach_def / js_set_foreach_def differ only in `arg` (the MAGIC_SET bit), because the collection's
 *   class id and which operand the callback's first argument is both follow from it — the same declaration the
 *   C body took as a magic.
 *   THE LOCK IS THE INTERESTING PART. The C loop raised the current record's ref_count across the call and
 *   advanced the cursor only afterwards, because a callback is allowed to delete the entry it is looking at (and
 *   to ADD entries, which 24.1.3.5 then requires the same walk to visit). "Across the call" now means across a
 *   SUSPENSION, so the locked record and the cursor moved onto the state — and fini releases the lock, which is
 *   the path a C local never had to cover: an abandoned machine (the callback threw, or the flow was torn down)
 *   would otherwise leave a record pinned forever. The fixture that pins this deletes the current entry, deletes
 *   a later one, adds during the walk, delete-then-re-adds, and calls clear() from inside the callback.
 *
 *   STRING 34 -> 32, BUILT: the replacer's ToString is a request. 22.1.3.19 states the step as
 *   `? ToString(? Call(replaceValue, undefined, «searched, position, string»))` and the CALL became a request
 *   when the walk became a machine, leaving JS_ToString on its RESULT in C — so a replacer returning an object
 *   with a toString ran that toString below the live flow, once per match. js_str_replace_step now takes out_cb
 *   and cb_pending says WHICH of the two requests is outstanding (1 = the call, 2 = the ToString), because both
 *   suspend and the walk has to resume into the right one.
 *   STRING IS NOW 0. The residual 32 was js_string_toLowerCase's own receiver coercion —
 *   JS_ToStringCheckObject(this_val) from C, so `String.prototype.toUpperCase.call({toString(){for(;;){}}})`
 *   preempted with no flow base — across all four registrations (toLowerCase/toUpperCase and the locale pair).
 *   NOTHING WAS BUILT FOR IT: js_str_recv_step already owns "coerce the receiver to a string, optionally coerce
 *   an argument, then compute" for eighteen String methods, so this is two more STRRECV modes and a three-line
 *   compute arm. The body loses its coercion and DCHECKs that it is handed a string. The same DCHECK went onto
 *   js_string_toWellFormed_body, which was re-coercing a value its own machine had already coerced — harmless
 *   only because the value was always a string, and a live C coercion the instant anything handed it an object.
 *   CREATEHTML WENT WITH THEM, and it carried a FIDELITY BUG no test could have found. B.2.2.2.1 step 4.a is
 *   `? ToString(value)` — a PLAIN ToString — and the C body used JS_ToStringCheckObject, so `"x".fontcolor()`
 *   threw a TypeError where V8 returns <font color="undefined">x</font>, and `"x".anchor(null)` likewise. annexB
 *   is not in test262.conf, so the suite was never going to say so; the SPEC is the oracle and reading it while
 *   converting the site is what caught it. Thirteen contiguous STEPDEF ids (JS_CFUNC_STEP_DEF spends the
 *   builtin's magic on the step id, so the tag has to come from the id) over one js_str_html_defs array, and one
 *   STRRECV_HTML_HAS_ATTR bitmask rather than a second copy of the defs table — two tables that must agree are
 *   two chances to disagree.
 *   ONE JS_ToStringCheckObject CALL SITE REMAINS IN THE ENGINE, inside JS_ToQuotedString (JSON's own output
 *   path, whose input is already a string).
 *
 *   THE PATTERN, THIRD TIME NOW: reach for the existing machine before writing one. The create-from-ctor sites
 *   needed CREATECTOR_DEF, these needed STRRECV, and in both cases the first instinct was to build.
 *
 *   IT WAS ONLY 2 OF THE 34, which is the useful part of the number: the directory's residual is somewhere else
 *   again, and "the probe named this directory" never meant "this is all of it". A DFAIL names the FIRST caller
 *   the run reaches, and re-probing after each conversion is the only way to see the next.
 *
 *   (was: STRING 34 IS LOCALISED and it is INSIDE the machine the last conversion touched, one stage further on.)
 *   Backtrace (String/prototype/replaceAll/replaceValue-call-tostring-abrupt.js): js_str_replace_step ->
 *   JS_ToString -> JS_ToPrimitiveFree -> JS_CallFree. 22.1.3.19 is
 *   `? ToString(? Call(replaceValue, undefined, «searched, position, string»))` — the CALL became a request when
 *   the walk became a machine, but the ToString on ITS RESULT stayed a C call, so a replacer returning an object
 *   with a toString still runs that toString off the tramp. It needs a stage per iteration (step_tostring_run on
 *   the callback's result), which is a change to js_str_replace_step's return protocol, not to the prologue.
 *   THE LESSON IS THE ONE THE SYNC-DISPOSE WRAPPER TAUGHT, in a second shape: converting a call exposes what it
 *   was feeding. The prologue conversion took String 46 -> 34 and the residual is the very next line of the same
 *   algorithm.
 *
 *   THE THIRD BRANCH IS ROUTED (49 -> 37) AND IT DID NOT GET AN ARM OF ITS OWN. ta_buffer_ctor_ready was
 *   WIDENED to accept a typed-array source, so the one entry now covers the element-count, buffer-view and
 *   typed-array-copy forms and the machine's own tag test picks between them — which is the rule this file
 *   already states for selectors, applied to a predicate rather than to a legacy body. A second predicate beside
 *   the first would be the same question asked twice, and every shape one of them failed to answer would have
 *   gone silently to the C body. js_typed_array_constructor_ta split into js_ta_copy_finish (everything after
 *   the object exists, which invokes nothing) plus a create, exactly as the view branch split.
 *   The source's element count is read BEFORE the `prototype` getter runs, where the C body read it, and the
 *   existing re-validation after the read is what catches a getter that detached the source.
 *
 *   (was: TYPEDARRAYCONSTRUCTORS 49 IS THE PROTOTYPE READ IN THE THIRD BRANCH.) js_typed_array_constructor_ta ->
 *   js_create_from_ctor: 23.2.5.1 step 6.b's TYPED-ARRAY source arm. The view arm was routed (the split into
 *   js_ta_view_plan / js_ta_view_finish) and the CONSUME arm before it; this is the copy-from-another-typed-array
 *   branch, which has its own js_create_from_ctor and was never in either. Three branches, three separate reads —
 *   which is what "route the branch, not the builtin" costs, and it is still the right shape (a machine wrapped
 *   around the whole constructor cannot see which branch applies).
 *
 *   REGEXP 33 IS LOCALISED, and every directory in the histogram now is. The probe's chain is
 *   JS_ToLengthFree -> JS_ToNumberFree -> JS_ToPrimitiveFree -> JS_CallFree -> JS_CallInternal: an argument
 *   coercion run from C, so a `lastIndex` whose valueOf contains a loop has no flow base. 22.2.7.2 step 4 is
 *   `? ToLength(? Get(R, "lastIndex"))` and lastIndex is an ORDINARY WRITABLE PROPERTY, so the page can put any
 *   object there — this is not an exotic case.
 *   THREE SITES, all the same read: js_regexp_exec (73788), JS_RegExpDelete (74016) and
 *   js_regexp_string_iterator_next (74367). The engine already has step_toint64_run for exactly this, so like
 *   the create-from-ctor sites these are declarations of an existing sub-sequence rather than a build — but
 *   unlike those, the three callers are not yet step machines, which is the actual work.
 *   NOTE ON THE PROBE ITSELF: the backtrace gdb printed stopped at JS_ToInt64SatFree and never named the
 *   caller — the frame that matters. `bt` alone was not enough here; the next probe of a coercion chain should
 *   ask for `bt 25`. The three sites above were found by grepping JS_ToLengthFree in the RegExp range instead,
 *   which worked because the coercion is rare in C by now; that will not stay true.
 *
 * AsyncDisposableStack
 *   29 / DisposableStack 28 ARE localised: js_disposable_stack_dispose drives each resource's dispose method with
 *   JS_Call from C and chains the rest through Promise.then, so the FIRST dispose is a drive-to-completion.
 *
 * WHY THIS LIST IS WORTH MORE THAN ONE MORE CONVERSION: the histogram is cheap (a bulk counter run per directory)
 * and a DFAIL names ONE caller, so the expensive part is the pairing, and it decays as soon as anything lands.
 * Recording it with the shapes is what keeps the next pass from re-deriving it — and re-deriving it is exactly the
 * mistake that produced the wrong "the residual is all Array.fromAsync" claim earlier in this file.
 *
 * THE MODULE GRAPH'S DEPTH IS NO LONGER THE C STACK'S. Four separate walks over that graph recursed in C, and
 * going after the largest drive-to-completion root is what found them — the walk that performs it could never be
 * suspended while it was C recursion, so flattening is that conversion's PREREQUISITE and not a substitute for it.
 * Each had a different failure mode, which is why one fixture found them one at a time:
 *   js_resolve_module — recurses OUT through the embedder's loader and back in (resolve loads, the loader
 *   compiles, compiling resolves), so it blew FIRST, at depth 473, and surfaced as the PARSER's "Maximum call
 *   stack size exceeded" — an error naming nothing to do with modules. It is a frame stack now, and the
 *   re-entrant call the loader makes is a no-op: the outer walk sees the loaded module through rme->module and
 *   descends at exactly the point the recursion did, so the load ORDER is unchanged.
 *   js_create_module_function — NO overflow guard at all. 32649 frames and then a SEGFAULT inside malloc. Worse
 *   than a bound: a crash.
 *   js_inner_module_linking and js_inner_module_evaluation — js_check_stack_overflow, i.e. a synthetic RangeError
 *   on a graph the page is entitled to have. Both are Tarjan walks; `index`, the dfs/ancestor indices and the
 *   per-child bookkeeping are threaded exactly where the recursion threaded them, with the pre-order, per-child
 *   and post-order halves split into functions so the frame stack can sit between them.
 * A FIFTH walk was found by pushing the fixture deeper, and it is the reason to push a fixture past the point it
 * passes: js_resolve_export1 forwards an INDIRECT export (`export { v } from "m"`) with a TAIL call, so a chain of
 * N re-exports was N C frames — and the chain fixture is exactly that shape, so at 40000 it still segfaulted after
 * the other four were flattened. A tail call is a loop; the star-export case beside it is a real branching walk and
 * stays recursive. The resolve state already carries the visited set, so the cycle guard is untouched.
 * THE LESSON IS ABOUT THE FIXTURE, not the walk: 6000 passed with four of the five fixed, and stopping there would
 * have shipped the claim "the module graph's depth is not the C stack's business" with a counter-example one
 * order of magnitude away. A depth fixture that passes proves nothing until it is raised until it fails.
 *
 * WHAT THIS DOES NOT DO, stated so it is not mistaken for done: the linker still performs step 9's
 * `JS_Call(m->func_obj, JS_TRUE)` from C, and SyncDriveToCompletion is unchanged at 1942. Routing that call needs
 * module evaluation to be a FLOW; what changed is that the walk around it can now hold a cursor instead of a C
 * frame, which is the thing that made routing impossible before.
 *
 * A CONTAINER-LEVEL SCARE worth recording as PROCESS. Mid-diff the local checkouts had reverted to an older
 * snapshot: main at 1c54288, the submodule at e563763, and this session's base commit 8be22e0 not even an object.
 * The rule held — check the tree, never assume — and the FIRST command was `git ls-remote`, not a reset: origin had
 * main = f54fc9f and apiclient-v2 = 44e000b, both this session's. `merge-base --is-ancestor` then proved local was
 * STRICTLY BEHIND and `origin/main..main` was empty, so `reset --hard` could not discard anything. Nothing was
 * lost. The lesson is the ORDER: establish what the remote has and prove the ancestry BEFORE any destructive
 * command, because "my work is gone" and "this checkout is stale" look identical from the working tree and have
 * opposite remedies.
 *
 * KNOWN FIDELITY GAP, carried forward deliberately and NOT introduced here: the zipper's `next` and `return` are
 * OWN, enumerable properties of the instance, which is what the object-literal the self-hosted version returned
 * produced. The spec's CreateIteratorFromClosure result takes them from %IteratorHelperPrototype% (which IS the
 * zipper's [[Prototype]] — result-is-iterator.js pins that and passes). Closing it means teaching the iterdrive
 * machinery about an N-source generator, which is a fidelity change with its own tests and must not be bundled
 * into a JS->C conversion. It now covers BOTH zippers, so the fidelity fix is one change to one drive.
 *
 * THAT CLAIM WAS WRONG AND MEASURING IT IS THE LESSON. "The residual is all Array.fromAsync" was written from ONE
 * backtrace and never checked; re-deriving the per-directory histogram showed language/ at 1095, then
 * TypedArrayConstructors 265, Array 204, Promise 66, String 64, Map 39, RegExp 33, AsyncDisposableStack 29 —
 * fromAsync is a MINORITY of it. A bulk counter gives a number and a DFAIL gives ONE caller; only the histogram
 * gives a distribution, and the earlier sweeps had that discipline while this sentence did not. The named roots
 * now, each with a backtrace: js_inner_module_linking's `JS_Call(m->func_obj, JS_TRUE)` (the module graph's
 * "initialize the global variables" pass, run from C for every module — the 1095), js_typed_array_constructor's
 * JS_ToIndex on its length argument, js_string_includes's JS_ToInt32Clamp on its position, and the
 * AsyncDisposableStack disposal loop's JS_Call on each dispose method.
 *
 * ONE SELF-HOSTED BUILTIN REMAINS: builtin-array-fromasync.js. It is the
 * hardest of the three — an `await` inside a loop and a try/finally — so it needs a C step machine that AWAITS,
 * which no builtin here does yet (.finally chains reactions; that is the nearest precedent). Building it takes
 * js_bytecode_eval, js_bytecode_autoinit, JS_AUTOINIT_ID_BYTECODE, the qjsc blob rule, the JS_ReadObject-at-init
 * path and js_call_function (with the tramp_is_call_function call-site reshape that exists only for it) with it —
 * so the counter reaching 0 and the last recognizer disappearing are the same diff.
 *
 * THERE IS NO INLINE JS_IteratorClose LEFT IN THE ENGINE EITHER. The last one was in js_map_constructor's
 * `fail:` label, on an `iter` that could never be an object — residue of the deleted C acquire, together with
 * 24.1.1.1 step 6's `? Get(map, "set")` read and the iterator locals. All of it sat DOWNSTREAM of the DFAIL for
 * an unrouted construct shape: the adder read ran first, but only on a path that then aborts. Being unreachable
 * is not being absent — a second copy of a step whose real implementation lives on the consume machine is the
 * legacy twin the ban is about, and the way it survived is that a teardown label kept it compiling.
 *
 * THERE IS NO C-DRIVEN PROXY TRAP LEFT IN THE ENGINE. Every one of the thirteen internal methods is a DFAIL plus a
 * visible release failure, and the only implementation is the routed one.
 *
 * Two lessons worth keeping:
 *
 *   THE ANSWER WAS USUALLY A WALKER, NOT A SITE. tramp_proto_proxy / tramp_accessor_getter / tramp_accessor_setter
 *   were each guarded at their ~18 call sites by `JS_VALUE_GET_TAG(x) == JS_TAG_OBJECT`, and that one guard,
 *   repeated, is what left EVERY primitive-base read and write running the page's code from C. The fix was to give
 *   the walkers the base VALUE and one tramp_walk_base — and the first attempt, which routed the two write opcodes
 *   explicitly instead, was wrong for exactly the case tramp_walk_base exists to state: a String primitive answers
 *   its own canonical indices and `length` ITSELF, so starting the walk at String.prototype would let a Proxy above
 *   it answer for a character the string owns. A per-site guard is not a small duplication; it is the gap.
 *
 *   AN ORDERING BUG TRAVELS WITH THE C CALL. Three of these callers had the spec's order wrong as well as the
 *   activation: propertyIsEnumerable coerced the key AFTER ToObject, and both array-element opcodes let a NULLISH
 *   base fall through to a C entry that coerces the KEY first — so `u[{toString(){…}}] = 1` ran the page's toString
 *   from C and ran it before the TypeError 6.2.5.5 step 3.a raises. The C entry does its own steps in its own
 *   order; routing is what forces the spec's.
 *
 * FIXED, the turn after it was named: the TypedArray CONSUME arm's js_create_from_ctor (23.2.5.1 step 6.a.i,
 * `new Int8Array(iterable)` with a Proxy new_target). CONT_TA_TARGET parks the consume SETUP across the
 * `prototype` read, which is what naming it as "a continuation like CONT_CTOR_PROTO with a different resumption
 * point" had already worked out. The machine's own setup moves ABOVE the read — it is pure state assignment with
 * nothing observable in it — so the state carries only the machine, new_target, the probed @@iterator and the
 * class id, and the abandon path is the machine's OWN teardown (it holds the argument list and the requester).
 *
 * FIXED, the same turn: the Map/Set CONSUME arm ran TWO of the page's operations from C — js_map_constructor's
 * `prototype` read and 24.1.1.1 step 5's `Get(map, "add"/"set")`, which a subclass can make an accessor.
 * CONT_SETMAP_CTOR is that two-read sequence with a phase, and building it needed the [[MapData]] slot to become
 * its own function (js_map_state_init): the routed path creates the object from a routed read, so the allocator
 * and the slot could no longer be one line inside js_create_from_ctor's caller. Same shape as the TypedArray
 * arm — the machine is built first, because everything it takes from the smc_* registers is pure state assignment
 * and those registers do not survive the reads.
 *
 * FIXED, the same turn: the APPLY trampoline's build_arg_list — 19.2.3.1 CreateListFromArrayLike, whose step 3 is
 * `? LengthOfArrayLike(obj)` and step 5 a `? Get(obj, index)` PER ELEMENT. CONT_ARG_LIST is the first of these
 * sequences whose length is unknown when it starts, so it carries a CURSOR and the half-built list, and it drives
 * THREE request kinds (the `length` read, that value's ToPrimitive, and one read per element). The three
 * build_arg_list calls inside do_apply_tramp collapse into ONE, hoisted ABOVE the question of what kind of target
 * this is: the list is an operand, so nothing about the callee decides how to read it.
 *
 * A FAST ARRAY still builds its list in C, and that is not a fallback — arg_list_is_fast asks whether the OPERAND
 * can answer without invoking anything (a fast array's `length` is an own data slot equal to its element count and
 * every element is a slot read), so the C loop reaches the same answer with no request rather than standing in for
 * a routed path. The test is on the operand, not on the callee, which is the difference between a capability
 * question and a recognizer.
 *
 * A correction the fixture forced, worth keeping because the earlier wording here had it wrong: the SPREAD is NOT
 * this operation. `f(...x)` is GetIterator-based, so an array-like with no @@iterator is not spreadable at all;
 * only .apply / Reflect.apply and the construct argument LIST take CreateListFromArrayLike.
 *
 * FIXED beside it, found by the same armed run: 23.1.3.9 step 5.b's `? Get(O, Pk)` in
 * find/findIndex/findLast/findLastIndex. Its CALLBACK was already routed and its ELEMENT READ was not, which is
 * the residue to expect in an already-converted machine — the loop had one JS_GetPropertyValue left in it.
 *
 * THREE of the six went as SUPERSEDED BODIES rather than as conversions, which is the other way a C caller of the
 * page's code disappears: js_function_apply, js_reflect_apply (which only forwarded to it) and
 * js_reflect_construct were all unregistered — Function.prototype.apply, Reflect.apply and Reflect.construct are
 * step machines — and every one of them still built its argument list with build_arg_list. A body nothing is meant
 * to reach is not harmless while it compiles: js_function_apply had ONE live use left, the OP_apply residue, and
 * through it a `length` getter that loops preempted with no flow base. That residue is two throws in spec order
 * (19.2.3.6 step 1's IsCallable, then 19.2.3.1 step 2's "not a object"), so the opcode performs them itself and
 * the body could go.
 *
 * A PROCESS note, because it cost the file: deleting a function by regex-matching its signature down to the next
 * `\n{\n` matched a FORWARD DECLARATION and deleted 418 lines across unrelated functions. `git checkout` put it
 * back — the tree was at a pushed commit, which is the only reason nothing was lost — and the redo anchored on
 * exact LINE RANGES with an assertion on both endpoints. A destructive edit needs its endpoints asserted, not
 * matched.
 *
 * The construct-spread list followed, and it is what turned CONT_ARG_LIST into a shared sequence rather than the
 * apply trampoline's own: the RESUMPTION POINT became a field on the state (ARGL_TO_APPLY / ARGL_TO_CONSTRUCT)
 * instead of a second continuation. That is the right shape because the list is an OPERAND — reading it is the
 * same algorithm whoever asked for it, and only where the finished list goes differs.
 *
 * build_arg_list IS DELETED, and the last three callers went three DIFFERENT ways, which is the point worth
 * keeping: a C caller of the page's code does not always become a conversion.
 *
 *   TWO were UNREACHABLE. The generator-via-apply reshapes (`g.next.apply(gen, args)` and
 *   `Reflect.apply(g.next, gen, args)`) sat below the general `.apply` / `Reflect.apply` routes, which test the
 *   SAME operand shapes and jump to do_apply_tramp first — so nothing could fall that far. They had been added
 *   because deleting the general routes broke a generator target; the general routes stayed, and these became
 *   residue nobody noticed. Reachability was settled by DELETING them and running the corpus and every fixture,
 *   not by reading the control flow twice.
 *
 *   ONE was a DRIVE-TO-COMPLETION hiding behind the list. OP_apply_eval's `eval` that resolves to something else
 *   is an ordinary call with a spread argument list, and it ran that callee with JS_Call FROM C — a bytecode
 *   body's loop preempted with no flow base, a step-machine callee reached its C entry. Routing it to
 *   do_apply_tramp fixes both halves at once, because that label already knows how to read a list. The reachable
 *   spelling in BOTH modes is replacing the GLOBAL binding (`var eval = f` is a SyntaxError in strict code), which
 *   is what the fixture had to be rewritten to use.
 *
 *   The DIRECT-eval half keeps its C read and ASSERTS why it may: the operand is the spread's own array, which the
 *   compiler emits, so arg_list_is_fast must hold. A DCHECK says so in dev and a visible InternalError says so in
 *   release — never a silent fall-back to a routed read, because if that assert ever fires the right answer is to
 *   give this site a resumption kind, not to paper over it.
 *
 * That leaves CreateListFromArrayLike existing EXACTLY ONCE in the engine, as CONT_ARG_LIST plus the
 * arg_list_is_fast / arg_list_fast_build pair for the compiler-built lists that invoke nothing.
 * The other js_create_from_ctor callers
 * (Object, RegExp, Map/Set, DisposableStack, Promise, the two TypedArray constructors) were not reached by the
 * armed corpus, which is evidence and not proof: each is a C constructor that would fire the moment a test
 * constructs it with a Proxy new.target, and each is converted the way Boolean was — the create-from-ctor
 * DECLARATION, whose CREATECTOR_WRAP bit now also states the wrapper kind whose CALL form answers with a primitive.
 *
 * A THIRD sweep, over OPERATOR and SYNTAX surfaces (destructuring, spread, optional chaining, `in`/`delete`
 * through a proxy chain, private brands, tagged templates, class heritage, labelled break with a proxied
 * `return`), found four. All four are fixed — each was NAMED here first, with its backtrace, so the attempt
 * started from building rather than from finding:
 *
 *   FIXED — `class C extends P` read P.prototype with JS_GetProperty from js_op_define_class. Now
 *           CONT_DEFINE_CLASS, and `prototype` is measurably the ONLY property a class definition reads off its
 *           heritage.
 *
 *   FIXED — ArraySetLength's two coercions, in set_array_length via JS_ToInt32/JS_ToNumber:
 *
 *       var a = [1,2,3]; a.length = { valueOf() { for(;;){} } }
 *
 *     10.4.2.4 steps 3-4 are ToUint32(V) then ToNumber(V), BOTH on the ORIGINAL V, which is why
 *     JS_ToArrayLengthFree converts twice and compares. Now CONT_ARRAY_LEN. Four things this one taught, each
 *     paid for by a measurement:
 *
 *     IT IS NOT the `value_tonum_toprim` shape. That idiom coerces V on the tramp and RE-EXECUTES the opcode, so
 *     the write sees a primitive and the C coercion inside it runs nothing. It is right for
 *     TypedArraySetElement, which coerces V ONCE. Substituting a primitive here collapses two observable valueOf
 *     calls into one — `var n = 0; [].length = { valueOf() { n++; return 0 } }` must leave n === 2 — and a first
 *     attempt made it 1 and was reverted rather than landed, because a wrong answer is worse than a missing
 *     capability.
 *
 *     WHAT THE SEQUENCE PRODUCES IS THE VALIDATED uint32, and the operation is then RE-ISSUED with that in place
 *     of V — which is literally 10.4.2.4 step 6, so the re-run's own conversions are of a number and invoke
 *     nothing. That is why the state is small: it is not a re-implementation of the write, it is two coercions
 *     and a re-issue. The re-issued request BORROWS every operand and the state stays alive as their owner,
 *     nesting one level exactly as a [[GetOwnProperty]] nests inside its descriptor walk; handing ownership to
 *     the borrowed registers instead leaked the array and the atom on every length write.
 *
 *     THE DCHECK FOUND A FOURTH SPELLING. Three were predicted — `a.length = obj` (OP_put_field),
 *     `Reflect.set(a, "length", obj)` and `Object.defineProperty(a, "length", {value: obj})` (the keyed entry's
 *     GP_SET / GP_DEFINE arms). `a[k] = obj` with k === "length" (OP_put_array_el) was not, and only the
 *     unconditional DCHECK at set_array_length named it, after the other three were routed. A predicted list of
 *     spellings is not a route; an assert at the destination is.
 *
 *     AND THE TWO TEARDOWN WALKS ARE MUTUALLY RECURSIVE, which was only half built. getprop_throw and
 *     do_getprop_abandon already hand a CONT_TOPRIM_GET link to js_toprim_abandon; a coercion requested BY a
 *     keyed operation hands the link back, and the first version of the ARRAY_LEN arm freed the sequence and
 *     dropped its waiter behind a comment claiming "its own waiter is released by whichever teardown owns it".
 *     Nothing owned it — `[].length = {valueOf(){throw}}` leaked the write's JSOpKeyed, which is a fifth false
 *     narrowing claim written beside a C call, this time by the same hand as the fix. The return direction has to
 *     be deferred because only one of the two walks is a function, so it is parked in ctx->pending_gp_unwind and
 *     drained at the exception label every caller reaches — the same shape as pending_import_cap and
 *     pending_close_iter, and ONE drain rather than the seven abandon sites an inline unwrap would have needed.
 *     The inline unwrap at toprim_throw that predated it is deleted.
 *
 *   FIXED — 20.5.3.4 Error.prototype.toString ran all four of its observable operations from C. Now
 *     STEPDEF_ERROR_TOSTRING.
 *
 *   FIXED, BY DELETING THE READ — 20.2.3.5 Function.prototype.toString read `name` off the function, and step 5
 *     makes the representation of a function with no [[SourceText]] IMPLEMENTATION-DEFINED, so that read was
 *     never required: the name in it is this engine's choice and JS_GetProperty made the choice the page's code.
 *     The own data property is the same string for every function that reaches there. Not every C-side read is a
 *     conversion waiting to be routed — some are gratuitous, and the spec is what says which.
 *
 *   A CONSEQUENCE worth recording, because it will recur with every remaining conversion: making a builtin's
 *     `toString` a machine turns each HOST-side JS_ToCString on an object into the backstop's abort — three of
 *     them in run-test262.c alone. That is right rather than unfortunate. JS_ToCString invokes the page's
 *     `toString`, a C entry has no flow to run it on, and a test runner reporting what went wrong must not
 *     depend on the code that went wrong. All three build a diagnostic out of PROPERTIES instead. The same will
 *     be true of any embedder, and the engine names it every time.
 *
 * What did NOT reproduce, recorded so it is not re-probed: RegExp.escape, Uint8Array.fromBase64 and
 * .setFromBase64 reject a non-String at step 1 with no coercion at all, so an object argument is a TypeError
 * rather than a C-side ToString.
 *
 * The pattern across the sweeps, worth stating because it recurred four times: the gap was never in the
 * mechanism, it was in a NARROWING CLAIM written beside a C call — "a data property in every real case", "the
 * looping traps abort only because the receiver check happens to run before them", "this cannot be a
 * coerce-then-compute declaration" where it could, and "it IS one" where it could not. Each was checked by
 * running it, and three of the four were false.
 *
 * THE THIRD internal method of this family, and the last one still measured only in prose. Every JS_HasProperty
 * is a C-side [[HasProperty]]: a shape-and-prototype walk on an ordinary object, and the page's `has` trap the
 * moment anything on that chain is a Proxy.
 *
 * The live consumer was the OBJECT ENVIRONMENT RECORD — `with` — and it is DONE. HasBinding step 2,
 * @@unscopables' two reads, GetBindingValue/SetMutableBinding's SECOND HasProperty and the access itself are
 * all requests, across three opcode groups that share one record: the OP_with_* family and the
 * OP_get_ref_value / OP_put_ref_value pair a reference is finished by. 19 -> 15.
 *
 * The GLOBAL Environment Record followed, and it is the same record one link along: 9.1.1.4's ObjectRecord half
 * IS an object Environment Record over `global_obj` with withEnvironment false, so it shares the machine with
 * @@unscopables switched off rather than owning a second copy. Routing OP_get_var/OP_get_var_undef retired
 * JS_GetGlobalVar, whose tail ran a global ACCESSOR's getter and any Proxy on the global object's prototype
 * chain from C. Two more bodies went with it, both left standing when their last caller was routed: js_operator_in
 * (OP_in has been the keyed entry's GP_HAS for some time) and js_has_unscopable. A superseded body that keeps
 * compiling is the fallback this file exists to forbid, whether or not anything still reaches it. 15 -> 14.
 *
 * 9.1.1.4.5 SetMutableBinding followed the read, retiring JS_SetGlobalVar, whose tail ran a global SETTER and
 * any Proxy on the global object's prototype chain from C. The delivery stopped re-deriving the base object,
 * the with-body jump and the written value from LISTS OF OPCODES and reads them off the record instead, which
 * is what collapsed the reference branch and the OP_with_* branch of 9.1.1.2.6/9.1.1.2.5 into the one
 * algorithm they always were. 14 -> 13.
 *
 * 9.1.1.4.7 DeleteBinding and GetIdentifierReference finished the record: OP_delete_var and OP_make_var_ref
 * route their HasBinding too, and JS_DeleteGlobalVar and JS_GetGlobalVarRef are deleted. All four of the
 * global Environment Record's operations are the machine's now, and NONE of the global object's own reads is
 * a claim any more — each is an own-property answer on a receiver a DCHECK asserts is not a Proxy. 13 -> 11.
 *
 * 10 -> 4. js_obj_to_desc's SIX HasProperty reads went with it when the C [[GetOwnProperty]] hook's body was
 * deleted — the ratchet had them listed as "reach only ENGINE-BUILT objects... and go when those do", and that is
 * exactly what happened, though by DELETION rather than by the conversion the note expected. What is left is the
 * import-attributes record, the two regexp `groups` reads and js_proxy_has's own forward, all of which go when the
 * remaining hook bodies do — and 4 -> 3 the moment they did, with js_proxy_has's own forward to the target. What
 * is left reaches only ENGINE-BUILT records: the import-attributes record and the two regexp `groups` reads.
 *
 * 11 -> 10. JS_TryGetPropertyInt64 — the has-then-get pair every array builtin used to walk a sparse source —
 * had lost its last caller when those builtins became step machines over step_hasidx_run and step_getidx_run, and
 * the body kept compiling. A superseded body that still compiles is the fallback this file forbids whether or not
 * anything reaches it, and while it stood the ratchet counted it as a consumer still to convert, which is a
 * conversion that can never be made because there is nothing to convert. Deleted.
 *
 * The ten that remain reach only ENGINE-BUILT objects: js_obj_to_desc's own six pairs (its last caller is
 * the Proxy getOwnPropertyDescriptor trap-result read, so it is reached only from a C-side [[GetOwnProperty]]
 * and goes when those do), a regexp groups record, an import-attributes record. Saying a site is safe is
 * weaker than asserting it.
 * 10 = the call sites, excluding the definition. */
const hasFromC = (src.match(/JS_HasProperty\(/g) || []).length
  - (src.match(/^int JS_HasProperty\(/gm) || []).length;
if (hasFromC !== 3) {
  console.error(`C-side [[HasProperty]] call sites: ${hasFromC}, expected 3.`);
  console.error(hasFromC > 3
    ? `  A new C caller can reach a Proxy's has trap with no flow base.`
    : `  One was routed: LOWER the count in engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}

const descReads = (src.match(/JS_(Get|Has)Property\(ctx, desc, /g) || []).length;
if (descReads !== 1) {
  console.error(`ToPropertyDescriptor C-side reads: ${descReads}, expected 1.`);
  console.error(descReads > 1
    ? `  A descriptor walk is reading fields from C again. It is the page's code — run it through JSDescCursor.`
    : `  js_obj_to_desc shrank: LOWER the count in engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}

if (names.size > CEILING) {
  console.error(`recognizer ratchet FAILED: ${names.size} > ceiling ${CEILING}`);
  console.error(`  names: ${[...names].sort().join(' ')}`);
  console.error(`A detector was added back. Delete the legacy body it chooses against; the detector then has`);
  console.error(`nothing to choose. CLAUDE.md: legacy body FIRST, detector SECOND.`);
  process.exit(1);
}
if (names.size < CEILING) {
  console.error(`recognizer ratchet: ${names.size} < ceiling ${CEILING} — LOWER CEILING to ${names.size} in ` +
                `engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}
console.log(`recognizer ratchet ok: ${names.size}/${CEILING} recognizers, ${callSitePredicates} call + ` +
            `${constructSitePredicates} construct convergence point, ${modeWrites} per-site mode writes, ` +
            `iterator-protocol C reads done/value/next 0/0/0, ToPropertyDescriptor C reads ${descReads}, ` +
            `C enum-only key walks ${enumOnlyCallers}, C-side [[GetOwnProperty]] ${gopdFromC}, ` +
            `C-side [[IsExtensible]] ${extFromC}, C-side [[HasProperty]] ${hasFromC}`);
