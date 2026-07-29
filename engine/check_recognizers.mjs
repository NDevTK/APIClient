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

/* The ceiling counts tramp_can_call_* recognizers. It goes DOWN whenever a conversion deletes a legacy JS_Call-LOOP
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
const CEILING = 7;              // tramp_can_call_* — down with each conversion; up only for a new reject-and-yield builtin
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
for (const m of src.matchAll(/^static bool (tramp_can_call_[a-z_0-9]+)/gm)) names.add(m[1]);

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
   tramp_body_entry and nowhere else; a re-appearing per-site copy fails here. */
const bodyPreds = ['tramp_can_call_async', 'tramp_can_call_agen_create'];   /* used ONLY by tramp_body_entry */
for (const p of bodyPreds) {
  const uses = (src.match(new RegExp(`${p}\\(`, 'g')) || []).length;
  if (uses !== 2) {   /* the definition + the single use inside tramp_body_entry */
    console.error(`body-entry drift: ${p} appears ${uses} times, expected 2 (its definition + tramp_body_entry).`);
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
            `${constructSitePredicates} construct convergence point, ${modeWrites} per-site mode writes`);
