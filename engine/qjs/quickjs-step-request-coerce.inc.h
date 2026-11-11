/* §7.1.2 ToBoolean ( arg ) AS A STEP MACHINE'S OWN — see quickjs-step.h for the whole argument. */
int step_tobool_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, const char *op, int *pres)
{
    int arm, r;

    if (h->fork_phase == FORK_PH_ASK) {
        /* THE SAME GATE branch_arm_fork PUTS IN FRONT OF THE BRANCH HOOK, and for the same reason: a concolic
           is minted through exactly one entry (concolic_alloc → JS_NewObjectClass), so unknown input is always
           an Object and nothing else can be. A value that is not one is answered by §7.1.2 itself with no
           request, no round trip through the driver and no fork — which is what makes this a coercion with one
           path rather than a choice between two. */
        if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT || !g_concolic.is(v)) {
            *pres = JS_ToBool(ctx, v);
            DCHECK(*pres == 0 || *pres == 1,
                   "§7.1.2 ToBoolean answered neither true nor false for a value that is not unknown input — "
                   "it answers -1 only for an exception, so a machine has handed its coercion a pending throw "
                   "instead of the value its callback returned");
            return 0;
        }
    }
    /* n IS 2 AND `real` IS UNSTATED, and neither is a placeholder. A truth has exactly two completions; and the
       arm a real session takes is computed by the BRANCH seam out of the condition's own example (that is what
       decide_real_arm is), so a machine stating one here would be a second, weaker answer to a question the
       seam already answers from the value itself. */
    r = step_fork_ask(ctx, h, v, op, 2, JS_OUTCOME_REAL_UNSTATED, JS_FORK_KIND_TOBOOL, &arm);
    if (r) return r;
    *pres = arm;
    return 0;
}

/* THE ToString SUB-SEQUENCE'S OWN CURSOR, declared first because more than one function in it reads the
   cursor: the coercion itself, its unknown-input arm below, ToPropertyKey's and the receiver form's. It was
   declared beside step_tostring_run alone, which was true only while that was the whole sub-sequence. */
enum { STR_PH_START = 0, STR_PH_PRIM };

/* A COERCION OVER UNKNOWN EXTERNAL INPUT — see JS_STEP_UNKNOWN for the spec argument and for what the driver
   then does with it. `v` is BORROWED; the header takes its own reference, because the caller's may be a value
   the caller is about to release. `phase` is the SUB-SEQUENCE'S OWN cursor, reset because this coercion of `v`
   is over either way — passed in rather than picked here, since ToString and the numeric family keep separate
   cursors on the header and one function must not reset the other's.
   Where `v` carries an example and the machine does not answer unknown input itself, `*ex` takes the example
   (owned) and 0 is returned: the caller runs the same coercion over it, and the machine runs on. The first
   such operand stays on the header for the driver's derivation. Otherwise the operand is parked and
   JS_STEP_UNKNOWN returned, with `*ex` undefined. */
/* Named residual. Not covered: a second unknown operand of one machine contributes its example to the result
   but not its root, since the derivation names the first. Next diff: the header holds the operand list and the
   driver derives over all of them (concolic_new_derived takes several). How its absence shows: a finding over a
   builtin's result whose root names one of two unknown arguments the page passed. */
/* BOTH FAMILIES, because the argument is the same one in both directions. §7.1.19 ToString ( arg ) steps 9-12
   and §7.1.4 ToNumber ( arg ) steps 7-10 have the identical shape — assert the remaining case is an Object,
   §7.1.1 ToPrimitive with the family's hint, assert the result is not an Object, recurse — and §7.1.1 over a
   concolic returns it unchanged, so both recursions are a coercion of an unknown whose result is an unknown of
   that type. Only ToString had this arm, so every builtin coercing a numeric ARGUMENT (§7.1.5
   ToIntegerOrInfinity, §7.1.22 ToLength, §7.1.8 ToInt32) carried its unknown to a conversion boundary that owes
   C a real double and aborts there: `Array.prototype.slice.call(a, {orphan.arg3})` ended a whole document on a
   real page, and a crashed instance's findings are discarded. */
static int step_coerce_unknown(JSContext *ctx, JSStepHdr *h, JSValueConst v, uint8_t *phase, JSValue *ex)
{
    JSValue e = h->def->answers_unknown ? JS_UNDEFINED : js_concolic_operand(ctx, v);

    *phase = 0;                    /* STR_PH_START / NUM_PH_START — this coercion of `v` is over either way */
    *ex = JS_UNDEFINED;
    DCHECK(!JS_IsUninitialized(e), "a coercion reported unknown input for an operand that is not unknown");
    if (!JS_IsUndefined(e)) {
        DCHECK(!(g_concolic.is(e)),
               "an unknown's example is itself unknown input — an example rides the concrete value the "
               "interpreter produced, so a producer attached a derivation where a datum belongs");
        if (JS_IsUninitialized(h->unknown_operand)) {
            h->unknown_operand = js_dup(v);
            h->unknown_stage = h->stage;
        }
        *ex = e;
        return 0;
    }
    /* No example: there is no value to run the machine on, and running it on an invented one would call page
       code with it, so the machine stops here. */
    DCHECK(!h->def->answers_unknown || JS_IsUninitialized(h->unknown_operand),
           "a machine that answers unknown input itself met a second one before taking the first off its header");
    /* An earlier operand's example has been run on, and this operand has none, so the completion has no
       example either: it is derived from this one (the residual above names the other's root). */
    JS_FreeValue(ctx, h->unknown_operand);
    h->unknown_operand = js_dup(v);
    h->unknown_stage = h->stage;
    return JS_STEP_UNKNOWN;
}

/* A sub-sequence's coercion of unknown `v` (borrowed; `owned`, a value the caller holds, is released here):
   return what step_coerce_unknown reports, or run the sub-sequence `self` again over the example. `self` is
   the sub-sequence this expands in, and `out` its result pointer. */
#define STEP_COERCE_UNKNOWN(self, ctx, h, v, owned, phase, out, out_cb, out_argc) do {  \
        JSValue ex_;                                                                     \
        int r_ = step_coerce_unknown((ctx), (h), (v), (phase), &ex_);                    \
        JS_FreeValue((ctx), (owned));                                                    \
        if (r_) return r_;                                                               \
        r_ = self((ctx), (h), ex_, JS_UNDEFINED, (out), (out_cb), (out_argc));           \
        JS_FreeValue((ctx), ex_);                                                        \
        return r_;                                                                       \
    } while (0)

/* ToString on a value that may run user code. The object case is a TOPRIMITIVE step; whatever comes back is a
   primitive, so the ToString that finishes it runs nothing. 0 = *pout is the string, 5 = the caller must return
   that step code and will be re-entered here, JS_STEP_UNKNOWN = the value is unknown external input and there
   is no String to produce (nothing is written to *pout), -1 = threw. */

int step_tostring_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, JSValue *pout,
                             JSValue **out_cb, int *out_argc)
{
    if (h->str_phase == STR_PH_START) {
        JS_FreeValue(ctx, in);
        /* §7.1.19 steps 9-12 OVER UNKNOWN EXTERNAL INPUT, ANSWERED BEFORE THE COERCION IS EVEN ASKED FOR —
           the same shape step_length_value already gives LengthOfArrayLike one screen up, and for the same
           reason: a concolic IS an Object, so the arm below would send it to ToPrimitive, get it straight back
           (§7.1.1 over a concolic is the identity — the value is primitive in the page and wears an Object
           only as a carrier), and hand it to a conversion boundary that owes C a real JSString. THIS is the
           "build it in the operator" that boundary's abort names, and it is built at the ONE sub-sequence every
           builtin's ToString goes through rather than at the sixty call sites that reach it. */
        if (unlikely(g_concolic.is(v)))
            STEP_COERCE_UNKNOWN(step_tostring_run, ctx, h, v, JS_UNDEFINED, &h->str_phase, pout, out_cb, out_argc);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;     /* borrowed view */
            *out_cb = h->cb_coerce; *out_argc = HINT_STRING;
            h->str_phase = STR_PH_PRIM;
            return 5;
        }
        *pout = JS_ToString(ctx, v);
        return JS_IsException(*pout) ? -1 : 0;
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->str_phase = STR_PH_START;
    /* AND THE OTHER WAY IN, which the arm above does not cover: an ORDINARY object whose own coercion method
       ANSWERED with unknown external input — `{ valueOf() { return location.hash } }`. §7.1.19 step 11 asserts
       the primitive is not an Object, and a concolic satisfies that assertion about the PAGE's value while
       failing it about the carrier, so this is the same case arriving one hop later. */
    if (unlikely(g_concolic.is(in)))
        STEP_COERCE_UNKNOWN(step_tostring_run, ctx, h, in, in, &h->str_phase, pout, out_cb, out_argc);
    *pout = JS_ToStringFree(ctx, in);
    return JS_IsException(*pout) ? -1 : 0;
}

int step_tostring_carry_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, JSValue *pout,
                            JSValue **out_cb, int *out_argc)
{
    if (h->str_phase == STR_PH_START) {
        if (unlikely(g_concolic.is(v))) {
            JS_FreeValue(ctx, in);
            *pout = js_dup(v);
            return 0;
        }
    } else if (unlikely(g_concolic.is(in))) {
        /* The conversion answered unknown input: this coercion is over, and its answer is the result. */
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        h->str_phase = STR_PH_START;
        *pout = in;
        return 0;
    }
    return step_tostring_run(ctx, h, v, in, pout, out_cb, out_argc);
}

/* THE COMPLETION OF A MACHINE THAT MET UNKNOWN INPUT: derived from `v` (consumed), the operand the driver took
 * off the machine's header, and named by the ALGORITHM of `def` and the SPEC STEP `stage` it was coercing at.
 * `real` (consumed) is the machine's own completion over the operand's example, which becomes the derivation's
 * example; JS_UNDEFINED where the operand had none and the machine stopped there.
 *
 * THE NAME IS BOTH HALVES BECAUSE THE IDENTITY IS WHAT THE PATH CONSTRAINT IS KEYED BY. `def->algorithm` alone
 * would give `x.replace(x, x)`'s search coercion and its replacement coercion ONE identity, and decide.c would
 * then let a branch over the first decide a branch over the second. A stage NAMES ONE SPEC STEP
 * (JSTrampStepDef.steps), and it is the label rather than the index because the label is what survives a build
 * whose stages moved. */
static JSValue js_step_unknown_result(JSContext *ctx, const JSTrampStepDef *def, uint16_t stage, JSValue v,
                                      JSValue real)
{
    JSValue r;
    const char *label;
    char *op;
    size_t n;
    int nstages = 0;

    DCHECK(g_concolic.is(v),
           "a step machine's completion was derived from something that is not unknown external input — the "
           "operand step_coerce_unknown records is not on the header the DRIVER is stepping, which is what "
           "happens when a machine FORWARDS its step to another machine's own header and does not move it across");
    /* ALWAYS FATAL: the pair IS this value's identity, and a derivation that cannot spell one is keyed by
       decide.c under whatever it shares a name with. */
    CHECK(def->algorithm != NULL && def->steps != NULL,
          "a step machine whose coercion was over unknown input declares no algorithm or no steps — the pair "
          "IS the derived value's identity, and a value that cannot be spelled cannot be told apart from "
          "this algorithm's other coercions");
    while (def->steps[nstages]) nstages++;
    DCHECK((int)stage < nstages,
           "a step machine coerced unknown input at a stage its algorithm does not declare — the stage label "
           "is half the derived value's identity, so there is no name to give this coercion");
    label = (int)stage < nstages ? def->steps[stage] : "(an undeclared stage)";
    /* Sized from the two strings: a stage label is prose, and a truncated name is two coercions sharing one
       identity. */
    n = strlen(def->algorithm) + 3 + strlen(label) + 1;
    op = js_malloc(ctx, n);
    CHECK(op != NULL, "out of memory naming a derived unknown — a dropped concolic collapses a branch to a "
                      "concrete arm and deletes everything behind the other");
    snprintf(op, n, "%s @ %s", def->algorithm, label);
    r = js_concolic_derive(ctx, v, op, real);
    js_free(ctx, op);   /* the derivation COPIES what it keeps (its shape and its identity are its own) */
    DCHECK(!JS_IsUninitialized(r),
           "the concolic derivation declined an operand this engine had already recognised as unknown input — "
           "the value semantics were installed without the builtin-derivation hook");
    JS_FreeValue(ctx, v);
    return r;
}

/* §7.1.21 ToPropertyKey ( arg ): ToPrimitive with hint STRING — the page's code when the key is an object — after
   which a Symbol is used as-is and anything else is ToString'd. The atom conversion runs nothing. Every C site
   that takes a property key coerces it exactly this way, which is why this is a shared sub-sequence and not part
   of one builtin. */
static int step_topropkey_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, JSAtom *pres,
                              JSValue **out_cb, int *out_argc)
{
    if (h->str_phase == STR_PH_START) {
        JS_FreeValue(ctx, in);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;
            *out_cb = h->cb_coerce; *out_argc = HINT_STRING;
            h->str_phase = STR_PH_PRIM;
            return 5;
        }
        *pres = JS_ValueToAtom(ctx, v);
        return (*pres == JS_ATOM_NULL) ? -1 : 0;
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->str_phase = STR_PH_START;
    *pres = JS_ValueToAtom(ctx, in);
    JS_FreeValue(ctx, in);
    return (*pres == JS_ATOM_NULL) ? -1 : 0;
}

/* PerformEval's / CreateDynamicFunction's EVALUATION of a source string. A script's body is a BYTECODE BODY, so
   evaluating one is a CALL and belongs on the tramp: performed from C it gets an activation off the chain, and a
   loop inside it has no flow base to park into. Two builtins perform it — indirect eval (19.2.1.1) and
   CreateDynamicFunction, whose parse-and-create this engine performs by evaluating the synthesized
   `(function anonymous(…){…})` source — so it is a shared sub-sequence rather than a stage each of them
   re-spells. The receiver is the GLOBAL OBJECT: an indirect eval's program is global scope whatever the code's
   strictness, so a `"use strict"` prologue must still see the global and not undefined.
   Between the source and the parse stands the evaluation's HostEnsureCanCompileStrings, issued as a call whose
   completion re-enters here (PROG_PH_CHECK); `cc` states its operands and is read only by the first entry.
     0 = *pout is the program's value, 3 or JS_STEP_YIELD (the parse rested) = the caller must return that step
   code, -1 = threw. */
enum { PROG_PH_START = 0, PROG_PH_RAN, PROG_PH_PARSE, PROG_PH_CHECK };

static inline bool tramp_body_is_plain(JSValueConst func);
typedef struct JSEvalCompile JSEvalCompile;
static JSValue js_eval_compile_continue(JSContext *ctx, JSEvalCompile **slot);
static void js_eval_compile_drop(JSContext *ctx, JSEvalCompile **slot);
static _Thread_local JSEvalCompile **g_eval_compile_slot;

/* The program's parse in flight and the UTF-8 it borrows for its whole length. */
typedef struct {
    JSEvalCompile *ec;
    JSContext *realm;
    const char *str;
    size_t len;
} JSProgCompile;

/* Releases the header's held parse and its source; a no-op where none is held. */
static void js_prog_compile_drop(JSContext *ctx, JSStepHdr *h)
{
    JSProgCompile *pc = h->prog_compile;

    if (pc) {
        js_eval_compile_drop(pc->realm, &pc->ec);
        JS_FreeCString(ctx, pc->str);
        js_free(ctx, pc);
        h->prog_compile = NULL;
    }
    JS_FreeValue(ctx, h->prog_src);
    h->prog_src = JS_UNDEFINED;
}

/* `realm` is the realm the program BELONGS to — its own for eval and the Function constructor, the eval realm
   for ShadowRealm.prototype.evaluate, whose 3.2.1 is this same compile-then-call in another global environment.
   It is a parameter rather than `ctx` because the machine performing it runs in the CALLER's realm.
   `origin_flags` is JS_EVAL_FLAG_FUNCTION_CTOR for the Function constructor and 0 for the indirect eval. The
   two share this body because their algorithm IS the same program evaluation — and they are not the same
   ORIGIN: `new Function` produces code with no eval origin and CallSite#isEval must say so. Passing the flag
   in rather than hardcoding it here is the difference between one implementation and one implementation that
   quietly asserts both callers are the same thing; hardcoded, indirect eval reported itself as not an eval. */
static int step_program_run(JSContext *ctx, JSStepHdr *h, JSContext *realm, JSValueConst src, JSValue in,
                            JSValue *pout, JSValue **out_cb, int *out_argc, int origin_flags,
                            const JSCompileCheck *cc)
{
    if (h->prog_phase == PROG_PH_START) {
        JSValue text, check;
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        DCHECK(cc != NULL, "a program evaluation began without stating its HostEnsureCanCompileStrings operands");
        /* THE @S JS-CONTEXT SINK AND §19.2.1.1's step 2, for every algorithm that reaches this sub-sequence:
           the INDIRECT eval (19.2.1), CreateDynamicFunction (20.2.1.1.1) and ShadowRealm.prototype.evaluate.
           Both halves belong to js_eval_program_source and neither is re-decided here — the direct-eval
           spellings reach the same operation from eval_direct_closure, which is a DIFFERENT algorithm
           (13.3.6.1's caller scope) sharing this one step, not a fallback for it. */
        text = js_eval_program_source(ctx, src);
        if (JS_IsUninitialized(text)) {
            *pout = js_dup(src);   /* 19.2.1.1 step 2: a non-string source is the result, unevaluated */
            return 0;
        }
        h->prog_src = text;
        h->prog_phase = PROG_PH_PARSE;
        /* PerformEval step 5 / CreateDynamicFunction's HostEnsureCanCompileStrings (Dynamic Code Brand Checks
           §3 and §4), with the program text as codeString; an eval's bodyString (JS_UNINITIALIZED in `cc`) is
           that text too. */
        {
            JSCompileCheck c = *cc;

            if (JS_IsUninitialized(c.body))
                c.body = text;
            check = js_compile_check_call(realm, &c, text);
        }
        if (JS_IsException(check)) {
            js_prog_compile_drop(ctx, h);
            h->prog_phase = PROG_PH_START;
            return -1;
        }
        if (!JS_IsUndefined(check)) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = check;                 /* owned across the call */
            h->cb_coerce[0] = JS_UNDEFINED;
            h->cb_coerce[1] = h->coerce;       /* borrowed view */
            *out_cb = h->cb_coerce; *out_argc = 0;
            h->prog_phase = PROG_PH_CHECK;
            return 3;
        }
    } else if (h->prog_phase == PROG_PH_CHECK) {
        /* The check's normal completion. Its throw refuses the compile and is the evaluation's completion: no
           machine running this sub-sequence declares catches_abrupt, so that throw abandons it and never
           arrives here. */
        DCHECK(!JS_IsException(in) && !h->def->catches_abrupt,
               "a program machine that catches abrupt request results was handed its host check's throw");
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        h->cb_coerce[0] = JS_UNDEFINED;
        h->cb_coerce[1] = JS_UNDEFINED;
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        h->prog_phase = PROG_PH_PARSE;
    }
    if (h->prog_phase == PROG_PH_PARSE) {
        /* The parse hands the thread back where the scheduler asks and is continued here from the record; a
           fork sheds the record and the sibling begins again from `prog_src`. */
        JSProgCompile *pc = h->prog_compile;
        JSValue clo;

        JS_FreeValue(ctx, in);
        if (!pc) {
            JSEvalCompile **outer = g_eval_compile_slot;

            pc = js_mallocz(ctx, sizeof *pc);
            if (!pc)
                return -1;
            pc->realm = realm;
            pc->str = JS_ToCStringLen(ctx, &pc->len, h->prog_src);
            if (!pc->str) {
                js_free(ctx, pc);
                return -1;
            }
            h->prog_compile = pc;
            js_eval_sink_rearm();   /* a parse begun again after a fork that shed it; see js_eval_sink_rearm */
            /* NO CALLER FRAME, and that is what INDIRECT means: 19.2.1.1 evaluates in the GLOBAL environment,
               so there is no caller scope to take — the `-1` scope_idx beside it says the same thing. */
            g_eval_compile_slot = &pc->ec;
            clo = JS_EvalInternal(realm, realm->global_obj, pc->str, pc->len, JS_EVAL_SCRIPT_NAME, 1,
                                  JS_EVAL_TYPE_INDIRECT | JS_EVAL_FLAG_TRAMP_CLOSURE | origin_flags, -1, NULL);
            g_eval_compile_slot = outer;
        } else {
            DCHECK(pc->ec != NULL && pc->realm == realm,
                   "a program parse resumed with nothing parked, or in another realm than it began in");
            clo = js_eval_compile_continue(realm, &pc->ec);
        }
        if (JS_VALUE_GET_TAG(clo) == JS_TAG_UNINITIALIZED)
            return JS_STEP_YIELD;
        DCHECK(pc->ec == NULL, "a program parse answered and left a parked parse behind");
        js_prog_compile_drop(ctx, h);
        if (JS_IsException(clo)) {
            h->prog_phase = PROG_PH_START;
            return -1;
        }
        DCHECK(tramp_body_is_plain(clo), "a program closure is not a trampolinable bytecode function");
        DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
               "a coercion is already in flight on this machine's header");
        h->coerce = clo;                     /* owned across the call */
        h->cb_coerce[0] = realm->global_obj; /* borrowed: the realm holds its global object */
        h->cb_coerce[1] = h->coerce;         /* borrowed view */
        *out_cb = h->cb_coerce; *out_argc = 0;
        h->prog_phase = PROG_PH_RAN;
        return 3;
    }
    DCHECK(h->prog_phase == PROG_PH_RAN, "a program evaluation resumed in an unknown phase");
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->cb_coerce[0] = JS_UNDEFINED;
    h->cb_coerce[1] = JS_UNDEFINED;
    h->prog_phase = PROG_PH_START;
    *pout = in;
    return 0;
}

/* JS_ToStringCheckObject on the RECEIVER — RequireObjectCoercible, then ToString. Every String.prototype method
   opens with it, which is why even a ZERO-ARGUMENT one could not stay a C body: `"".trim.call(o)` where o's
   toString loops preempted in an activation with no flow base. */
static int step_thisstring_run(JSContext *ctx, JSStepHdr *h, JSValue in, JSValue *pout,
                               JSValue **out_cb, int *out_argc)
{
    if (h->str_phase == STR_PH_START
        && (JS_IsUndefined(h->this_val) || JS_IsNull(h->this_val))) {
        JS_FreeValue(ctx, in);
        JS_ThrowTypeError(ctx, "null or undefined are forbidden");
        return -1;
    }
    return step_tostring_run(ctx, h, h->this_val, in, pout, out_cb, out_argc);
}

/* ToNumber on a value that may run user code, delivered as a saturating int64 (ToIntegerOrInfinity's clamp).
   The object case is a TOPRIMITIVE step; the primitive that comes back converts without running anything. This
   is the shape EVERY builtin that takes an index has — `[1,2].at(x)`, indexOf's fromIndex, String's positions —
   and each of them did it with JS_ToInt64Sat straight out of C, so a valueOf with a loop in it preempted in an
   activation with no flow base. 0 = *pres is filled, 5 = the caller must return that step code and will be
   re-entered here, -1 = threw. */
enum { NUM_PH_START = 0, NUM_PH_PRIM };

/* ToPrimitive(v, hint) delivering the PRIMITIVE ITSELF. The existing helpers each finish with a conversion —
   ToString, ToLength, ToNumber — but some spec steps ARE the coercion and inspect what came back (which type it
   is, whether it is finite). 0 = *pout is filled, 5 = the caller must return that step code, -1 = threw. */
static int step_toprim_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, int hint, JSValue in, JSValue *pout,
                           JSValue **out_cb, int *out_argc)
{
    if (h->num_phase == NUM_PH_START) {
        JS_FreeValue(ctx, in);
        if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) { *pout = js_dup(v); return 0; }
        DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
               "a coercion is already in flight on this machine's header");
        h->coerce = js_dup(v);
        h->cb_coerce[0] = h->coerce;
        *out_cb = h->cb_coerce; *out_argc = hint;
        h->num_phase = NUM_PH_PRIM;
        return 5;
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->num_phase = NUM_PH_START;
    *pout = in;
    return 0;
}

int step_toprimitive_number_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, JSValue *pout,
                                JSValue **out_cb, int *out_argc)
{
    /* §7.1.1 over unknown external input returns it unchanged, so the coercion is the unknown's — see
       step_coerce_unknown. */
    if (h->num_phase == NUM_PH_START && unlikely(g_concolic.is(v))) {
        STEP_COERCE_UNKNOWN(step_toprimitive_number_run, ctx, h, v, in, &h->num_phase, pout, out_cb, out_argc);
    }
    return step_toprim_run(ctx, h, v, HINT_NUMBER, in, pout, out_cb, out_argc);
}

int step_toint64_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, int64_t *pres,
                     JSValue **out_cb, int *out_argc)
{
    if (h->num_phase == NUM_PH_START) {
        JS_FreeValue(ctx, in);
        /* §7.1.4 steps 7-10 over unknown external input — see step_coerce_unknown. */
        if (unlikely(g_concolic.is(v)))
            STEP_COERCE_UNKNOWN(step_toint64_run, ctx, h, v, JS_UNDEFINED, &h->num_phase, pres, out_cb, out_argc);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;     /* borrowed view */
            *out_cb = h->cb_coerce; *out_argc = HINT_NUMBER;
            h->num_phase = NUM_PH_PRIM;
            return 5;
        }
        return JS_ToInt64Sat(ctx, pres, v);
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->num_phase = NUM_PH_START;
    /* …and the other way in: the page's own valueOf ANSWERED with unknown external input. */
    if (unlikely(g_concolic.is(in)))
        STEP_COERCE_UNKNOWN(step_toint64_run, ctx, h, in, in, &h->num_phase, pres, out_cb, out_argc);
    return JS_ToInt64SatFree(ctx, pres, in);
}

/* THE SAME REQUEST DELIVERING THE NUMBER ITSELF. Web IDL's integer conversions are not ToIntegerOrInfinity:
   §3.2.7's `long` is ToNumber and then a MODULO 2^32, and §3.2.4.2's [Clamp] is ToNumber and then a round to the
   nearest integer choosing the EVEN one at a half. A saturating int64 has already lost what both of those need —
   `f(2**32 + 5)` is 5 for a `long` and INT64 5000000005 for a saturated one, and `f(1.5)` is 2 for a [Clamp] and
   1 for a truncation. So the coercion stops at the number and the caller's type says what happens next. */
int step_todouble_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, double *pres,
                      JSValue **out_cb, int *out_argc)
{
    if (h->num_phase == NUM_PH_START) {
        JS_FreeValue(ctx, in);
        /* §7.1.4 steps 7-10 over unknown external input — see step_coerce_unknown. */
        if (unlikely(g_concolic.is(v)))
            STEP_COERCE_UNKNOWN(step_todouble_run, ctx, h, v, JS_UNDEFINED, &h->num_phase, pres, out_cb, out_argc);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;     /* borrowed view */
            *out_cb = h->cb_coerce; *out_argc = HINT_NUMBER;
            h->num_phase = NUM_PH_PRIM;
            return 5;
        }
        return JS_ToFloat64(ctx, pres, v);
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->num_phase = NUM_PH_START;
    /* …and the other way in: the page's own valueOf ANSWERED with unknown external input. */
    if (unlikely(g_concolic.is(in)))
        STEP_COERCE_UNKNOWN(step_todouble_run, ctx, h, in, in, &h->num_phase, pres, out_cb, out_argc);
    return JS_ToFloat64Free(ctx, pres, in);
}

/* A CALL AS A REQUEST — the operation a browser component needs most, and the last one that was missing.
   Every host machine so far COERCED its arguments and then computed; dispatchEvent has to RUN the page's
   listeners, in order, and report whether one of them cancelled — which §2.9 makes SYNCHRONOUS. Calling them
   from C is the drive-to-completion the engine aborts on, and enqueueing them as jobs answers before any of
   them has run, so `dispatchEvent` would always report "not cancelled". This parks the machine on the call and
   answers at the SAME call site when it is re-entered, exactly like the coercion sub-sequences above.

   It takes the machine's OWN phase byte and its OWN buffer rather than borrowing the header's, because a host
   machine can hold a call across several of its stages while a coercion is a within-stage sub-sequence — and
   because the buffer must be declared in the machine's `visit` for a fork to copy it. `cb` is 2 + argc slots:
   [this, func, args...], the shape the dispatch reads. The machine OWNS what it puts there; this dups on the
   way in and releases on the way out, so a machine never has to remember either half.
     0 = *pout is the result, 3 = the caller must return that step code, and a throw ABANDONS the machine unless
   its definition declares catches_abrupt — a listener's uncaught exception is the page's, and the engine does not
   swallow it; an algorithm the standard wraps in "while catching any exceptions" is told about it here, as
   *pout == JS_EXCEPTION with the completion still live. */
int step_call_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst func,
                  JSValueConst this_val, int argc, JSValueConst *argv, JSValue in, JSValue *pout,
                  JSValue **out_cb, int *out_argc)
{
    int i;

    /* THE BUFFER IS THE CALLER'S, SO ITS SIZE HAS TO ARRIVE WITH IT. Without this the shape `[this, func,
       args...]` was a comment the caller was trusted to have read, and the first caller to pass two arguments
       into a three-slot buffer dupped a controller one slot PAST the array — onto the next field of the owning
       state, which happened to be that same controller. It failed nowhere near the write, as a reaction
       capturing "something that is not a controller". The capacity travels through STEP_CB so it is derived
       from the array rather than restated, and a caller that forwards a bare pointer gets a capacity of zero
       and fails here on its first call instead of silently. */
    DCHECK(cb_cap >= 2 + argc, "a host call request has more arguments than its buffer has slots");

    if (*phase == 0) {
        JS_FreeValue(ctx, in);
        cb[0] = js_dup(this_val);
        cb[1] = js_dup(func);
        for (i = 0; i < argc; i++)
            cb[2 + i] = js_dup(argv[i]);
        *out_cb = cb;
        *out_argc = argc;
        *phase = 1;
        return 3;
    }
    DCHECK(*phase == 1, "a host call request resumed in a phase it never parked in");
    *phase = 0;
    for (i = 0; i < 2 + argc; i++) {
        JS_FreeValue(ctx, cb[i]);
        cb[i] = JS_UNDEFINED;
    }
    *pout = in;
    return 0;
}

/* A [[Construct]] AS A REQUEST — the same sub-sequence over the CONSTRUCT operand shape. See quickjs-step.h.
   The only difference from the call above is the buffer: [ctor, args...] with no receiver slot, because a
   construct MAKES the receiver. That shape is the driver's, not this function's — do_step_step's
   JS_STEP_CONSTRUCT arm reads cb[0] as the constructor and &cb[1] as the arguments — which is why the two
   cannot share one helper with a flag. */
int step_construct_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValueConst ctor,
                       int argc, JSValueConst *argv, JSValue in, JSValue *pout,
                       JSValue **out_cb, int *out_argc)
{
    int i;

    DCHECK(cb_cap >= 1 + argc, "a host construct request has more arguments than its buffer has slots");

    if (*phase == 0) {
        JS_FreeValue(ctx, in);
        cb[0] = js_dup(ctor);
        for (i = 0; i < argc; i++)
            cb[1 + i] = js_dup(argv[i]);
        *out_cb = cb;
        *out_argc = argc;
        *phase = 1;
        return JS_STEP_CONSTRUCT;
    }
    DCHECK(*phase == 1, "a host construct request resumed in a phase it never parked in");
    *phase = 0;
    for (i = 0; i < 1 + argc; i++) {
        JS_FreeValue(ctx, cb[i]);
        cb[i] = JS_UNDEFINED;
    }
    *pout = in;
    return 0;
}


/* the same sub-sequence delivering 7.1.20 ToLength, which is ToIntegerOrInfinity CLAMPED to [0, 2**53-1]. A
   saturating ToInt64 cannot stand in for it — a negative index must become 0, not stay negative — so it gets its
   own tail like the others rather than being approximated from one. */
static int step_tolength_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, int64_t *pres,
                             JSValue **out_cb, int *out_argc)
{
    if (h->num_phase == NUM_PH_START) {
        JS_FreeValue(ctx, in);
        /* §7.1.4 steps 7-10 over unknown external input — see step_coerce_unknown. */
        if (unlikely(g_concolic.is(v)))
            STEP_COERCE_UNKNOWN(step_tolength_run, ctx, h, v, JS_UNDEFINED, &h->num_phase, pres, out_cb, out_argc);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;     /* borrowed view */
            *out_cb = h->cb_coerce; *out_argc = HINT_NUMBER;
            h->num_phase = NUM_PH_PRIM;
            return 5;
        }
        return JS_ToLengthFree(ctx, pres, js_dup(v));
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->num_phase = NUM_PH_START;
    /* …and the other way in: the page's own valueOf ANSWERED with unknown external input. */
    if (unlikely(g_concolic.is(in)))
        STEP_COERCE_UNKNOWN(step_tolength_run, ctx, h, in, in, &h->num_phase, pres, out_cb, out_argc);
    return JS_ToLengthFree(ctx, pres, in);
}

/* the same sub-sequence delivering ToInt32's exact result. A saturating int64 cannot stand in for it: ToInt32
   WRAPS modulo 2**32, so parseInt's radix of 2**32+16 is 16, and any narrowing applied after a saturation has
   already lost that. Each conversion gets its own tail rather than one being approximated from another. */
static int step_toint32_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, int *pres,
                            JSValue **out_cb, int *out_argc)
{
    if (h->num_phase == NUM_PH_START) {
        JS_FreeValue(ctx, in);
        /* §7.1.4 steps 7-10 over unknown external input — see step_coerce_unknown. */
        if (unlikely(g_concolic.is(v)))
            STEP_COERCE_UNKNOWN(step_toint32_run, ctx, h, v, JS_UNDEFINED, &h->num_phase, pres, out_cb, out_argc);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;
            *out_cb = h->cb_coerce; *out_argc = HINT_NUMBER;
            h->num_phase = NUM_PH_PRIM;
            return 5;
        }
        return JS_ToInt32(ctx, pres, v);
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->num_phase = NUM_PH_START;
    /* …and the other way in: the page's own valueOf ANSWERED with unknown external input. */
    if (unlikely(g_concolic.is(in)))
        STEP_COERCE_UNKNOWN(step_toint32_run, ctx, h, in, in, &h->num_phase, pres, out_cb, out_argc);
    return JS_ToInt32Free(ctx, pres, in);
}

/* the same sub-sequence delivering a double: String.prototype.lastIndexOf's position is compared as one
   (NaN means "from the end"), which a saturating int64 cannot express. */
/* ToIntegerOrInfinity CLAMPED to int (Array.prototype.flat's depth): the same shape as step_toint32_run, differing
   only in the conversion applied once the operand is primitive — ToInt32 WRAPS where this SATURATES, and a depth of
   2**32 must flatten everything rather than nothing. */
static int step_toint32sat_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, int *pres,
                               JSValue **out_cb, int *out_argc)
{
    if (h->num_phase == NUM_PH_START) {
        JS_FreeValue(ctx, in);
        /* §7.1.4 steps 7-10 over unknown external input — see step_coerce_unknown. */
        if (unlikely(g_concolic.is(v)))
            STEP_COERCE_UNKNOWN(step_toint32sat_run, ctx, h, v, JS_UNDEFINED, &h->num_phase, pres, out_cb, out_argc);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;
            *out_cb = h->cb_coerce; *out_argc = HINT_NUMBER;
            h->num_phase = NUM_PH_PRIM;
            return 5;
        }
        return JS_ToInt32Sat(ctx, pres, v);
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->num_phase = NUM_PH_START;
    /* …and the other way in: the page's own valueOf ANSWERED with unknown external input. */
    if (unlikely(g_concolic.is(in)))
        STEP_COERCE_UNKNOWN(step_toint32sat_run, ctx, h, in, in, &h->num_phase, pres, out_cb, out_argc);
    { int r = JS_ToInt32Sat(ctx, pres, in); JS_FreeValue(ctx, in); return r; }
}

static int step_tofloat64_run(JSContext *ctx, JSStepHdr *h, JSValueConst v, JSValue in, double *pres,
                              JSValue **out_cb, int *out_argc)
{
    if (h->num_phase == NUM_PH_START) {
        JS_FreeValue(ctx, in);
        /* §7.1.4 steps 7-10 over unknown external input — see step_coerce_unknown. */
        if (unlikely(g_concolic.is(v)))
            STEP_COERCE_UNKNOWN(step_tofloat64_run, ctx, h, v, JS_UNDEFINED, &h->num_phase, pres, out_cb, out_argc);
        if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
            DCHECK(JS_VALUE_GET_TAG(h->coerce) != JS_TAG_OBJECT,
                   "a coercion is already in flight on this machine's header");
            h->coerce = js_dup(v);
            h->cb_coerce[0] = h->coerce;
            *out_cb = h->cb_coerce; *out_argc = HINT_NUMBER;
            h->num_phase = NUM_PH_PRIM;
            return 5;
        }
        return JS_ToFloat64(ctx, pres, v);
    }
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->num_phase = NUM_PH_START;
    /* …and the other way in: the page's own valueOf ANSWERED with unknown external input. */
    if (unlikely(g_concolic.is(in)))
        STEP_COERCE_UNKNOWN(step_tofloat64_run, ctx, h, in, in, &h->num_phase, pres, out_cb, out_argc);
    return JS_ToFloat64Free(ctx, pres, in);
}

/* `? ToLength(? Get(obj, key))` — the pair LengthOfArrayLike is, and the pair 22.2.7.8 step 4 performs on
   `lastIndex`. Both halves are the page's code: the read is an accessor or a trap, and the ToLength coerces
   whatever it produced. ONE implementation, the key as a parameter, so the two spec operations cannot drift.
   0 = *plen is filled, 5/6 = the caller must return that step code, -1 = threw. */
static int step_getlen_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSAtom key, JSValue in, int64_t *plen,
                           JSValue **out_cb, int *out_argc)
{
    switch (h->len_phase) {
    case LEN_PH_START:
        JS_FreeValue(ctx, in);
        h->cb_coerce[0] = (JSValue)obj;   /* borrowed: the machine holds the receiver */
        *out_cb = h->cb_coerce; *out_argc = (int)key;
        h->len_phase = LEN_PH_GOT;
        return 6;
    case LEN_PH_GOT:                      /* `in` is the value the read produced */
        return step_length_value(ctx, h, in, plen, out_cb, out_argc);
    case LEN_PH_UNKNOWN:                  /* re-entered at an outcome fork; the unknown rides `coerce` */
        JS_FreeValue(ctx, in);
        return step_length_unknown(ctx, h, plen);
    default:                             /* `in` is the settled primitive */
        DCHECK(h->len_phase == LEN_PH_PRIM, "a keyed ToLength resumed in an unknown phase");
        JS_FreeValue(ctx, h->coerce);
        h->coerce = JS_UNDEFINED;
        h->len_phase = LEN_PH_START;
        return JS_ToLengthFree(ctx, plen, in);
    }
}

int step_length_run(JSContext *ctx, JSStepHdr *h, JSValueConst obj, JSValue in, int64_t *plen,
                    JSValue **out_cb, int *out_argc)
{
    return step_getlen_run(ctx, h, obj, JS_ATOM_length, in, plen, out_cb, out_argc);
}

