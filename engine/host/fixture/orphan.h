/* THE ORPHAN DOCUMENT: code the page ships and never calls, driven as flows of its own.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ORPHAN_H
#define ENGINE_HOST_FIXTURE_ORPHAN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ORPHAN[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* ORPHAN-INVOKE — the headline capability, and the ONE statement in this document that nothing in it calls.
       It asks two things at once because they are the two halves of the mechanism and either alone would pass
       while the other was broken: that the function RUNS at all (`/api/orphan/report`), and that its PARAMETER
       is unknown external input rather than `undefined` — so the equality gate FORKS and the arm behind it is
       explored (`/api/orphan/admin-only`), which is the whole of "learn the logged-in surface while logged
       out" reduced to one line. A driven orphan that received `undefined` would emit the first and never the
       second, and a run in which orphan-invoke does not exist emits neither: a page holding only this function
       measured ZERO endpoints against one flow, which is what this row exists to keep from happening again. */
    "function orphanNeverCalled(role){ fetch('/api/orphan/report');"
    " if (role === 'admin') { fetch('/api/orphan/admin-only'); } }"
    /* AND THE SEAM INSIDE ONE, which the two rows above cannot see: they assert an orphan RUNS and that its
       parameter is unknown, and both are true of a body driven start-to-finish with nothing to preempt at.
       A driven orphan's frame is a CALL-ROOT flow (JS_FlowNewCall), so a back-edge in its body must reach the
       preempt hook, park the flow as a COW snapshot and resume it — 3000 times here, under FORK_PREEMPT. The
       terminal value is in the PATH rather than a query parameter so one exact match decides it: `loop3000`
       says the loop ran to its end and every one of those resumes was byte-identical, where a dropped or
       reordered resume produces some other number and a lost one produces no endpoint at all. */
    "function orphanLoops(){ var s = 0; for (var i = 0; i < 3000; i++) { s = s + 1; }"
    " fetch('/api/orphan/loop' + s); }"
    /* AND AN ORPHAN THAT COMPARES ITS UNKNOWN AGAINST AN OBJECT, which is the FIRST thing a real library does
       with an argument it was handed and which took four unmodified bundles' engine instances down before this
       row existed: axios's `isPlainObject` and vue's own walk are both `while (n !== Object.prototype)`. The
       equality's other operand is an Object, and §7.1.19 ToString step 10 sends one to ToPrimitive — the page's
       valueOf/toString, run from a C activation with no flow base under it — so spelling it aborted the whole
       instance and every finding went with it. §7.2.14 IsStrictlyEqual step 1 returns false whenever
       SameType(x, y) is false, so there is nothing for such a comparison to pin the unknown to: the predicate
       forks and carries no pin. The claim is the endpoint BEHIND the walk, because an abort emits none. */
    "function orphanIdentity(o){ var n = o;"
    " while (n != null && n !== Object.prototype) { n = Object.getPrototypeOf(n); }"
    " fetch('/api/orphan/identity'); }"
    /* AND AN ORPHAN THAT PUTS ITS UNKNOWN THROUGH A COERCE-THEN-COMPUTE BUILTIN, which is the SECOND thing a
       real library does with an argument it was handed: every hand-rolled base64 / UTF-16 / hash decoder in a
       bundle is `String.fromCharCode(x.y)`. §22.1.2.1 step 2.a is `ℝ(? ToUint16(next))` and §7.1.11 step 1 is
       §7.1.4 ToNumber, while the declaration's own coercion is §7.1.1 ToPrimitive — which over an unknown is
       the identity — so the unknown reached the conversion boundary that owes C a real double and the whole
       instance aborted. The claim is the endpoint AFTER the call, because an abort emits none: reaching it says
       the coercion produced a value the string concatenation could carry rather than a crash. */
    "function orphanCharCode(e){ var s = String.fromCharCode(e.charCode);"
    " fetch('/api/orphan/charcode?c=' + s); }"
    /* AND THE POSTFIX UPDATE OPERATOR, WHICH IS HALF OF §13.4 AND WAS THE HALF NOBODY ASKED. `++i` reaches
       js_unary_arith_slow and its .arith hook answered; `i--` goes through §13.4.3.1 step 3's
       `? ToNumeric(? GetValue(lhs))` first and CONVERTED, so `for (; e--; )` — a real route loader's own loop —
       aborted the whole instance. Both spellings are here because the row must fail when only one is built:
       the PREFIX arm passed the entire time the postfix arm was crashing, so a fixture holding only `++i`
       reports a family that works while half of it takes documents down. */
    "function orphanUpdate(e){ var i = e.n, j = ++i, k = i--, n = 0;"
    " for (var m = e.n; m--;) { n = n + 1; if (n > 2) break; }"
    " fetch('/api/orphan/update?n=' + n); }"
    /* AND A COERCE-THEN-COMPUTE BUILTIN WITH A GENERIC BODY — the clamp every bundle writes. §21.3.2.26
       Math.min ( ...args ) step 2 is `? ToNumber(arg)` for each argument and the declaration's own coercion is
       §7.1.1 ToPrimitive, which over an unknown is the identity, so the body's JS_ToFloat64 reached the
       conversion boundary. One unknown argument makes the whole result unknown (step 4's `number > highest` is
       undecided against a value nobody has), and the endpoint AFTER the clamp is the claim because an abort
       emits none. */
    "function orphanClamp(e){ var v = Math.max(0, Math.min(255, e.n));"
    " fetch('/api/orphan/clamp?v=' + v); }"
    /* AND THE EIGHTH AND NINTH: the driven body built a DATE out of its unknown and survived it. TWO rows
       because §21.4.2.1 Date ( ...values ) has TWO coercing arms whose repairs are DIFFERENT, and each was
       aborting on a different real site of a 19-site corpus: the ONE-argument shape's step 4.c.iii coerces the
       primitive step 4.c.i produced and has to keep its ToPrimitive apart, because step 4.c.ii inspects what
       came back; the MULTI-argument shape's steps 5.b-5.h are §7.1.4 ToNumber over the ARGUMENT with no step
       in between, so the pair is one sub-sequence call. A row holding only one of them reports a family that
       works while half of it takes documents down, which is the same reason `orphanUpdate` spells both the
       prefix and the postfix update.
       THE UNKNOWN IS THE FIELD AT INDEX 0 in the multi-argument statement and the remaining two are literals,
       deliberately: an unknown at an earlier field does not coerce the later ones (see the residual on
       js_date_ctor_step), so a `valueOf` written into field 1 here would be asserting that residual rather
       than this row. The requests are uninteresting and REACHING them is the claim, because an abort emits
       none — and this is the arm that carries §21.4.2.1 step 5.i MakeFullYear too. */
    "function orphanDate(e){ var t = new Date(e.n * 1000), u = new Date(e.y, 0, 1);"
    " fetch('/api/orphan/date1?t=' + t); fetch('/api/orphan/datef?u=' + u); }"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
