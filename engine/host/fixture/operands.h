/* THE OPERAND SHAPES DOCUMENT: disposal, rest parameters, accessors, constructors and rejected heads.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_OPERANDS_H
#define ENGINE_HOST_FIXTURE_OPERANDS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_OPERANDS[] =
    "<!doctype html><html>"
    "<body>"
    "<script>var cfg = { admin: state.admin };</script>"

    /* ─── THREE OPERAND-SHAPE STATEMENTS, APPENDED RATHER THAN INSERTED ────────────────────────────────────
       THE APPEND IS THE POINT AND IT IS NOT TIDINESS. This document is ONE LINE with no newlines, so a `@WHY`
       frame's COLUMN is the only coordinate a reader has into it, and three separate investigations located
       their defect by mapping one into this string. An insertion moves every column after it, which does not
       break a run — it breaks the READER, silently, in the direction of confidence: the column still lands
       inside a statement, just not the one that produced the frame. Appending in front of `</body></html>`
       moves nothing: every script above keeps its own text, its own byte offset in this literal and its own
       internal columns, and the new script's columns start where no old coordinate pointed.
       WHAT THE THREE HAVE IN COMMON is the contract quickjs's height and idle asserts are about: the operand
       shape a site DECLARES versus the shape execution actually takes. Each one is a path whose declared shape
       was wrong and whose only symptom was a value read one slot off — none of them crashes at the site, and
       none of them is reached by any other statement in this document. */
    "<script id=stk>"

    /* (1) A `for await` HEAD THAT REJECTS. ECMAScript §14.7.5.7 ForIn/OfBodyEvaluation steps 8.a-8.d are `?`,
       so the head runs under a catch offset of its own whose handler drops the enum_rec and rethrows. A catch
       offset's slot is its handler's operand depth, so anything that MOVES the offset after `OP_catch` (the
       async head did: `OP_await` wants the promise on top) runs the handler at a depth compute_stack_size did
       not record.
       ONLY A REJECTED HEAD REACHES THE HANDLER: `next()` returns a rejected promise, so the throw arrives
       THROUGH the await. `after` is computed in the same frame after the catch, so a handler that left the
       stack one slot off crashes here or writes a different number. `Symbol.asyncIterator` is assigned rather
       than a computed key, so the statement asks about the head lowering alone. */
    "(async function(){"
    " var _fawi = {}; _fawi[Symbol.asyncIterator] = function(){"
    "   return { next: function(){ return Promise.reject(new Error('fawREJ')); } }; };"
    " var _fawv = 'noreject';"
    " try { for await (var _fawx of _fawi) { _fawv = 'body' + _fawx; } } catch (_fawe) { _fawv = _fawe.message; }"
    " fetch('/api/faw?v=' + _fawv + '&after=' + ((1 + 2) * (3 + 4)));"
    "})();"

    /* (2a) A STEP BUILTIN'S CALLBACK WITH A REST PARAMETER — the argument shape that BUILDS an object out of
       the slots and so can observe whose storage a callback frame's arguments are. `narg_alloc == 0` borrows
       the slots in place, sound only for the caller's operand stack; a C-builtin callback's slots are the
       continuation record's own buffer. The leak of a rest array over borrowed slots is reported by the
       `gc_obj_list` walk in `JS_FreeRuntime`; this row states the callback RAN with its arguments intact.
       THREE ARGUMENTS ASSERTED AS THREE: `forEach` passes (element, index, array), so `n` is 3 and `t` the
       array's type; `e` carries two values because the callback runs twice.
       THIS IS THE NON-ZERO CLAUSE: a three-argument callback takes the clause that raises the allocation to
       the argument count before the borrow; (2b) is the zero-argument shape that still reaches the branch
       where the pointer is decided. */
    "[1,2].forEach(function(..._fr){"
    " fetch('/api/rest?n=' + _fr.length + '&e=' + _fr[0] + '&t=' + (typeof _fr[2])); });"

    /* (2b) A FORKING ZERO-ARGUMENT ACCESSOR — the ordinary shape of the borrow above. §10.1.8.1 OrdinaryGet
       ( O, P, Receiver ) step 7 calls the getter with NO arguments, so `eff_argc` and `narg_alloc` are 0 and the
       frame names the property-operation record's own buffer, which the flow clone must then carry for it.
       THE FORK IS WHAT RUNS THE CLONE: this getter's `cfg.admin` is this document's first test of it, so the
       branch forks WHILE THE GETTER'S FRAME IS ON THE CHAIN — the only way to reach the clone's accessor arm.
       Both values must be present: one alone is a sibling that lost its arm or read its parent's. */
    "var _gx = { get x(){ return cfg.admin ? 'gxADMIN' : 'gxPUBLIC'; } };"
    " fetch('/api/getfork?v=' + _gx.x);"

    /* (3) A CONSTRUCT AFTER A CAUGHT CONSTRUCT-TIME THROW IN THE SAME FRAME. The construct side's operand shape
       is a register set just before a `goto`, read as "the arguments ARE the operands" when neutral (-1), so an
       arm that throws between the set and the read leaves the next construct popping a stale count.
       THE PATH: §28.1.2 Reflect.construct ( target, args [ , newTarget ] ) requests its Construct with no
       operands (neutral); §10.4.1.2 [[Construct]] ( argList, newTarget ) on a BOUND target rewrites the shape to
       the pop count it resolved. The throw is §10.1.14 GetPrototypeFromConstructor ( ctor,
       intrinsicDefaultProto ) step 3.a's `? GetFunctionRealm(ctor)`: §7.3.24 GetFunctionRealm ( func ) step 2
       forwards a bound function to its target and step 3.a's ValidateNonRevokedProxy throws. The bound function
       has no own `prototype` and its [[Prototype]] was captured by §10.4.1.3 BoundFunctionCreate before the
       revoke, so step 2 of §10.1.14 answers `undefined` and step 3.a is reached.
       THE SECOND CONSTRUCT IS THE ASSERTION: `new _CShape(1, 2)` with the stale shape pops none of its two
       operands, and in a dev build the pending-call idle check fires at the next opcode boundary — the `@WHY`
       is this statement's signature. `c` says the throw happened at all. */
    "var _rvk = Proxy.revocable(function(){}, {});"
    " var _cnt = Function.prototype.bind.call(_rvk.proxy);"
    " _rvk.revoke();"
    " function _CShape(a, b){ this.s = '' + a + b; }"
    " var _cst = 'nothrow';"
    " try { Reflect.construct(function(){}.bind(null), [], _cnt); } catch (_cse) { _cst = 'threw'; }"
    " fetch('/api/conshape?c=' + _cst + '&s=' + new _CShape(1, 2).s);"

    /* (2c) THE CONTROL FOR (2b): the same ternary over the same source in the same frame WITHOUT an accessor.
       If this carries both values and `/api/getfork` does not, the difference is the accessor.
       It forks nothing of its own — the ternary is decided inside each arm that exists — so one arm HERE is an
       arm that did not reach this statement: the gate upstream decided (`boot-fork`'s finding), an arm still
       parked on an undrained frontier, or an arm LOST. Only the third is (2b)'s premise, and only a drained
       frontier says which; `base_fork` in probes_eval separates them. */
    " fetch('/api/getbase?v=' + (cfg.admin ? 'gbADMIN' : 'gbPUBLIC'));"

    /* (2d) THE DISPOSE ARM NO CORPUS RUNS, appended at the end for the reason (2c) was: a statement added
       here shifts no column, so every statement above keeps the coordinates it was measured at.
       27.5.4.7.1 PromiseResolve is reached two ways from DisposeResources' async chain and they are DIFFERENT
       WALKS. A dispose method that returns a PROMISE takes step 1's `constructor` read; one that returns a
       THENABLE skips step 1 entirely, builds the capability at step 2, and reaches 27.5.1.3 step 2.f's
       `then` read through the resolving function. test262 runs the first and NOT the second: nothing under
       built-ins/AsyncDisposableStack/ or language/statements/await-using/ returns a non-promise thenable
       from a dispose method, and `grep -rln thenable` over both finds no file. So a whole-corpus 0 is SILENT
       about the walk these three statements are the only exercise of.
       THE FIRST ROW IS `tdreach` AND IT IS WHY THE OTHER THREE MEAN ANYTHING. Without it a 0 on any row
       below is three states wearing one number — the arm ran and settled wrong, the arm ran and never
       settled, or this engine has no AsyncDisposableStack and the statement never began. The catch names the
       third and `tdreach` names the difference between the other two. */
    " try {"
    "  fetch('/api/tdreach?d=ok');"
    /* A -- step 2's build path: a NON-async method, so the value is not a promise and step 1 is skipped. */
    "  var _tdA = new AsyncDisposableStack();"
    "  _tdA.use({ [Symbol.asyncDispose]: function () { return { then: function (r) { r(1); } }; } });"
    "  _tdA.disposeAsync().then(function () { fetch('/api/tdplain?d=ok'); },"
    "                           function () { fetch('/api/tdplain?d=no'); });"
    /* B -- THE SAME ARM WITH `then` BEHIND AN ACCESSOR, which is the case the deleted selector detected by
       name: the C entry refused this rather than reading it, so it must now park on the flow and settle. */
    "  var _tdT = {};"
    "  Object.defineProperty(_tdT, 'then', { get: function () { return function (r) { r(2); }; } });"
    "  var _tdB = new AsyncDisposableStack();"
    "  _tdB.use({ [Symbol.asyncDispose]: function () { return _tdT; } });"
    "  _tdB.disposeAsync().then(function () { fetch('/api/tdacc?d=ok'); },"
    "                           function () { fetch('/api/tdacc?d=no'); });"
    /* C -- the chain link's OWN PromiseResolve, which only runs when a link already carries an error. LIFO
       disposes the last use() first, so the throw below is what puts every link under it on that arm with a
       thenable value. The stack must still reject, and with the FIRST error. */
    "  var _tdC = new AsyncDisposableStack();"
    "  _tdC.use({ [Symbol.asyncDispose]: function () { return { then: function (r) { r(3); } }; } });"
    "  _tdC.use({ [Symbol.asyncDispose]: function () { throw new Error('tdboom'); } });"
    "  _tdC.disposeAsync().then(function () { fetch('/api/tdprev?d=no'); },"
    "                          function () { fetch('/api/tdprev?d=rej'); });"
    " } catch (_tde) { fetch('/api/tdreach?d=no'); }"
    /* A BYOB READ WHOSE MINIMUM IS UNKNOWN IS THREE WORLDS AND NOT A RANGE. Streams §4.5.3 "Constructor,
       methods, and properties"' read(view, options) names `min` in step 4 ("If options["min"] is 0, return a
       promise rejected with a TypeError exception.") and in steps 5.1 / 6.1 ("If options["min"] >
       view.[[ArrayLength]], return a promise rejected with a RangeError exception.", and the same over
       [[ByteLength]] for a DataView). Over an unknown both conditions fork: TypeError, RangeError, and the
       ordinary pull-into that carries on to §4.9.5 — all three in one run.
       THE MINIMUM IS UNKNOWN AND ITS EXAMPLE IS IN RANGE: the surviving arm needs an example in [1, the view's
       element count] for §4.9.5's minimum fill, so `(screen.width & 7) + 1` (§6.1.6.1.16 NumberBitwiseOp runs
       on the real example and the result stays unknown). `location.hash` would be an @S source whose candidate
       sessions substitute bytes and turn the conversion into a TypeError from Web IDL §3.3.6 [EnforceRange],
       the wrong mechanism.
       EIGHT ELEMENTS, EIGHT BYTES QUEUED, so the surviving world SETTLES rather than parking; the refusals are
       rejected promises (Web IDL §3.7.7 "Operations"), handled in the expression that produced them. */
    " try {"
    "  var bsSt = new ReadableStream({ type: 'bytes',"
    "    start: function (bsC) { bsC.enqueue(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8])); } });"
    "  var bsRd = bsSt.getReader({ mode: 'byob' });"
    "  var bsMin = (screen.width & 7) + 1;"
    "  fetch('/api/bsreach?d=ok');"
    "  bsRd.read(new Uint8Array(8), { min: bsMin }).then("
    "   function (bsR) { fetch('/api/bsmin?w=read'); fetch('/api/bsdone?d=' + bsR.done); },"
    "   function (bsE) { fetch('/api/bsmin?w=' + bsE.name); });"
    " } catch (bsX) { fetch('/api/bsreach?d=no&n=' + bsX.name); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
