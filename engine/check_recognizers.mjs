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
 * Counted as JS_GetProperty/JS_HasProperty against the atom, which is the shape every one of them had —
 * EXCLUDING js_obj_to_desc, whose `value` is a property DESCRIPTOR's, not an iterator result's. That one is a
 * live C-side page-code read too (a Proxy getOwnPropertyDescriptor trap can return a descriptor with an accessor
 * `value`), but it belongs to ToPropertyDescriptor and converts with that walk, not with this family. Excluding
 * it by BODY rather than by raising the ceiling is what keeps a re-added iterator-result read visible. */
const descStart = src.indexOf('static int js_obj_to_desc(');
if (descStart < 0) {
  console.error('js_obj_to_desc is gone — remove its exclusion from the iterator-read ratchet.');
  process.exit(1);
}
const descEnd = src.indexOf('\n}\n', descStart);
const iterSrc = src.slice(0, descStart) + src.slice(descEnd);
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
 * js_obj_to_desc is what remains, reached through the C [[GetOwnProperty]] HOOK. THAT CLAIM USED TO SAY routing
 * JSON.stringify and for-in would remove it. Both are routed now and it is still reachable, so the claim was
 * WRONG and is corrected here rather than quietly left standing:
 *
 *     var inner = new Proxy({q:1}, {getOwnPropertyDescriptor(t,k){ <loop>; return Reflect.getOwnPropertyDescriptor(t,k) }});
 *     Object.getOwnPropertyDescriptor(new Proxy(inner, {getOwnPropertyDescriptor(t,k){...}}), "q")
 *
 * aborts today. The consumer IS routed — the outer trap runs on the chain — but 10.5.5 steps 10 and 12 are
 * `target.[[GetOwnProperty]](P)` and `IsExtensible(target)`, and js_proxy_gopd_pre performs BOTH from C. When
 * the target is itself a Proxy those are its traps. Every other proxy invariant has the same shape, which is
 * why the count below is over the whole family and not over this one function.
 *
 * 13 = js_obj_to_desc's twelve (six fields x HasProperty + Get) plus ONE `enumerable` read, shared by both
 * enumerable-key walks through js_desc_object_is_enumerable. That one is safe by construction — every
 * GP_GETOWNPROP delivery rebuilds the record through js_desc_to_object — and asserts it with
 * js_read_is_page_code rather than claiming it in a comment. A second walk needing the same read is a reason to
 * share the site, never to raise this. It may only go down. */
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
if (gopdFromC !== 10) {
  console.error(`C-side [[GetOwnProperty]] call sites: ${gopdFromC}, expected 10.`);
  console.error(gopdFromC > 10
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
 * 15 = the call sites, excluding the definition. */
const extFromC = (src.match(/JS_IsExtensible\(/g) || []).length
  - (src.match(/^int JS_IsExtensible\(/gm) || []).length;
if (extFromC !== 15) {
  console.error(`C-side [[IsExtensible]] call sites: ${extFromC}, expected 15.`);
  console.error(extFromC > 15
    ? `  A new C caller can reach a Proxy's isExtensible trap with no flow base.`
    : `  One was routed: LOWER the count in engine/check_recognizers.mjs so the gain cannot be given back.`);
  process.exit(1);
}

const descReads = (src.match(/JS_(Get|Has)Property\(ctx, desc, /g) || []).length;
if (descReads !== 13) {
  console.error(`ToPropertyDescriptor C-side reads: ${descReads}, expected 13.`);
  console.error(descReads > 13
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
            `C-side [[IsExtensible]] ${extFromC}`);
