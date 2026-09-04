/* The concolic value type — see concolic.h. A host component built on upstream quickjs's PUBLIC class API, so
   the qjs fork carries no value-type delta. */
#include "solver/concolic.h"
#include "solver/absent.h"
#include "solver/flow.h"
#include "solver/decide.h"    /* the ONE speller of a constraint key — see concolic_exotic_own_names' read */
#include "solver/solve.h"     /* …and the SEARCH is told a substitution happened — see concolic_deliver */
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "check.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* The per-value state hung off the JSObject via JS_SetOpaque.
 *
 * `src`, `root` AND `ident` ARE THREE DIFFERENT FACTS, AND ONE FIELD HAS TWICE BEEN CAUGHT ANSWERING TWO.
 *   `src` is the INJECTION IDENTITY — the source an @S candidate substitutes at. A derivation is entitled to
 *   mint a new one and two of them do: a field read of an unknown OBJECT is an independently controlled datum
 *   (`{state}.admin`), and so is the result of calling an unknown.
 *   `root` is the DELIVERY PROVENANCE — the source whose component physically carried these bytes into the
 *   program. It is INHERITED unchanged through EVERY derivation, including the two above, because nothing a
 *   derivation does can change how the attacker got the bytes there. `location.hash.slice(1)` is no longer the
 *   fragment for INJECTION (the `#` is gone, and concolic_lead_hook says so) and is still the fragment for
 *   DELIVERY, which is what a reproduction envelope and a percent-encode set are questions about.
 *   `ident` is IDENTITY — WHICH VALUE THIS IS. It is COMPOSED at every derivation, because it is what the
 *   per-flow path constraint is keyed by, and a constraint key must name the value a branch tests.
 * With one field answering both, `x`, `x*2`, `x < 700` and `x < 300` were ONE fact: a flow that decided any of
 * them decided all of them, so feasible-refinement pruned arms the flow's constraint does not contradict —
 * the one direction §Solver-half forbids ("a contradicted branch is pruned (sound-only — uncertainty keeps
 * the arm)"), and the one that no gate can see, because a pruned arm emits nothing to be wrong.
 *
 * `ident` IS ABSENT (NULL) WHERE THIS ENGINE CANNOT SPELL THE VALUE EXACTLY — an operand that is a plain
 * object or a symbol, a property name that would not convert. That is a POSITIVE statement and not a hole: a
 * value with no identity is never decided from another value's record, so BOTH arms of every branch over it
 * stay. Absence costs forks; a wrong identity costs the arm. */
/* WITH `src` ANSWERING THE DELIVERY QUESTION TOO, THE REPORT LIED ABOUT A FINDING IT HAD JUST FIRED. A fixture
 * doing `eval("var t='" + location.hash.slice(1) + "';")` was loaded in real Chrome at
 * `http://127.0.0.1:8781/#';X9()//`; the sink fired, and the popup's reproduction envelope said "the engine
 * declares no browser delivery for this source — nothing carries or transforms these bytes on the way in, so
 * there is no navigation that reproduces it", about a payload delivered in the victim's URL fragment by the
 * single navigation that had just been performed. The chain is `location.hash` (src `location.hash`, declared)
 * -> concolic_exotic_get `.slice` (src `{location.hash}.slice`) -> concolic_call `()` (src
 * `{location.hash}.slice()`), and the registry is an exact strcmp, so it matched nothing. Nothing was broken
 * on the consumer side: reading absence as "the source declared none" is exactly what a consumer owes a
 * producer. The producer had thrown the fact away three derivations earlier. */
/* "THIS PREDICATE IS NOT AN EQUALITY", WHICH IS NOT A JSConcolicEqOp AND MUST NOT BE ENCODED AS ONE. quickjs
   declares the two algorithms an equality OPERATOR can name; an ordering names neither, and reusing LOOSE's
   zero for it would make `x < 700` answer that the page wrote `==`. Negative so it cannot collide with a
   future member of that enum either. */
enum { CMP_ALGO_NONE = -1 };
typedef struct {
    char *shape;        /* @H/@S display form */
    char *src;          /* INJECTION IDENTITY: the source an @S candidate substitutes at */
    char *root;         /* DELIVERY PROVENANCE: where the bytes ENTERED. Inherited unchanged; NULL iff !src */
    char *ident;        /* IDENTITY: this exact value, composed below. NULL = this engine cannot spell it */
    /* WHICH PREDICATE A BRANCH OVER THIS VALUE IS ASKING ABOUT, AND WITH WHAT POLARITY — a SECOND name, and
       the two are not interchangeable because they answer two questions. `ident` names THIS VALUE, which is
       what a DERIVATION composes from (`"" + !p` and `"" + p` must not collide). `br_key` names the value
       whose TRUTH this one's truth is a function of, which is what the path constraint is keyed by — and for
       a negation that is the OPERAND, because `!p` is not a second predicate about `p`, it is `p` read the
       other way round. §7.1.2 ToBoolean is applied on BOTH sides of `if (p)` and `if (!p)` alike, so nothing
       is lost or invented by saying so: one question, two spellings, ONE constraint entry, and a flow that
       decided `p` decides `!p` without forking again.
       NULL br_key = this value's own `ident` is the key, which is every value that is not a negation. The two
       are written together at the one mint that produces them and asserted together there; `br_neg` may be 1
       only where `br_key` names something, since a polarity against no predicate is a fact about nothing. */
    char *br_key;
    signed char br_neg; /* 1 = this value is the logical COMPLEMENT of the predicate `br_key` names */
    JSValue example;    /* concrete example, or JS_UNDEFINED */
    /* THIS VALUE'S OWN [[Prototype]], DERIVED ONCE AND HELD — §10.1.1 [[GetPrototypeOf]] ( ) over an unknown,
       memoised because the answer must have an IDENTITY the page can compare. A fresh derivation per call
       answers `Object.getPrototypeOf(x) === Object.getPrototypeOf(x)` FALSE, which is a wrong answer no page
       can be written around; and §10.4.7.2 SetImmutablePrototype ( obj, proto ) step 2 is a SameValue against
       whatever step 1 returned, so a value that differs per call cannot satisfy its own [[SetPrototypeOf]].
       JS_UNINITIALIZED = not yet asked. It is written here rather than left to reclaim_calloc for cmp_algo's
       reason — the allocator's zero is a JSValue this file did not choose, and "nobody has asked yet" is a
       state that has to be spelled rather than inherited.
       IT IS NOT PER-FLOW STATE AND MUST NOT BE CAPTURED, which is the identical argument the key-name map
       states about itself two hundred lines down: the prototype of a value is a pure function of that value,
       so two flows asking mint ONE answer and neither can disagree with the other. What is per-flow is
       anything the page then WRITES through the link, and that is an ordinary property write the COW delta
       already carries. */
    JSValue proto;
    int cmp_op;         /* for an EQUALITY RESULT: OPCMP_EQ/NE — `src <op> cmp_tok` (else OPCMP_NONE) */
    char *cmp_tok;      /* the concrete side of the equality, SPELLED (§7.1.19 ToString of the operand) */
    signed char cmp_kind;  /* …and WHAT it spells (ConcolicLit) — the spelling's other half, never separable
                              from it: decide.c pins with both or pins nine characters where the page wrote
                              `undefined`. Written with `cmp_tok` at pred_new, copied with it at pred_copy. */
    /* …AND WHICH EQUALITY THE PAGE WROTE, which is what says what an arm is ENTITLED TO CONCLUDE and is
       therefore not a spelling of the operator that is already in `ident`. §7.2.14 IsStrictlyEqual ( x, y )
       step 1 returns false unless SameType, so a `===` that HELD determines the operand; §7.2.13
       IsLooselyEqual ( x, y ) coerces instead, and its holding arm leaves a SET for every kind this engine can
       spell — see concolic.h's concolic_pin for the step-by-step reading. Written with `cmp_op` at pred_new
       and carried with it through a negation, because the arm and the algorithm are read at one line of
       decide.c and a value holding one without the other pins a witness.
       CMP_ALGO_NONE FOR A PREDICATE THAT IS NOT AN EQUALITY, and it is not a JSConcolicEqOp: quickjs declares
       the two algorithms an OPERATOR can name, and "this is an ordering" is not one of them. Sharing the zero
       with LOOSE would make an ordering answer that the page wrote `==`. */
    signed char cmp_algo;
    char *cmp_subj;     /* …and the HOLE KEY of the unknown side — see concolic_cmp_subject. The four are ONE
                           observation: written together at pred_new, asserted together there, and read
                           together by decide.c, which pins on one arm and excludes on the other. */
    /* …AND THE UNKNOWN SIDE'S IDENTITY, WHICH IS A FOURTH NAME FOR A REASON AND NOT A DUPLICATE OF `cmp_subj`.
       The hole above is what a REPORT prints and is derived from the display shape by stripping braces, so it
       is a lossy, total-only-sometimes name: `{}` has no hole at all, and two shapes can strip to one string.
       This is the OPERAND VALUE's own identity — precise, composed, and the key the per-flow record of "this
       flow proved that value's example wrong" is filed under. Written at the same line as the hole and under
       the same condition, so a predicate cannot carry one and not the other.
       IT IS ABSENT WHEN BOTH SIDES ARE UNKNOWN, and that is a positive statement: `x === y` over two examples
       proves, on the contradicting arm, that at least one of them is wrong and never WHICH, so naming either
       would be the invention §@H forbids. The pin and the exclusion already refuse that case for the same
       reason (there is no concrete side to spell), which is why one condition covers all three. */
    char *cmp_subj_ident;
    /* …AND THE ORDERING'S OWN THREE FIELDS, which are the SAME observation for the other half of §Solver-half's
       two-facts rule and are deliberately NOT the equality's. An equality determines a VALUE on one arm; an
       ordering determines a BOUND on both, so the two cannot share `cmp_op` without one of them answering a
       question it was not asked. `rel_op` is normalised SUBJECT-ON-THE-LEFT (`5 < x` arrives as `x > 5`), so
       decide.c reads a relation it can state without knowing which operand the page wrote first. */
    RelOp rel_op;       /* for an ORDERING RESULT: the relation, subject-left (else REL_NONE)  */
    char *rel_tok;      /* the concrete side SPELLED — the page's own §6.1.6.1.20 Number::toString */
    double rel_num;     /* …and its value, which is what a merge orders two bounds by */
    char *rel_subj;     /* …and the HOLE KEY of the unknown side. All four written and asserted together. */
    /* THE PROPERTY NAME THIS VALUE WAS READ UNDER — written by concolic_exotic_get and by nothing else, so it
       is present exactly on a MEMBER READ of an unknown and NULL on every other derivation. It exists because
       the call handler below needs to name the method the page invoked, and the only honest source for that
       is the ATOM the read went through: recovering it from the display shape would be a parse of a string
       this file composed for a human, and a `.` in a property name would decide it wrong. */
    char *member;
    /* …AND THE CALL PREDICATE'S OWN FOUR FIELDS — the third class, for the reason the ordering's three are not
       the equality's. A call determines neither a value nor an interval and still narrows, so it can share
       neither group without answering a question it was not asked. All four are written together at
       pred_set_strpred and read together by concolic_strpred; `sp_meth` is the presence test, exactly as
       `rel_op != REL_NONE` is the ordering's. */
    char *sp_meth;      /* the method the page called on the unknown — c->member of the CALLEE */
    char **sp_args;     /* every argument SPELLED, all of them or (when one is unspellable) none at all */
    int   sp_nargs;
    char *sp_subj;      /* …and the HOLE KEY of the RECEIVER, which is what the emission looks a domain up by */
    /* …AND THE FOURTH CLASS, WHICH IS THE ONE WHOSE OPERANDS ARE PREDICATES RATHER THAN VALUES: the conjuncts
       this value's truth is the conjunction of. It is a SET — sorted by identity and holding each member once
       — so `p ∧ q` and `q ∧ p` compose ONE key and no flow can stand on both answers to one proposition.
       IT IS RECORDED AS A FACT AND NEVER READ BACK OUT OF `ident`. The identity composes these same members
       under the conjunction tag, so the set is recoverable from that string by PARSING it — which is the
       matching §RUN-DON'T-MATCH forbids and what a change to the key format would silently break. The one
       reader is the mint itself, which FLATTENS a conjunct that is already a conjunction.
       PRESENT EXACTLY WHERE THE IDENTITY IS A SPELLED CONJUNCTION: `conj_n >= 2` iff this value is a
       conjunction this engine could name, and 0 everywhere else — including on a conjunction one of whose
       members was unspellable (its `ident` is NULL, so it contributes NULL to any parent and makes that
       parent unspellable too, which is the same answer the list would have given), and including on a
       NEGATION of one, because `!(p ∧ q)` is not a conjunction and flattening it would spell `p` and `q` as
       conjuncts of something that is neither. */
    char **conj;
    int    conj_n;
} Concolic;

/* ONE DISPOSER FOR AN OWNED LIST OF SPELLED STRINGS, because several records hold one — a call predicate's
   arguments on the value (`sp_args`) and on the constraint (ConcolicPred), and a conjunction's conjuncts —
   and a list freed at one site only is a leak the runtime's own GC walk cannot see, since these are C strings
   and not GC objects. */
static void ident_list_free(char **args, int n) {
    int i;
    for (i = 0; i < n; i++) free(args[i]);
    free(args);
}

void concolic_pred_copy(ConcolicPred *dst, const ConcolicPred *src) {
    int k;
    dst->method = strdup(src->method);
    CHECK(dst->method, "concolic: OOM copying the method a flow's own gate called");
    dst->nargs = src->nargs;
    dst->holds = src->holds;
    dst->args = NULL;
    if (!src->nargs) return;
    dst->args = malloc((size_t)src->nargs * sizeof(char *));
    CHECK(dst->args, "concolic: OOM copying a gate's argument list");
    for (k = 0; k < src->nargs; k++) {
        dst->args[k] = strdup(src->args[k]);
        CHECK(dst->args[k], "concolic: OOM copying an argument a flow's own gate passed");
    }
}

void concolic_pred_release(ConcolicPred *p) {
    free(p->method);
    ident_list_free(p->args, p->nargs);
    p->method = NULL; p->args = NULL; p->nargs = 0;
}

/* ── THE LOOSE EQUALITY'S HOLDING ARM, AS A ROW ──────────────────────────────────────────────────────────────
 * See concolic.h for what the row IS and for the three reasons it is not a ConcolicPred. These three functions
 * are the row's whole ownership contract, kept beside ConcolicPred's for the reason that pair states: three
 * holders keep a set of them, so a field added to the struct must have exactly one place it is copied and one
 * where it is released. */
int concolic_looseeq_same(const ConcolicLooseEq *p, ConcolicLit kind, const char *tok) {
    /* THE KIND IS HALF THE COMPARISON. §7.1.19 ToString ( arg ) flattens `undefined`, `null`, `0` and `false`
       onto text that is also a legal String operand, so two rows agreeing on the spelling alone would collapse
       `x == undefined` into `x == "undefined"` — two gates with two different holding sets, deduped into one
       claim the run never made. This is the same pair pred_new asserts and concolic_pin refuses to split. */
    return p->kind == (signed char)kind && !strcmp(p->tok, tok);
}

void concolic_looseeq_copy(ConcolicLooseEq *dst, const ConcolicLooseEq *src) {
    dst->kind = src->kind;
    dst->tok = strdup(src->tok);
    CHECK(dst->tok, "concolic: OOM copying the operand a flow's own loose equality held against");
}

void concolic_looseeq_release(ConcolicLooseEq *p) {
    free(p->tok);
    p->tok = NULL; p->kind = (signed char)CONCOLIC_LIT_NONE;
}

/* THE KIND AS A REPORT NAMES IT — see concolic.h for why this is not lit_tag. The switch has no `default`, so
   a kind added to ConcolicLit fails to compile here rather than being emitted under a neighbour's word. */
const char *concolic_lit_report_name(ConcolicLit k)
{
    switch (k) {
    case CONCOLIC_LIT_STRING:    return "string";
    case CONCOLIC_LIT_NUMBER:    return "number";
    case CONCOLIC_LIT_BOOL:      return "boolean";
    case CONCOLIC_LIT_NULL:      return "null";
    case CONCOLIC_LIT_UNDEFINED: return "undefined";
    case CONCOLIC_LIT_BIGINT:    return "bigint";
    case CONCOLIC_LIT_NONE:      break;
    }
    DFAILF("a domain row reached the emission carrying a kind this file has no report word for (%d) — a row is "
           "written only where the operand HAS a spelling, and a reviewer reading `== undefined` must be able "
           "to tell the value from the nine-character string, which is exactly what the kind says", (int)k);
    return NULL;
}

/* ── THE ONE ENCODING ───────────────────────────────────────────────────────────────────────────────────────
 * Every identity this file composes and every constraint key decide.c builds is a TAG plus a sequence of
 * FIELDS, written as `<byte length>:<bytes>` each. Two properties follow from that and both are load-bearing:
 *   - NO FIELD'S CONTENTS CAN SPELL ANOTHER FIELD'S BOUNDARY, so a member is never mistaken for a separator
 *     and two different sequences can never write the same string. The separator-plus-forbidden-character
 *     scheme concolic_source_wrap_joint uses cannot be applied here, because a member is often a PROPERTY NAME
 *     and a page may name a property anything at all — a DCHECK forbidding a byte in it would be an abort the
 *     page triggers.
 *   - THE LENGTH IS MEASURED AND WRITTEN BY THE SAME TWO LINES, so a buffer that could truncate does not
 *     exist. A truncated key is two different predicates under one name, which is the same defect as the
 *     collapsed relational key arriving by a second route: `decide_key` built into a `char key[256]` with an
 *     unchecked snprintf while concolic_exotic_get's shapes alone were 192 bytes.
 * A member that is ABSENT makes the whole composition absent — see the struct above. */
static size_t ident_field_len(const char *f)
{
    char pre[24];
    int w = snprintf(pre, sizeof pre, "%lu:", (unsigned long)strlen(f));
    DCHECK(w > 0 && (size_t)w < sizeof pre, "an identity field's length did not fit its own prefix");
    return (size_t)w + strlen(f);
}

static size_t ident_field_put(char *out, size_t at, const char *f)
{
    char pre[24];
    int w = snprintf(pre, sizeof pre, "%lu:", (unsigned long)strlen(f));
    size_t l = strlen(f);
    DCHECK(w > 0 && (size_t)w < sizeof pre, "an identity field's length did not fit its own prefix");
    memcpy(out + at, pre, (size_t)w);
    memcpy(out + at + (size_t)w, f, l);
    return at + (size_t)w + l;
}

char *concolic_ident_compose(const char *tag, const char *const *fields, int n)
{
    size_t len, at;
    char *out;
    int i;

    DCHECK(tag != NULL,
           "an identity was composed with no TAG — the tag is what keeps a field read, a call, an arithmetic "
           "result and a comparison over the same operands four different values");
    DCHECK(n == 0 || fields != NULL, "an identity was composed with a member count and no members");
    for (i = 0; i < n; i++)
        if (!fields[i]) return NULL;   /* an unspellable member makes the whole identity absent */
    len = ident_field_len(tag);
    for (i = 0; i < n; i++) len += ident_field_len(fields[i]);
    out = reclaim_malloc(len + 1);
    CHECK(out, "concolic: OOM composing an identity — a value whose identity could not be spelled would be "
               "decided by whatever constraint another value left under the key it fell back to");
    at = ident_field_put(out, 0, tag);
    for (i = 0; i < n; i++) at = ident_field_put(out, at, fields[i]);
    out[at] = '\0';
    DCHECK(at == len, "an identity was composed to a different length than it was measured for");
    return out;
}

/* THE CANONICAL ORDER OVER A SET OF IDENTITY STRINGS, and it lives HERE, with the encoding, because it is the
   second half of the same rule. The encoding above makes two different SEQUENCES compose two different keys;
   this is what makes two spellings of one SET compose ONE key — a joint provenance's sources, a conjunction's
   conjuncts. Without it a set's key would be a property of the order the arithmetic or the fold happened to
   assemble it in, so one question would have as many keys as it had spellings and a flow that decided under
   one would fork again under another while standing on both answers.
   IT ORDERS BY `strcmp` AND NOTHING ELSE, which is what makes the order outlive the flow that composed it: an
   identity is TEXT composed from program facts (an operand this engine cannot spell without an address has
   NO identity at all — see the struct), so this order is the same on the flow that minted the key, on the
   flow that resumes it from the cold tier, and in the next session.
   `order` is filled with the members' indices and the members are never moved, so a caller can still reach
   each one at the position its own record holds it in. */
static void ident_set_order(const char *const *idents, int n, int *order)
{
    int i, j;

    for (i = 0; i < n; i++) order[i] = i;
    for (i = 1; i < n; i++) {
        int cur = order[i];
        for (j = i; j > 0 && strcmp(idents[order[j - 1]], idents[cur]) > 0; j--) order[j] = order[j - 1];
        order[j] = cur;
    }
}

/* THE DISPLAY SHAPE, SIZED FROM ITS PARTS. Every shape in this file was a fixed 192- or 224-byte buffer, and
   concolic_exotic_get's shape is ALSO the field path an @S candidate is injected at — so a chain long enough
   to truncate gave two different sources one provenance. Measured once, written once, freed by the caller. */
static char *shapef(const char *fmt, ...)
{
    va_list ap;
    int n, m;
    char *out;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    CHECK(n >= 0, "concolic: a display shape could not be measured");
    out = reclaim_malloc((size_t)n + 1);
    CHECK(out, "concolic: OOM building a display shape");
    va_start(ap, fmt);
    m = vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    DCHECK(m == n, "a display shape was written to a different length than it was measured for");
    (void)m;
    return out;
}

/* WHICH KIND OF CONCRETE OPERAND THIS IS, AND WHETHER IT IS ONE THIS ENGINE MAY SPELL AT ALL — asked HERE and
   nowhere else, because a comparison spells its other operand TWICE (once as half the predicate's identity,
   once as the value the taken arm PINS the unknown to) and two classifications of one operand can disagree.
   They did: this function has said since it was written that an OBJECT or a SYMBOL is unspellable, and
   concolic_cmp_hook four hundred lines down called JS_ToCString on the same operand unconditionally. §7.1.19
   ToString step 10 sends an Object to ToPrimitive, which runs the page's own valueOf/toString from a C
   activation with no flow base under it — the abort JS_ToPrimitiveFree names — and step 2 throws a TypeError
   for a Symbol, which that hook would have left PENDING on ctx while reporting success. Every library utility
   that walks a prototype chain or tests an identity against a sentinel object reaches it on its first driven
   call: `while (n !== Object.prototype)` took four real bundles' worth of engine instances down. */
static ConcolicLit operand_kind(JSValueConst v)
{
    if (JS_IsString(v))    return CONCOLIC_LIT_STRING;
    if (JS_IsNumber(v))    return CONCOLIC_LIT_NUMBER;
    if (JS_IsBool(v))      return CONCOLIC_LIT_BOOL;
    if (JS_IsNull(v))      return CONCOLIC_LIT_NULL;
    if (JS_IsUndefined(v)) return CONCOLIC_LIT_UNDEFINED;
    if (JS_IsBigInt(v))    return CONCOLIC_LIT_BIGINT;
    /* AN OBJECT OR A SYMBOL — no ConcolicLit spelling, see above. That is a statement about the VALUE and
       not about whether the thing can be NAMED: an intrinsic is named by its realm slot, which literal_ident
       reaches through a different question entirely. */
    return CONCOLIC_LIT_NONE;
}

/* THE KIND'S ONE-LETTER FIELD IN A COMPOSED IDENTITY — one speller, because the identity a page's own
   `x === 5` composes and the identity a component's declared member composes must be BYTE-IDENTICAL or the
   two are two constraint entries over one predicate, each forking the other's settled gate. That is the
   property page_visibility.h states, and it is what made the hardcoded `"s"` at concolic_new_cmp a defect
   rather than a shorthand: a second speller cannot be wrong about a string and cannot be right about
   anything else. */
static const char *lit_tag(ConcolicLit k)
{
    switch (k) {
    case CONCOLIC_LIT_STRING:    return "s";
    case CONCOLIC_LIT_NUMBER:    return "n";
    case CONCOLIC_LIT_BOOL:      return "b";
    case CONCOLIC_LIT_NULL:      return "z";
    case CONCOLIC_LIT_UNDEFINED: return "u";
    case CONCOLIC_LIT_BIGINT:    return "g";
    case CONCOLIC_LIT_NONE:      break;
    }
    DFAILF("a concrete operand was spelled under a kind this file does not name (%d) — the kind is composed "
           "into the predicate's identity, so an unnamed one either composes no key at all or shares the key "
           "of a comparison against a different type", (int)k);
    return NULL;
}

/* THE PINNED VALUE, BACK — the read-back half of ConcolicLit, and the ONE place a stored (kind, spelling) pair
   becomes a JSValue again, so the two read sites cannot disagree about what a pin means.
   IT MINTS THE REAL VALUE AND NOT A DISPLAY OF IT, which is the whole point: §Solver-half's concretize-on-pin
   says a later branch "is decided by RUNNING the real predicate on a real string", and the interpreter can
   only do that if what it is handed is the value the flow proved. A Number comes back through §7.1.4
   ToNumber ( arg ) — the engine's own inverse of the §7.1.19 ToString ( arg ) that spelled it, run on a
   String primitive so it reaches neither §7.1.1 ToPrimitive nor any of the page's code.
   -0 COMES BACK AS +0 AND THAT IS A WITNESS RATHER THAN A FABRICATION: §6.1.6.1.20 Number::toString spells
   both zeroes "0", and §7.2.14 IsStrictlyEqual answers true for `-0 === 0`, so +0 satisfies exactly the
   predicate this flow observed. */
static JSValue pin_mint(JSContext *ctx, ConcolicLit kind, const char *val)
{
    DCHECK(val != NULL, "a pinned value was minted from no spelling — the kind and the text are written "
                        "together at concolic_pin, so a kind with no text is an entry whose own writer did "
                        "not know what it had pinned");
    switch (kind) {
    case CONCOLIC_LIT_STRING:    return JS_NewString(ctx, val);
    case CONCOLIC_LIT_NULL:      return JS_NULL;
    case CONCOLIC_LIT_UNDEFINED: return JS_UNDEFINED;
    case CONCOLIC_LIT_BOOL:
        DCHECK(!strcmp(val, "true") || !strcmp(val, "false"),
               "a Boolean pin was spelled as something §7.1.19 ToString of a Boolean never produces — the "
               "spelling is the operand the page's own predicate wrote, so anything else is a token minted "
               "somewhere that does not go through literal_tok");
        return JS_NewBool(ctx, !strcmp(val, "true"));
    case CONCOLIC_LIT_NUMBER: {
        JSValue s = JS_NewString(ctx, val), n;
        if (JS_IsException(s)) return s;
        n = JS_ToNumber(ctx, s);
        JS_FreeValue(ctx, s);
        DCHECK(!JS_IsException(n),
               "§7.1.4 ToNumber ( arg ) threw over a String primitive — its String arm is §7.1.4.1 ToNumber "
               "Applied to the String Type, which has no abrupt completion, so an exception here is a value "
               "that is not the string this line just minted");
        return n;
    }
    case CONCOLIC_LIT_BIGINT:
    case CONCOLIC_LIT_NONE:
        break;
    }
    DFAILF("a pin was stored under a kind the read-back cannot mint (%d) — concolic_pin refuses exactly the "
           "kinds this switch cannot answer, so reaching here means the store accepted one it may not hold "
           "and every later read of that source is about to compute a value of the wrong type", (int)kind);
    return JS_UNDEFINED;
}

/* THE OPERAND SPELLED — its §7.1.19 ToString, owned by the caller, NULL where it has none. This is also the
   value an equality PINS to, and that is one fact rather than two: `""+x` is what a pinned source contributes
   to a URL the page builds, so the pin and the identity must be the same bytes or a flow's later reads compute
   a different string from the one the predicate matched. */
static char *literal_tok(JSContext *ctx, JSValueConst v)
{
    const char *s;
    char *r;

    if (operand_kind(v) == CONCOLIC_LIT_NONE) return NULL;
    s = JS_ToCString(ctx, v);
    if (!s) return NULL;
    r = strdup(s);
    CHECK(r, "concolic: OOM spelling a concrete operand");
    JS_FreeCString(ctx, s);
    return r;
}

/* AN INTRINSIC'S NAME, OWNED BY THE CALLER — the ONE speller of it, for the reason lit_tag is one: this name
   is spent TWICE, once as an operand's IDENTITY and once as its DISPLAY SHAPE, and a second speller cannot be
   wrong about a prototype and right about a constructor.
   THE REGISTRY IS QUICKJS'S AND NOT THIS FILE'S (JS_IntrinsicName): a realm's class_proto/class_ctor arrays,
   its global object, and the well-known symbols below JS_ATOM_END. Each of those is fixed at its own
   definition, which is the one condition under which a POSITION may name anything, and it is what makes the
   name reproducible by the replay a resumed flow performs — the actual requirement, rather than
   uniqueness-in-a-heap, since `js_malloc`/`js_free_rt` REUSE addresses and no park carries one.
   NULL WHERE THE VALUE NAMES NO INTRINSIC, which is the answer every Object and Symbol gave unconditionally
   before this and is still the answer for a PAGE-created one — a page-created object is named by its creation
   site plus the creating flow's count of prior creations there, which is a fact about the executed prefix and
   is NOT built yet; see the named residual at literal_ident below. */
static char *intrinsic_name(JSContext *ctx, JSValueConst v)
{
    char buf[JS_INTRINSIC_NAME_MAX];
    int n = JS_IntrinsicName(ctx, v, buf, sizeof buf);
    char *r;

    if (n < 0) return NULL;
    DCHECK(n < (int)sizeof buf,
           "an intrinsic's name did not fit the buffer quickjs.h's own bound sizes for it — the two are tied "
           "by a _Static_assert there, so this fires only if that bound was widened past this buffer, and a "
           "TRUNCATED name is two intrinsics under one constraint key and one property atom");
    r = strdup(buf);
    CHECK(r, "concolic: OOM copying an intrinsic's name");
    return r;
}

/* A CONCRETE OPERAND'S IDENTITY IS ITS VALUE, AND ITS TYPE IS PART OF THAT — `x === 5` and `x === "5"` are two
   predicates and their operands print the same.
   AN OBJECT OR A SYMBOL HAS NO IDENTITY IN THAT SENSE — its address does not survive the park a resumed flow
   replays through, and coercing it would run the page's own `toString` from C — AND THAT ARGUMENT IS ABOUT THE
   VALUE, WHICH IS NOT THE ONLY THING THAT CAN NAME ONE. A value has as many name sources as it has kinds of
   CREATOR, and an INTRINSIC is named by the slot it occupies rather than by anything about its bytes, so the
   arm below answers for one and this paragraph keeps its force for every other object: unnamed, absent
   identity, and both arms of the branch stay. */
static char *literal_ident(JSContext *ctx, JSValueConst v)
{
    ConcolicLit kind = operand_kind(v);
    const char *f[2];
    char *tok, *r;

    if (kind == CONCOLIC_LIT_NONE) {
        /* …UNLESS IT IS AN INTRINSIC, WHICH IS A SINGLETON OF ITS REALM AND SO IS NAMED BY THE SLOT IT
           OCCUPIES. The paragraph above is about a value whose only name would be its ADDRESS; an intrinsic
           has a second name source that is not an address at all, and refusing it made
           `while (n !== Object.prototype)` — the shape every prototype walk in every library ends on — a
           branch with NO identity, which decide.c files as a bare site row that "records no constraint, claims
           no replay slot, and re-forks every time the flow reaches it".
           THE TAG IS ITS OWN AND NOT `k`, because an intrinsic is not a spelled VALUE: `k` composes a
           ConcolicLit kind with a §7.1.19 ToString, and this composes a registry slot. Two namespaces under
           one tag would let a page's string `"%Array%"` and %Array% itself share a constraint key.
           NO PIN FOLLOWS FROM IT, and that is not an omission: literal_tok still answers NULL here, so
           concolic_cmp_hook mints no token, and §7.2.14 IsStrictlyEqual step 1 is why — SameType(x, y) is
           false for every string this solver could substitute, so there is nothing an object operand could
           pin the unknown TO. The predicate is named; the value is not determined.
           NAMED RESIDUAL — A PAGE-CREATED OBJECT IS STILL UNNAMED, so `var o = {}; o[k]` keeps both arms and
           its bare site row. What the next diff builds is the OTHER name source: the object's creation site
           (which quickjs already composes for a function body at JS_OrphanHash) PLUS THE CREATING FLOW'S OWN
           COUNT OF PRIOR CREATIONS AT THAT SITE, because a site is 1:N with the objects made there and one
           `{}` in a loop is one site and a thousand objects — a fact about the EXECUTED PREFIX, so a replay
           reproduces it by reproducing the prefix and a fork carries it as it carries every other prefix
           quantity. ITS ABSENCE SHOWS as a `~` site row whose subject renders `?` — derived_operand_shape's
           answer for an operand with no name — climbing across a session while `replayHits` stays flat.
           `{}` IS NOT THAT SPELLING AND NAMING IT AS ONE SENDS A READER TO GREP FOR THE WRONG THING: `{}` is
           what a CONCOLIC handed no shape renders as, and it was also this site's own spelling for an object
           operand until derived_operand_shape was routed here — so a census row `~{}[…]` quoted from an older
           artifact and a row `~?[…]` measured today are ONE population under two spellings, and only the
           second is greppable in this tree.
           Building the site half WITHOUT the ordinal is worse than leaving it: three iterations of one loop
           would name one object, so iteration 1's constraint would refine 2 and 3 and ARMS WOULD BE LOST
           rather than duplicated.

           AND THE POPULATION THAT NEEDS IT IS NOT HYPOTHETICAL — IT IS WHAT `G = G || {}` LEAVES BEHIND, which
           is the ordinary spelling of every shared-store and hydration shim a real minified bundle ships, and
           it is the SAME line §Solver-half names as the counterexample refuting the banned retire-the-symbolic-
           arm fix. Read for what each arm HOLDS rather than for which arm survives: where the global is absent
           — the arm this engine must keep — the store is a freshly created `{}` belonging to the page, so an
           INTRINSIC namer cannot reach it however complete it is, and every later `store[wks]` read off it is
           unnamed. That is the exact shape that makes the site-plus-ordinal half load-bearing rather than a
           tidy completion, and it is why naming intrinsics does not by itself settle whether a given heavy row
           is in scope: one real bundle carries both populations at once, computed keys landing on DOM class
           prototypes (which this half names) and on that page-created store (which it does not).

           AND WHAT A NAME BUYS IS THE COLLAPSE OF A REPEAT, NEVER OF A BREADTH — which decides how the fork
           census is read after this lands, and reading it the other way makes a working namer look broken. A
           named branch over an unknown value STILL FORKS: naming is what lets concolic_branch_decided refine
           the SAME question asked again, so N DISTINCT named operands owe N distinct questions and exactly
           the worlds they owed before. Collapsing those would be the wrong narrowing §Solver-half forbids.
           So on a BREADTH document the win is that ONE site row of N becomes N predicate rows summing to N,
           each with a replay slot — the frontier is unchanged and that is CORRECT — and only on a REPETITION
           document does the frontier fall. Measured on frozen bytes, one intrinsic and one unknown key
           branched R times, three runs each, identical: before the namer the frontier is 2^R and the forks
           2^R - 1 (2/1, 4/3, 8/7 at R = 1, 2, 3), with a PAGE-CREATED base giving BYTE-IDENTICAL numbers —
           which is what makes that base the control for this one. */
        char *nm = intrinsic_name(ctx, v);
        const char *nf[1];
        if (!nm) return NULL;
        nf[0] = nm;
        r = concolic_ident_compose("i", nf, 1);
        free(nm);
        return r;
    }
    tok = literal_tok(ctx, v);
    if (!tok) return NULL;
    f[0] = lit_tag(kind); f[1] = tok;
    r = concolic_ident_compose("k", f, 2);
    free(tok);
    return r;
}

/* An operand's identity whichever kind it is, always OWNED by the caller (NULL = unspellable). */
static char *ident_of_operand(JSContext *ctx, JSValueConst v)
{
    if (concolic_is(v)) {
        const char *id = concolic_ident_c(v);
        char *r;
        if (!id) return NULL;
        r = strdup(id);
        CHECK(r, "concolic: OOM copying an operand's identity");
        return r;
    }
    return literal_ident(ctx, v);
}

/* A STRING OPERAND, RENDERED AS THE PAGE WROTE IT. The quotes are not decoration: §7.1.19 ToString spells the
   Number 5 and the String "5" with the same byte, so a display that showed both as `5` would be COARSER than
   the identity beside it, which separates them by kind. The two characters that could close the rendering
   early are escaped for the same reason — `f("a\"", "b")` and `f("a", "\"b")` must not render alike. OWNED. */
static char *shape_quote(const char *s)
{
    size_t i, j, n = strlen(s), extra = 0;
    char *out;

    for (i = 0; i < n; i++) if (s[i] == '"' || s[i] == '\\') extra++;
    out = reclaim_malloc(n + extra + 3);
    CHECK(out, "concolic: OOM quoting a string operand for a display shape");
    out[0] = '"';
    j = 1;
    for (i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\') out[j++] = '\\';
        out[j++] = s[i];
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

/* ONE OPERAND'S DISPLAY — THE SHAPE TWIN OF ident_of_operand ABOVE, AND IT LIVES HERE BECAUSE THE TWO ARE ONE
   RULE. An unknown renders as its own hole shape and a concrete one as the LITERAL the page wrote; an operand
   this engine may not spell at all — a PAGE-CREATED Object or Symbol, operand_kind's own last line — renders
   "?", and it has already made the whole IDENTITY absent through concolic_ident_compose's unspellable-member
   rule. An INTRINSIC renders as its %-name, because literal_ident composes one for it and this pair moves
   together or the assert at keyname_record catches the half that did not.
   THE RULE THE PAIR ENFORCES, and it is an invariant rather than a preference: A SHAPE MUST SEPARATE EVERY
   PAIR OF OPERANDS ITS IDENTITY SEPARATES. keyname_record spends a display shape to buy a property atom, so a
   shape coarser than the identity beside it makes two distinct unknowns ONE slot on the page's own object —
   and the assert there is the one that catches it. That is why the kind is rendered here (a quoted String, a
   BigInt's `n`) and not dropped as §7.1.19 ToString drops it: `f(x, 5)` and `f(x, "5")` are two derivations
   and two identities, so they are owed two shapes.
   literal_tok AND NOT JS_ToCString, which is the same distinction operand_kind's own comment draws: §7.1.19
   ToString step 10 sends an Object to ToPrimitive, running the page's own valueOf from a C activation with no
   flow base under it. OWNED by the caller. */
static char *derived_operand_shape(JSContext *ctx, JSValueConst v)
{
    ConcolicLit kind;
    char *tok, *r;

    if (concolic_is(v)) {
        const char *sh = concolic_shape_c(v);
        return shapef("%s", sh ? sh : "{}");
    }
    kind = operand_kind(v);
    tok = literal_tok(ctx, v);
    /* AN INTRINSIC RENDERS AS ITS OWN NAME, AND IT IS THE SAME NAME literal_ident COMPOSED ITS IDENTITY FROM —
       which is this pair's whole invariant restated for one more kind of operand. Naming the identity without
       moving the display would put two intrinsics under ONE shape and two identities, and keyname_record's
       assert is exactly the thing that catches it; so the two arms landed together or neither would have.
       ECMAScript's own §6.1.7.4 Well-Known Intrinsic Objects notation is the spelling (`%Array.prototype%`),
       which is a name a reader of the fork census can act on — the reason this is text and not a hash. */
    if (!tok) {
        char *nm = intrinsic_name(ctx, v);
        if (nm) { r = shapef("%s", nm); free(nm); return r; }
        return shapef("?");
    }
    switch (kind) {
    case CONCOLIC_LIT_STRING: r = shape_quote(tok); break;
    /* §6.1.6.2 The BigInt Type spells 5n as "5", which is the Number's spelling — the same collision one type
       over, and the identity separates them by kind exactly as it does the String. */
    case CONCOLIC_LIT_BIGINT: r = shapef("%sn", tok); break;
    case CONCOLIC_LIT_NUMBER:
    case CONCOLIC_LIT_BOOL:
    case CONCOLIC_LIT_NULL:
    case CONCOLIC_LIT_UNDEFINED:
        /* §7.1.19 ToString of each of these IS the literal a page writes for it, and no two of the four spell
           one token, so the token alone already separates what the identity separates. */
        r = shapef("%s", tok);
        break;
    case CONCOLIC_LIT_NONE:
        DFAIL("a concrete operand spelled a token under the kind operand_kind reserves for values it refuses "
              "to spell — literal_tok answers NULL for exactly that kind, so a token arriving with it means "
              "the two classifications of one operand have disagreed");
        r = shapef("?");
        break;
    }
    free(tok);
    return r;
}

/* Mint a value DERIVED from an unknown one: `ident` is CONSUMED, `example` is CONSUMED, and the candidate
   substitution applies exactly as it does to a source read (see concolic_new). */
static JSValue concolic_derived(JSContext *ctx, const char *shape, const char *src, const char *root,
                                char *ident, JSValue example);
/* …and the same without the substitution, for a value that is NOT a source read: a comparison RESULT is a
   boolean the operator computed, so handing the attacker's payload back in its place would answer a predicate
   with a string. */
static JSValue concolic_alloc(JSContext *ctx, const char *shape, const char *src, const char *root,
                              char *ident, JSValue example);

/* THE PER-FLOW PATH CONSTRAINT. One map carrying every fact this flow has learned about the unknown input it
   read — which is the whole of what a DART/SAGE-lineage constraint is here, because concrete execution grounds
   all shared and interprocedural state and only the INPUT variables are ever symbolic. Two facts, because two
   things narrow a domain, and they are keyed by two different names for the good reason that they are facts
   about two different things: a PIN is about a SOURCE (which is what a later read of that source looks itself
   up by), a decided TRUTH is about a PREDICATE (decide.c composes its key from the tested value's identity,
   the operator and both operands). One entry holds whichever of the two its key names:
     val   — an EQ gate PINNED the source to a concrete value on its true arm, so later reads compute the REAL
              @H value (`/api/admin`, never `/api/{state}.role`). CONCRETIZE-ON-PIN.
     valkind — …AND WHAT THOSE BYTES SPELL, which is not decoration on `val` but the other half of it. The
              spelling is §7.1.19 ToString of the operand the page wrote, so on its own it cannot tell the
              value `undefined` from the nine-character string; the read-back mints the REAL value from the
              pair (pin_mint), and a store that could hold a spelling without its kind would be a store whose
              own reader has to guess. Written and copied wherever `val` is, and never separately.
     truth — a PREDICATE over this source was already decided in this flow. `if (cfg.admin)` is not an equality
              and pins nothing, yet taking its true arm still says the value is truthy FOR THIS FLOW — and a
              bundle branches on the same flag over and over. Without this every one of those branches forked
              again, so N tests of one flag cost 2^N flows and the frontier is exponential in a quantity that
              carries no information: the sibling arms are CONTRADICTED, they explore nothing, and each still
              drags a COW delta. Deciding them is feasible-refinement, the thing that makes forced multi-path
              execution tractable, and it is sound-only — it prunes a branch on the SAME predicate the flow has
              already fixed, never one whose domain still permits both outcomes.
     pinned_root — SOME value ROOTED at this key was DEMANDED by this flow, which is a fact about the SOURCE
              and not about the value. `event.origin`, `event.origin.toLowerCase()` and `String(event.origin)`
              are three identities with one root, and a demand made of any of them is one demand on the
              attacker's principal. It is a separate field and not `val` because it is a different claim:
              writing the lowercased token under `message.origin` would make a later read of `event.origin`
              ITSELF answer it, which is a fabricated value in exactly the direction §Solver-half forbids.
              AND THE TWO ARE SEPARATELY DECIDED, which is the sharp end of that sentence rather than a
              qualification of it. `val` is a DETERMINATION and this is a DEMAND, and a §7.2.13 IsLooselyEqual
              ( x, y ) gate makes the second without the first: its holding arm bounds the operand to a SET, so
              there is no value to write, while the flow has still required the principal to satisfy it. See
              concolic_pin.
     excl  — the tokens an equality gate over this HOLE proved it is NOT, on the arm `val` does not cover.
              Forced multi-path runs both arms of every `x === "admin"`, so a pin and an exclusion are minted
              at the same rate; without this the second of the two flows knew nothing it could report about
              its own parameter. It is keyed by the HOLE KEY rather than by a source's `src` — the two differ
              for every derived value (see concolic_hole_key) and the emission has only the hole.
     pred  — the CALL PREDICATES this flow branched on over this HOLE, each as the page's own method name, the
              arguments it passed and the ARM this flow took. It is the third of the three ways a gate narrows
              a domain and shares a record with neither of the others for the reason they do not share one
              with each other: an equality determines a value, an ordering an interval, and a call neither.
              Within one flow they CONJOIN and accumulate, like the interval's two sides and unlike anything
              across flows, and an exact repeat adds nothing.
     bnd   — the INTERVAL an ordering gate over this HOLE narrowed it to, which is the exclusion's twin over an
              ORDERED domain and the second-most-frequent gate class in real minified bundles. It is a pointer
              rather than six inline fields because an entry either holds a domain over an ordered value or it
              does not, and paying six words on every pin and every branch outcome would grow the chain the
              cold tier measures for a fact most entries never carry.
              WITHIN ONE FLOW THE TWO SIDES CONJOIN. `if (x > 5 && x < 100)` is TWO observations of one
              parameter and a representation holding only one of them is a wrong report by the same rule that
              made `excl` necessary — so a lower and an upper bound are separate fields, each narrowed
              (max for the lower, min for the upper) as the flow observes more gates, and a tie between an
              inclusive and an exclusive bound at the same number keeps the EXCLUSIVE one, which is the
              tighter fact.
     leq   — the LOOSE EQUALITIES that HELD over this HOLE, each as the operand the page wrote and what that
              operand SPELLS. It is the equality's third arm and shares a record with neither the pin nor the
              exclusion, for the reason `pred` shares one with neither of those either: §7.2.14's holding arm
              DETERMINES a value and is `val`, §7.2.13's holding arm determines none and bounds the operand to
              a set §7.2.13 alone can read, and the failing arm of both is `excl`. It is keyed by the HOLE and
              `val` by the `src`, which is not an inconsistency to unify — a pin is what a later READ
              concretizes through and a domain is what the EMISSION looks up. Within one flow they CONJOIN and
              accumulate, and an exact repeat adds nothing.
   All seven are per-flow and travel together, which is why they are ONE entry rather than seven maps that a
   fork, a suspend and a resume would each have to remember to carry. */
/* One side of an interval, and the SPELLING beside the number. The number is what a merge orders two bounds
   by; the text is the page's own §6.1.6.1.20 Number::toString of the literal it wrote, kept so nothing
   downstream re-spells a double and reports a bound the source file does not contain. */
typedef struct { double num; char *txt; signed char present; signed char inclusive; } BoundSide;
typedef struct { BoundSide lo, hi; } Bound;
typedef struct { char *key; char *val; signed char valkind; char **excl; int nexcl; Bound *bnd;
                 ConcolicPred *pred; int npred; ConcolicLooseEq *leq; int nleq;
                 signed char truth; signed char pinned_root; signed char ex_contra; } Cons;

static void bound_side_free(BoundSide *s) { free(s->txt); s->txt = NULL; s->present = 0; }
/* THE ONE COPY OF ONE SIDE, because a bound is copied at the fork's copy-up and nowhere else, and a field
   added to BoundSide must be copied there or a sibling silently inherits half a fact. */
static void bound_side_copy(BoundSide *d, const BoundSide *s) {
    DCHECK(!s->present == !s->txt,
           "a bound side's presence and its spelling disagree — the two are written by one line, and a side "
           "that is present with no text is a bound the emission would read as a number it cannot print");
    *d = *s;
    d->txt = NULL;
    if (s->present) {
        d->txt = strdup(s->txt);
        CHECK(d->txt, "concolic: OOM copying an inherited bound's spelling");
    }
}

/* ONE FREE FOR ONE ENTRY, because there are now two places that dispose of an array of them (the live head and
   a frozen segment) and a field added to the struct must not be freed in one of them only. */
static void cons_entry_free(Cons *e) {
    int i;
    free(e->key);
    free(e->val);
    for (i = 0; i < e->nexcl; i++) free(e->excl[i]);
    free(e->excl);
    if (e->bnd) { bound_side_free(&e->bnd->lo); bound_side_free(&e->bnd->hi); free(e->bnd); }
    for (i = 0; i < e->npred; i++) concolic_pred_release(&e->pred[i]);
    free(e->pred);
    for (i = 0; i < e->nleq; i++) concolic_looseeq_release(&e->leq[i]);
    free(e->leq);
}
/* …and one measurement, for the same reason: the freeze's byte census reads exactly the fields the free above
   disposes of, so a field that grows the entry is counted where it is counted for every other. */
static long cons_entry_bytes(const Cons *e) {
    long n = (long)strlen(e->key) + 1 + (e->val ? (long)strlen(e->val) + 1 : 0);
    int i;
    n += (long)e->nexcl * (long)sizeof(char *);
    for (i = 0; i < e->nexcl; i++) n += (long)strlen(e->excl[i]) + 1;
    if (e->bnd) {
        n += (long)sizeof(Bound);
        if (e->bnd->lo.txt) n += (long)strlen(e->bnd->lo.txt) + 1;
        if (e->bnd->hi.txt) n += (long)strlen(e->bnd->hi.txt) + 1;
    }
    n += (long)e->npred * (long)sizeof(ConcolicPred);
    for (i = 0; i < e->npred; i++) {
        int k;
        n += (long)strlen(e->pred[i].method) + 1;
        n += (long)e->pred[i].nargs * (long)sizeof(char *);
        for (k = 0; k < e->pred[i].nargs; k++) n += (long)strlen(e->pred[i].args[k]) + 1;
    }
    n += (long)e->nleq * (long)sizeof(ConcolicLooseEq);
    for (i = 0; i < e->nleq; i++) n += (long)strlen(e->leq[i].tok) + 1;
    return n;
}

/* THE CONSTRAINT IS A MUTABLE HEAD OVER IMMUTABLE, REFCOUNTED, STRUCTURALLY-SHARED SEGMENTS — the third
   instance of the one primitive cow.c's `CowSeg` and dom_cow.c's `DomSeg` already are, and it is here for the
   same reason and in the same shape.
   IT USED TO BE A WHOLE-MAP DEEP COPY, PER FORK: one malloc for the array and two strdups per entry, taken by
   every sibling, so a flow that forks n times over a map it is also growing allocated O(n^2) bytes and held
   every one of them until the last sibling finished. THE SHAPE THAT REACHED IT is an unknown-length walk —
   `Array.from(state.items)` asks step_length_unknown one outcome fork per POSITION and each position adds one
   entry — and the run died in this file's own CHECK with the wasm heap gone, at the FIRST script.
   A frozen segment is never written again, so two flows that reach the same segment agree about everything
   from there down: a fork hands the sibling the parent's whole knowledge by taking ONE reference on the chain
   (O(1)), and what each flow LEARNS afterwards lives in its own head above it. That is the copy-on-write the
   deep copy was imitating, and it makes a fork cost what the arm has learned rather than what its whole history
   knows.
   A WRITE TO A KEY THE CHAIN ALREADY HOLDS COPIES THAT ONE ENTRY UP INTO THE HEAD (`cons_entry`), which is what
   keeps the shared segments immutable while a flow may still narrow a fact it inherited — never a write through
   into a segment a sibling is reading. */
/* `bytes` is what this segment costs, computed ONCE at the freeze and carried, because the strings it holds
   are strdup'd and re-walking them at every census would make a measurement O(chain). The cold tier reads it:
   a frozen segment is SHARED by every flow forked below it, so it is counted once for the whole frontier and
   never once per flow. */
typedef struct ConsSeg { Cons *e; int n; int *hash; int hash_cap; struct ConsSeg *base; int refcount; long bytes; } ConsSeg;
/* THE FROZEN CONSTRAINT CHAIN'S OWN CENSUS — the twin of cow.c's and dom_cow.c's, counted at the two points a
   segment's lifetime begins and ends so the pair cannot drift from the thing it counts. */
static long g_cons_seg_live, g_cons_seg_entries_live, g_cons_seg_bytes_live;
void concolic_chain_stats(long *segs, long *entries, long *bytes) {
    if (segs) *segs = g_cons_seg_live;
    if (entries) *entries = g_cons_seg_entries_live;
    if (bytes) *bytes = g_cons_seg_bytes_live;
}

static Cons    *g_pins = NULL;  static int g_pins_n = 0, g_pins_cap = 0;   /* the running flow's HEAD */
static int     *g_pins_hash = NULL; static int g_pins_hash_cap = 0;        /* …and its index (key -> idx+1) */
static ConsSeg *g_pins_base = NULL;                                        /* the frozen chain under it */
/* HAS ANY FLOW IN THIS AGENT EVER CONTRADICTED AN EXAMPLE — a one-directional LATCH, never cleared while the
   agent lives, and it is a performance fast path that cannot become a correctness one.
   WHY IT IS SAFE IN THE ONLY DIRECTION THAT MATTERS: it can only cause the lookup below to run when it need
   not, never to be skipped when it is needed. A per-flow answer would be the tempting version and is the
   dangerous one — it would have to be inherited at every fork, carried across a freeze and rebuilt by the cold
   tier, and any one of those forgotten is a flow silently answering with an example it proved wrong. This
   asks a question nothing has to maintain: has the mechanism fired at all, ever. On a document with no
   example-carrying gate the whole of it costs one predicate.
   THE COST IT AVOIDS IS REAL AND IS THE CHAIN'S. cons_lookup probes the head and then EVERY frozen segment
   under it, so a flow that has forked a thousand times pays a thousand hash probes — which `pin_of` already
   pays at every source-member read, and which this would otherwise add to every example read in the engine
   (each `+` over an unknown asks for two). */
static int g_ex_contra_any;

static uint32_t cons_hash(const char *k) {   /* FNV-1a over the constraint key */
    uint32_t h = 2166136261u;
    while (*k) { h ^= (unsigned char)*k++; h *= 16777619u; }
    return h;
}
/* THE ONE PROBE, over the head's index or a segment's — a segment's index IS the head's, handed over unchanged
   at the freeze, so there is one implementation and the two cannot drift. Returns the entry index or -1. */
static int cons_index_find(const Cons *e, const int *hash, int cap, const char *key) {
    uint32_t m, h;
    if (!hash) return -1;
    m = (uint32_t)cap - 1; h = cons_hash(key) & m;
    while (hash[h]) {
        if (!strcmp(e[hash[h] - 1].key, key)) return hash[h] - 1;
        h = (h + 1) & m;
    }
    return -1;
}
static void cons_hash_put(int idx) {   /* insert head entry idx; caller guarantees room */
    uint32_t m = (uint32_t)g_pins_hash_cap - 1, h = cons_hash(g_pins[idx].key) & m;
    while (g_pins_hash[h]) h = (h + 1) & m;
    g_pins_hash[h] = idx + 1;
}
static void cons_hash_rebuild(void) {   /* size to >= 2*n (power of two), re-insert every head entry */
    int i, nc = 16;
    int *nh;
    /* SIZED IN A LOCAL, PUBLISHED AFTER — the allocation can sell a flow (solver/reclaim.h), and a `cap`
       advertising the new size over the old table is an out-of-bounds read for anything that looks a pin up
       while the sale runs. */
    while (nc < g_pins_n * 2) nc *= 2;
    nh = reclaim_realloc(g_pins_hash, (size_t)nc * sizeof(int));
    CHECK(nh, "concolic: OOM path-constraint index");
    g_pins_hash = nh; g_pins_hash_cap = nc;
    memset(g_pins_hash, 0, (size_t)g_pins_hash_cap * sizeof(int));
    for (i = 0; i < g_pins_n; i++) cons_hash_put(i);
}
/* THE WHOLE CONSTRAINT THIS FLOW CAN SEE, nearest-first: the head, then each frozen segment from the newest
   down. Nearest-first is the copy-on-write rule read the other way round — a head entry SHADOWS the segment
   entry it was copied up from, so a fact this arm has narrowed is the one that answers. */
static const Cons *cons_lookup(const char *key) {
    ConsSeg *s;
    int i = cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key);
    if (i >= 0) return &g_pins[i];
    for (s = g_pins_base; s; s = s->base) {
        i = cons_index_find(s->e, s->hash, s->hash_cap, key);
        if (i >= 0) return &s->e[i];
    }
    return NULL;
}
/* THE WRITABLE entry for `key`: the head's, copying the chain's fact up into the head the first time this flow
   narrows one it inherited. */
static Cons *cons_entry(const char *key) {
    const Cons *below;
    int i = cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key);
    if (i >= 0) return &g_pins[i];
    below = cons_lookup(key);   /* the head misses, so this is the chain's entry or nothing */
    if (g_pins_n >= g_pins_cap) {
        int nc = g_pins_cap ? g_pins_cap * 2 : 8;
        Cons *np = reclaim_realloc(g_pins, (size_t)nc * sizeof(Cons));
        CHECK(np, "concolic: OOM path constraint");
        g_pins = np; g_pins_cap = nc;   /* published after the ask — see cons_hash_rebuild */
    }
    g_pins[g_pins_n].key = strdup(key); CHECK(g_pins[g_pins_n].key, "concolic: OOM constraint key");
    g_pins[g_pins_n].val = (below && below->val) ? strdup(below->val) : NULL;
    CHECK(!(below && below->val) || g_pins[g_pins_n].val, "concolic: OOM copying an inherited pin value");
    /* …AND ITS KIND WITH IT, on the same line and under the same condition, because the two are one fact. A
       copy-up that took the bytes and left the kind behind would hand the sibling a spelling the read-back
       reconstructs as CONCOLIC_LIT_NONE, which is the one value pin_mint refuses. */
    g_pins[g_pins_n].valkind = (below && below->val) ? below->valkind : (signed char)CONCOLIC_LIT_NONE;
    /* THE EXCLUSIONS COPY UP WITH THE REST OF THE ENTRY. They are a fact this flow proved, so a copy-up that
       dropped them would make the very same path report an unconstrained parameter the moment a context
       switch happened to fall between the gate and the request that carries the value. */
    g_pins[g_pins_n].excl = NULL;
    g_pins[g_pins_n].nexcl = 0;
    if (below && below->nexcl) {
        int k;
        g_pins[g_pins_n].excl = malloc((size_t)below->nexcl * sizeof(char *));
        CHECK(g_pins[g_pins_n].excl, "concolic: OOM copying an inherited exclusion set");
        for (k = 0; k < below->nexcl; k++) {
            g_pins[g_pins_n].excl[k] = strdup(below->excl[k]);
            CHECK(g_pins[g_pins_n].excl[k], "concolic: OOM copying an inherited excluded value");
        }
        g_pins[g_pins_n].nexcl = below->nexcl;
    }
    /* …AND SO DOES THE INTERVAL, for exactly the reason above. A flow that observed `x > 5`, was preempted,
       and resumed to build its request would otherwise report an unbounded parameter — the fact lost to a
       context switch rather than to anything the program did. */
    g_pins[g_pins_n].bnd = NULL;
    if (below && below->bnd) {
        Bound *nb = malloc(sizeof *nb);
        CHECK(nb, "concolic: OOM copying an inherited bound");
        bound_side_copy(&nb->lo, &below->bnd->lo);
        bound_side_copy(&nb->hi, &below->bnd->hi);
        g_pins[g_pins_n].bnd = nb;
    }
    /* …AND THE CALL PREDICATES, for exactly the reason the two above copy up. A flow that observed
       `path.startsWith("/api")`, was preempted, and resumed to build its request would otherwise report a
       parameter nothing had ever tested — the fact lost to a context switch rather than to anything the
       program did, which is the defect this whole entry is a list of. */
    g_pins[g_pins_n].pred = NULL;
    g_pins[g_pins_n].npred = 0;
    if (below && below->npred) {
        int k;
        g_pins[g_pins_n].pred = malloc((size_t)below->npred * sizeof(ConcolicPred));
        CHECK(g_pins[g_pins_n].pred, "concolic: OOM copying an inherited call-predicate set");
        for (k = 0; k < below->npred; k++)
            concolic_pred_copy(&g_pins[g_pins_n].pred[k], &below->pred[k]);
        g_pins[g_pins_n].npred = below->npred;
    }
    /* …AND THE LOOSE EQUALITIES THAT HELD, for exactly the reason the three above copy up. A flow that
       observed `x == 0`, was preempted, and resumed to build its request would otherwise report a parameter
       nothing had ever tested — the fact lost to a context switch rather than to anything the program did,
       which is the defect this whole entry is a list of. */
    g_pins[g_pins_n].leq = NULL;
    g_pins[g_pins_n].nleq = 0;
    if (below && below->nleq) {
        int k;
        g_pins[g_pins_n].leq = malloc((size_t)below->nleq * sizeof(ConcolicLooseEq));
        CHECK(g_pins[g_pins_n].leq, "concolic: OOM copying an inherited loose-equality set");
        for (k = 0; k < below->nleq; k++)
            concolic_looseeq_copy(&g_pins[g_pins_n].leq[k], &below->leq[k]);
        g_pins[g_pins_n].nleq = below->nleq;
    }
    g_pins[g_pins_n].truth = below ? below->truth : -1;
    /* INHERITED WITH THE REST OF THE ENTRY. A principal demand this flow made before its last freeze is still
       this flow's demand, and a copy-up that dropped it would make the very same path answer "unpinned" the
       moment a context switch happened to fall between the pin and the sink. */
    g_pins[g_pins_n].pinned_root = below ? below->pinned_root : 0;
    /* …AND THE CONTRADICTED EXAMPLE, for the reason every field above it copies up. It is a fact this flow
       PROVED — it took an arm the value's own example says a real session does not take — so a copy-up that
       dropped it would make the very same path start answering with that example again the moment a context
       switch happened to fall between the gate and the request that carries the value, which is the defect
       this whole entry is a list of. */
    g_pins[g_pins_n].ex_contra = below ? below->ex_contra : 0;
    g_pins_n++;
    if (!g_pins_hash || g_pins_hash_cap < g_pins_n * 2) cons_hash_rebuild();
    else cons_hash_put(g_pins_n - 1);
    DCHECK(cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key) == g_pins_n - 1,
           "a constraint entry is not findable through the index that was just given it — a later read of the "
           "same fact would fork a branch this flow has already decided");
    return &g_pins[g_pins_n - 1];
}
/* THE PIN, AND THE MARK BESIDE IT — see concolic.h. The two writes are one act and are made together here
   because nothing downstream can recover the second: by the time a value reaches a sink the identity that was
   pinned is long gone, and only the ROOT survives every derivation.
   THE ROOT IS WRITTEN EVEN WHEN IT EQUALS THE IDENTITY, which is the common case (`event.origin === X`). Two
   entries then hold one key and the second write is a no-op on the first, which is what `cons_entry` already
   does for every repeated narrowing — writing the mark only for a DERIVED pin would make the direct spelling
   the one case the principal rule missed, and it is the spelling every real bundle writes. */
void concolic_pin(const char *src, const char *root, ConcolicLit kind, const char *val, JSConcolicEqOp algo) {
    Cons *c;
    /* THE TWO WRITES ARE ONE ACT AND TWO DECISIONS, AND THE ALGORITHM SEPARATES THEM. See concolic.h for the
       step-by-step reading of §7.2.13 IsLooselyEqual ( x, y ); in one line, `===` holding DETERMINES the
       operand (§7.2.14 IsStrictlyEqual ( x, y ) step 1 is "If SameType(x, y) is false, return false") while
       `==` holding leaves a SET for every kind this store can spell, so a loose pin picks a WITNESS and
       concretize-on-pin then decides every later branch over the source from that choice.
       THE DEMAND SURVIVES BOTH, which is why this is not a `return` at the top of the function. A loose
       equality against a principal is still a demand for a particular value of it, and `e.origin == TRUSTED`
       is in fact unforgeable cross-origin for exactly the reason §7.2.13 step 1 gives — two Strings go
       straight to §7.2.14. Refusing the mark with the value would un-suppress every loose origin check in
       every bundle, which is a FALSE PoC, and §Attacker-sources forbids that in the direction that emits
       one. */
    DCHECK(algo == JS_CONCOLIC_EQ_LOOSE || algo == JS_CONCOLIC_EQ_STRICT,
           "an arm recorded an equality observation naming an algorithm quickjs.h does not declare — §7.2.13 "
           "and §7.2.14 differ on exactly whether the holding arm determined a value, so an unnamed one has "
           "no answer to the only question this function asks of it");
    /* THE VOCABULARY IS ASSERTED AT THE MINT, WHICH IS WHERE THE STORE CAN STILL REFUSE. A reader that met a
       spelling with no kind could only guess, and the guess this store used to make — "everything is a
       string" — is the defect: `x === undefined` recorded nine characters and every later read of that source
       computed them. The kind travels from operand_kind through pred_new and concolic_cmp precisely so this
       line has a fact to check rather than a default to apply. */
    DCHECK(kind != CONCOLIC_LIT_NONE,
           "an arm pinned a source to an operand this engine cannot spell — literal_tok answers NULL for an "
           "Object and a Symbol and the hook mints no token for one, so a pin arriving here with no kind is a "
           "token composed somewhere that does not go through that classification");
    /* AND THE KINDS IT MAY NOT HOLD LEAVE NO ENTRY AT ALL — the store accepts exactly what pin_mint can hand
       back as a value, which is what makes that function's own DFAILF unreachable rather than merely
       unexpected. The list is spelled here and NOT behind the DCHECK above, because a DCHECK is compiled out
       in release and a guard that vanishes there would let a release build record a spelling whose kind the
       read-back answers `undefined` for — the impossible state made impossible in both builds instead of
       asserted in one. BIGINT is concolic.h's named residual; NONE is the dev abort above, still refused here
       so release cannot proceed past it either.
       THE REFUSAL IS BEFORE THE ROOT MARK AS WELL AS BEFORE THE VALUE, AND THAT IS ABOUT THIS REFUSAL RATHER
       THAN ABOUT THE TWO WRITES IN GENERAL — the loose-equality gate below makes the mark WITHOUT the value,
       so the reason has to be the narrow one it always was. Here the store cannot express the operand AT ALL:
       there is no spelling it can hold, so recording the mark alone would say a flow demanded a particular
       principal with nothing anywhere able to say which, and nothing could later supply it. The loose case is
       the opposite — the token is expressible and the demand is observed; only the DETERMINATION is missing. */
    if (kind == CONCOLIC_LIT_BIGINT || kind == CONCOLIC_LIT_NONE) return;
    /* THE DETERMINATION, WHICH ONLY §7.2.14 MAKES. §7.2.13 IsLooselyEqual ( x, y )'s holding arm leaves the
       operand in a SET — `x == undefined` admits null by step 2, a numeric-looking String by step 6, a Boolean
       by step 9 and an Object by step 12, which runs the page's own ToPrimitive — so writing one member of it
       here is choosing a WITNESS, and concretize-on-pin makes every later branch over the source decide from
       the choice instead of forking. §@H draws the line at whether a VALUE was determined, never whether a
       constraint was, and this is the value half of it.
       THE NARROWING IS NOT LOST AND IS NOT THIS FUNCTION'S TO HOLD. A pin is keyed by `src`, because that is
       what a later READ concretizes through; the loose arm's fact is a DOMAIN, and a domain is keyed by the
       HOLE the report prints — two names for two consumers (concolic_cmp_subject states the pair). So
       decide.c files it through `concolic_looseeq` on the same arm this line refuses, and what an unpinned
       source still does here is FORK again at its next gate, which is sound and is what this engine did
       before any pin existed. */
    /* A CONCOLIC CARRIES A PROVENANCE AND A ROOT TOGETHER (concolic_alloc asserts it), so a pin taken off one
       and missing the other is a value minted somewhere that does not go through that mint — and what it costs
       is silent: the principal rule below would answer "unpinned" for a flow that demanded an origin, and the
       PoC it then emits is one no cross-document attacker can deliver. ASKED ABOVE THE SPLIT, because the
       demand is made under BOTH algorithms and the root is what names whose bytes were demanded. */
    DCHECK(root != NULL, "an equality pinned an attacker value that carries no delivery ROOT — the root is "
                         "what says WHOSE bytes were demanded, and §Attacker-sources' unforgeable-origin rule "
                         "is decided by exactly that");
    if (algo == JS_CONCOLIC_EQ_STRICT) {
        c = cons_entry(src);
        free(c->val); c->val = strdup(val); CHECK(c->val, "concolic: OOM pin value");
        c->valkind = (signed char)kind;
        /* `c` IS DEAD FROM HERE. cons_entry may grow the head, and the growth is a realloc — so the second
           entry is taken only after the first has been written, and nothing below may reach back through it. */
    }
    if (root) cons_entry(root)->pinned_root = 1;
}
/* THE HOLE KEY — see concolic.h. One speller, because it is read at two ends that would otherwise normalise
   the same hole differently and never meet. */
char *concolic_hole_key(const char *shape) {
    char *r;
    size_t i, n = 0;

    if (!shape || !strchr(shape, '{')) return NULL;
    r = malloc(strlen(shape) + 1);
    CHECK(r, "concolic: OOM naming the hole a domain is a fact about");
    for (i = 0; shape[i]; i++) if (shape[i] != '{' && shape[i] != '}') r[n++] = shape[i];
    r[n] = 0;
    if (!n) { free(r); return NULL; }   /* `{}` — the value this engine cannot name; it gets no domain either */
    return r;
}

/* THE NEGATIVE HALF OF THE EQUALITY OBSERVATION — see concolic.h. The token is the concrete side of a
   predicate the PAGE wrote, so nothing here is chosen: this records that the flow took the arm on which the
   value is not that token, which is as much an observation as the pin on the other arm is. */
/* THE CONSTRAINT KEY AN EXCLUSION SET LIVES UNDER. It is COMPOSED and not the raw hole, because the one map
   already holds two other kinds of key — a pin's raw `src` and a branch's composed `branch` identity — and a
   raw shape sharing a namespace with a raw source name is a collision waiting for the first page whose member
   path spells a declared source. Composition is this file's own encoding (length-prefixed fields under a tag),
   so an `excl` key can never equal a `branch` key or a bare `src`. Caller frees. */
static char *excl_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("excl", f, 1);
}

void concolic_exclude(const char *hole, const char *tok) {
    Cons *c;
    char **a;
    char *key;
    int i;

    DCHECK(hole && *hole,
           "an equality's false arm was recorded against no HOLE — the subject is what the emission looks the "
           "domain up by, so a nameless one is a constraint stored where nothing can read it and a parameter "
           "that renders as unconstrained while this flow has proved otherwise");
    DCHECK(tok != NULL,
           "an equality's false arm named no TOKEN to exclude — the token is the concrete side the page's own "
           "predicate wrote, and without it there is no fact, only the knowledge that some fact existed");
    key = excl_key(hole);
    CHECK(key, "concolic: the exclusion key could not be composed — a hole is a real string, so the only way "
               "this fails is allocation, and a lost constraint reports an unconstrained parameter");
    c = cons_entry(key);
    free(key);
    for (i = 0; i < c->nexcl; i++) if (!strcmp(c->excl[i], tok)) return;   /* the same gate, tested again */
    a = realloc(c->excl, (size_t)(c->nexcl + 1) * sizeof(char *));
    CHECK(a, "concolic: OOM recording a value this flow proved its input is not");
    c->excl = a;
    c->excl[c->nexcl] = strdup(tok);
    CHECK(c->excl[c->nexcl], "concolic: OOM copying a value this flow proved its input is not");
    c->nexcl++;
}

const char *const *concolic_excluded(const char *hole, int *n) {
    const Cons *c = NULL;

    DCHECK(n != NULL, "the exclusion set was asked for with nowhere to put its SIZE — a borrowed array with no "
                      "count is an array the caller has to guess the end of");
    if (hole) {
        char *key = excl_key(hole);
        CHECK(key, "concolic: the exclusion key could not be composed at the read");
        c = cons_lookup(key);
        free(key);
    }
    if (!c || !c->nexcl) { *n = 0; return NULL; }
    *n = c->nexcl;
    return (const char *const *)c->excl;
}

/* THE CONSTRAINT KEY A LOOSE-EQUALITY SET LIVES UNDER — its OWN tag, for the reason excl_key and bnd_key state
   of theirs: `!= "admin"`, `> 5`, `startsWith("/api")` and `== 0` are four claims about one hole, and filing
   any two of them together would make a consumer that reads one see another's shape. It is a separate tag from
   `excl` in particular because those two are the SAME operand under the SAME operator on OPPOSITE arms — the
   one pair a shared key would silently merge into a claim that a value both is and is not the token. Caller
   frees. */
static char *leq_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("looseeq", f, 1);
}

/* THE HOLDING ARM OF A LOOSE EQUALITY — see concolic.h. Nothing here is chosen: the operand is the concrete
   side the PAGE wrote and the arm is the arm this run took, so this records that the page's own `==` against
   that operand answered true of this value. §@H's line is whether a VALUE was determined, and none is. */
void concolic_looseeq(const char *hole, ConcolicLit kind, const char *tok) {
    Cons *c;
    ConcolicLooseEq *a;
    char *key;
    int i;

    DCHECK(hole && *hole,
           "a loose equality's holding arm was recorded against no HOLE — the subject is what the emission "
           "looks the domain up by, so a nameless one is a constraint stored where nothing can read it and a "
           "parameter that renders as unconstrained while this flow has proved otherwise");
    DCHECK(tok != NULL,
           "a loose equality's holding arm named no OPERAND — the operand is the concrete side the page's own "
           "predicate wrote, and without it there is no fact, only the knowledge that some fact existed");
    /* THE OPERAND AND ITS KIND ARE ONE ARGUMENT IN TWO HALVES, asserted where they meet, exactly as
       concolic_pin asserts them: a spelling with no kind states `x == undefined` and `x == "undefined"` with
       identical bytes, and those are two gates whose holding sets under §7.2.13 IsLooselyEqual ( x, y ) have
       nothing in common — the first admits {undefined, null} by steps 2 and 3, the second neither of them.
       Unlike concolic_pin this does NOT refuse a BigInt: that refusal is about the read-back having to MINT a
       value again, and a row is only ever PRINTED. */
    DCHECK(kind != CONCOLIC_LIT_NONE,
           "a loose equality's holding arm was recorded for an operand this engine cannot spell — literal_tok "
           "answers NULL for an Object and a Symbol and the hook mints no token for one, so a row arriving "
           "here with no kind is a token composed somewhere that does not go through that classification");
    key = leq_key(hole);
    CHECK(key, "concolic: the loose-equality key could not be composed — a hole is a real string, so the only "
               "way this fails is allocation, and a lost constraint reports an unconstrained parameter");
    c = cons_entry(key);
    free(key);
    for (i = 0; i < c->nleq; i++)
        if (concolic_looseeq_same(&c->leq[i], kind, tok)) return;   /* the same gate, tested again */
    a = realloc(c->leq, (size_t)(c->nleq + 1) * sizeof(ConcolicLooseEq));
    CHECK(a, "concolic: OOM recording a loose equality this flow's own run proved of its input");
    c->leq = a;
    {   /* THE ONE DEEP COPY, asked of the same pair every other holder asks — see concolic_looseeq_copy. The
           row is assembled on the stack first so the copy has exactly one shape to take, which is what keeps a
           field added to ConcolicLooseEq from having a fourth place it could be forgotten. */
        ConcolicLooseEq in;
        in.tok = (char *)tok;
        in.kind = (signed char)kind;
        concolic_looseeq_copy(&c->leq[c->nleq], &in);
    }
    c->nleq++;
}

const ConcolicLooseEq *concolic_looseeq_read(const char *hole, int *n) {
    const Cons *c = NULL;

    DCHECK(n != NULL, "the loose-equality set was asked for with nowhere to put its SIZE — a borrowed array "
                      "with no count is an array the caller has to guess the end of");
    if (hole) {
        char *key = leq_key(hole);
        CHECK(key, "concolic: the loose-equality key could not be composed at the read");
        c = cons_lookup(key);
        free(key);
    }
    if (!c || !c->nleq) { *n = 0; return NULL; }
    *n = c->nleq;
    return c->leq;
}

/* THE CONSTRAINT KEY AN INTERVAL LIVES UNDER — composed under its own tag for the reason excl_key states, and
   a SEPARATE tag from that one because the two are different claims about one hole: `≠ "admin"` and `> 5` are
   both domain facts, and filing them together would make a consumer that reads one see the other's shape.
   Caller frees. */
static char *bnd_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("bound", f, 1);
}

/* ONE SIDE NARROWED. Within one flow the observations CONJOIN — `if (x > 5 && x < 100)` is two facts about one
   parameter, and each side keeps the TIGHTER of what it held and what just arrived. On a tie between an
   inclusive and an exclusive bound at the same number the exclusive one wins, because `x > 5` is strictly more
   than `x >= 5` says and a flow that observed both stands on both. */
static void bound_side_narrow(BoundSide *s, int is_lower, double num, int inclusive, const char *txt) {
    if (s->present) {
        int tighter = is_lower ? (num > s->num) : (num < s->num);
        if (!tighter) {
            if (num != s->num) return;                     /* the looser fact adds nothing */
            if (inclusive || !s->inclusive) return;        /* same number: only exclusive-over-inclusive wins */
        }
        bound_side_free(s);
    }
    s->num = num;
    s->inclusive = (signed char)(inclusive ? 1 : 0);
    s->txt = strdup(txt);
    CHECK(s->txt, "concolic: OOM copying the spelling of a bound a flow observed");
    s->present = 1;
}

void concolic_bound(const char *hole, RelOp rel, double num, const char *txt) {
    Cons *c;
    char *key;

    DCHECK(hole && *hole,
           "an ordering gate was recorded against no HOLE — the subject is what the emission looks a domain "
           "up by, so a nameless one is a constraint stored where nothing can read it and a parameter that "
           "renders as unbounded while this flow has proved otherwise");
    DCHECK(rel != REL_NONE,
           "an ordering gate was recorded with no RELATION — `x < 5` and `x > 5` are the two answers this "
           "field distinguishes, so a bound with neither is a number filed under a claim nobody made");
    DCHECK(txt && *txt,
           "an ordering gate was recorded with no SPELLING for its bound — the text is the page's own literal "
           "and the only thing a report may print, because re-spelling the double here would state a number "
           "the source file does not contain");
    key = bnd_key(hole);
    CHECK(key, "concolic: the bound key could not be composed — a hole is a real string, so the only way this "
               "fails is allocation, and a lost constraint reports an unbounded parameter");
    c = cons_entry(key);
    free(key);
    if (!c->bnd) {
        c->bnd = calloc(1, sizeof *c->bnd);
        CHECK(c->bnd, "concolic: OOM recording the interval a flow narrowed its input to");
    }
    if (rel == REL_GT || rel == REL_GE)
        bound_side_narrow(&c->bnd->lo, 1, num, rel == REL_GE, txt);
    else
        bound_side_narrow(&c->bnd->hi, 0, num, rel == REL_LE, txt);
}

int concolic_bound_read(const char *hole, ConcolicBound *out) {
    const Cons *c = NULL;

    DCHECK(out != NULL, "the interval was asked for with nowhere to put it — a bound returned only as a "
                        "yes/no is a constraint the caller cannot state");
    out->has_lo = out->has_hi = 0;
    out->lo_incl = out->hi_incl = 0;
    out->lo = out->hi = 0;
    out->lo_txt = out->hi_txt = NULL;
    if (hole) {
        char *key = bnd_key(hole);
        CHECK(key, "concolic: the bound key could not be composed at the read");
        c = cons_lookup(key);
        free(key);
    }
    if (!c || !c->bnd) return 0;
    if (c->bnd->lo.present) {
        out->has_lo = 1; out->lo = c->bnd->lo.num;
        out->lo_incl = c->bnd->lo.inclusive; out->lo_txt = c->bnd->lo.txt;
    }
    if (c->bnd->hi.present) {
        out->has_hi = 1; out->hi = c->bnd->hi.num;
        out->hi_incl = c->bnd->hi.inclusive; out->hi_txt = c->bnd->hi.txt;
    }
    DCHECK(out->has_lo || out->has_hi,
           "a hole carries a bound record with neither side present — concolic_bound allocates the record "
           "only as it writes a side, so an empty one is a narrowing that was allocated and then not made");
    return 1;
}

/* THE CONSTRAINT KEY A CALL-PREDICATE SET LIVES UNDER — its OWN tag, for the reason bnd_key states of its
   own: `≠ "admin"`, `> 5` and `startsWith("/api")` are three claims about one hole, and filing any two of
   them together would make a consumer that reads one see another's shape. Caller frees. */
static char *strp_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("strpred", f, 1);
}

int concolic_pred_same(const ConcolicPred *p, const char *method, const char *const *args, int nargs,
                       int holds) {
    int i;
    if (p->holds != (signed char)(holds ? 1 : 0) || p->nargs != nargs || strcmp(p->method, method)) return 0;
    for (i = 0; i < nargs; i++) if (strcmp(p->args[i], args[i])) return 0;
    return 1;
}

void concolic_strpred_file(const char *hole, const char *method, const char *const *args, int nargs,
                           int holds) {
    Cons *c;
    ConcolicPred *a;
    char *key;
    int i;

    DCHECK(hole && *hole,
           "a call predicate was recorded against no HOLE — the receiver is what the emission looks the "
           "domain up by, so a nameless one is a constraint stored where nothing can read it and a parameter "
           "that renders as untested while this flow has proved otherwise");
    DCHECK(method && *method,
           "a call predicate was recorded with no METHOD — the name is the whole of what the page's own test "
           "WAS, and a domain that cannot say which predicate held is a claim nobody made");
    DCHECK(nargs == 0 || args != NULL,
           "a call predicate was recorded with an argument COUNT and no arguments — the two are written by "
           "one line at the mint, so a count without a list is an argument this run cannot spell arriving "
           "as one it can");
    DCHECK(holds == 0 || holds == 1,
           "a call predicate was recorded for an arm that is neither taken nor not-taken — the arm IS the "
           "fact here (a page's own test answered true, or it answered false), so a third value is a "
           "decision seam that answered something else");
    key = strp_key(hole);
    CHECK(key, "concolic: the call-predicate key could not be composed — a hole is a real string, so the only "
               "way this fails is allocation, and a lost constraint reports an untested parameter");
    c = cons_entry(key);
    free(key);
    for (i = 0; i < c->npred; i++)
        if (concolic_pred_same(&c->pred[i], method, args, nargs, holds)) return;  /* the same gate, again */
    a = realloc(c->pred, (size_t)(c->npred + 1) * sizeof(ConcolicPred));
    CHECK(a, "concolic: OOM recording a predicate this flow's own run proved of its input");
    c->pred = a;
    {   /* THE ONE DEEP COPY, asked of the same pair every other holder asks — see concolic_pred_copy. The
           borrowed row is assembled on the stack first so the copy has exactly one shape to take, which is
           what keeps a field added to ConcolicPred from having a fourth place it could be forgotten. */
        ConcolicPred in;
        in.method = (char *)method;
        in.args = (char **)args;
        in.nargs = nargs;
        in.holds = (signed char)(holds ? 1 : 0);
        concolic_pred_copy(&c->pred[c->npred], &in);
    }
    c->npred++;
}

const ConcolicPred *concolic_strpred_read(const char *hole, int *n) {
    const Cons *c = NULL;

    DCHECK(n != NULL, "the call-predicate set was asked for with nowhere to put its SIZE — a borrowed array "
                      "with no count is an array the caller has to guess the end of");
    if (hole) {
        char *key = strp_key(hole);
        CHECK(key, "concolic: the call-predicate key could not be composed at the read");
        c = cons_lookup(key);
        free(key);
    }
    if (!c || !c->npred) { *n = 0; return NULL; }
    *n = c->npred;
    return c->pred;
}

void concolic_constrain_branch(const char *key, int truth) {
    cons_entry(key)->truth = (signed char)(truth ? 1 : 0);
}
int concolic_branch_decided(const char *key) {
    const Cons *c = cons_lookup(key);
    return c ? c->truth : -1;
}

/* Drop a chain reference: refcount--, free the segment's entries at zero, continue into its base. A loop, not
   recursion — the chain's depth is the fork depth, and an unknown-length walk makes that as deep as the walk is
   long; C stack cannot be parked. The twin of cow_seg_unref / dom_seg_unref. */
static void cons_seg_unref(ConsSeg *s) {
    while (s && --s->refcount <= 0) {
        ConsSeg *base = s->base;
        int i;
        g_cons_seg_live--; g_cons_seg_entries_live -= s->n; g_cons_seg_bytes_live -= s->bytes;
        for (i = 0; i < s->n; i++) cons_entry_free(&s->e[i]);
        free(s->e); free(s->hash); free(s);
        s = base;
    }
}

/* HAVE THE RUNNING FLOW'S OWN CANDIDATE BYTES ENTERED THE PROGRAM YET? — a per-flow fact, held here because
   this component is the only one that can answer it and because concolic_deliver is the only place it becomes
   true. See concolic.h for what it is a precondition OF; this is where it lives and how it travels.
   PER FLOW, LIKE THE PINS BESIDE IT, AND IT TRAVELS THE SAME WAY: the live value is the RUNNING flow's,
   concolic_pins_suspend copies it into that flow's blob (so a FORK's sibling starts from exactly what its
   parent had at the branch — §scheduler's fork-is-rank-neutral, which matters the moment a rung reads it),
   concolic_pins_resume installs a parked one, concolic_pins_blob_empty answers 0 for a flow that has never run
   in this session, and concolic_clear_pins is the fresh-flow reset. Four points, all in this file.
   IT DOES NOT CROSS THE COLD TIER, and that is not an omission — cold.c gives a resumed candidate an EMPTY pin
   blob because such a flow REPLAYS the document from the baseline (park_rec_cand parks a recipe, never a live
   frame chain), so it re-reaches its own source read and re-earns this. flow.h says the same thing about
   `cand_surv`/`cand_rung`: an observation of a re-execution belongs to the session that made it. */
static int g_cand_delivered;

/* THIS IS THE PER-FLOW CONCOLIC STATE'S CLEAR AND NOT ONLY THE PINS' — which is a statement about its CALLERS
   rather than about its name. decide.c's decide_enter calls it to give a FRESH flow an empty concolic state,
   flow.c's teardown calls it when there is no flow left to own one, and concolic_pins_resume calls it before
   installing a parked one. Every one of those means "nothing this component holds is this flow's yet", so a
   per-flow fact of this component that is NOT reset here is a fact one flow reads off another — and the
   delivery bit below is the second such fact this file holds. The name says `pins` because flow.h spells the
   slot `pin_blob` and three of the four callers are outside this file; see concolic_candidate_delivered. */
void concolic_clear_pins(void) {
    int i;
    for (i = 0; i < g_pins_n; i++) cons_entry_free(&g_pins[i]);
    free(g_pins); g_pins = NULL; g_pins_n = g_pins_cap = 0;
    free(g_pins_hash); g_pins_hash = NULL; g_pins_hash_cap = 0;
    cons_seg_unref(g_pins_base); g_pins_base = NULL;
    g_cand_delivered = 0;
}

/* Per-flow constraint state is swappable so interleaved flows keep their OWN narrowing: suspend FREEZES the
   live head onto the chain and hands back a reference to it, resume installs a parked chain as the live one.
   A blob is one flow's constraint parked while another runs — and the blob a FORK takes is the sibling's whole
   starting knowledge, which is the same object either way and is why one function serves both. */
/* AND IT CARRIES EVERY PER-FLOW FACT THIS COMPONENT HOLDS, WHICH IS TWO AND USED TO BE ONE. `delivered` rides
   here for the same reason `seg` does: it is the running flow's and nobody else's, so a switch that left it
   behind would hand the incoming flow the outgoing one's answer. The struct's NAME is the slot's name in
   flow.h (`pin_blob`), which is outside this file; the CONTENT is this component's. */
typedef struct { ConsSeg *seg; int delivered; } PinBlob;
void *concolic_pins_suspend(void) {
    PinBlob *b = reclaim_malloc(sizeof *b);
    CHECK(b, "concolic: the path constraint could not be parked — the frontier never drops a work item, and a "
             "flow whose constraint is lost would re-fork every branch it has already decided");
    /* TAKEN AND NOT MOVED — the live value stays where it is, because this function serves a FORK as well as a
       park: the sibling starts from what the parent had at the branch AND the parent goes on holding it. */
    b->delivered = g_cand_delivered;
    if (g_pins_n == 0) {
        /* NOTHING LEARNED SINCE THE LAST FREEZE, so there is nothing to freeze: the blob is one more reference
           on the chain the flow already stands on. Without this a park/resume pair with no writes between them
           would push an empty segment per switch and the chain's depth would count SWITCHES rather than forks. */
        b->seg = g_pins_base;
        if (b->seg) b->seg->refcount++;
        return b;
    }
    DCHECK(g_pins_hash != NULL, "a non-empty constraint head is being frozen with no index — every read of the "
                                "frozen segment would miss and the fact would be re-forked");
    {
        ConsSeg *s = reclaim_malloc(sizeof *s);
        CHECK(s, "concolic: OOM freezing the path constraint into a shared segment");
        s->e = g_pins; s->n = g_pins_n; s->hash = g_pins_hash; s->hash_cap = g_pins_hash_cap;
        s->base = g_pins_base; s->refcount = 2;   /* the running flow + this blob */
        {   /* what this segment costs, measured where it is frozen — see the struct */
            int k;
            s->bytes = (long)sizeof *s + (long)s->n * (long)sizeof(Cons)
                     + (long)s->hash_cap * (long)sizeof(int);
            for (k = 0; k < s->n; k++)
                s->bytes += cons_entry_bytes(&s->e[k]);
            g_cons_seg_live++; g_cons_seg_entries_live += s->n; g_cons_seg_bytes_live += s->bytes;
        }
        g_pins = NULL; g_pins_n = g_pins_cap = 0;
        g_pins_hash = NULL; g_pins_hash_cap = 0;
        g_pins_base = s;                          /* the running flow continues on top of what it just froze */
        b->seg = s;
    }
    return b;
}
void concolic_pins_resume(void *blob) {
    PinBlob *b = blob;
    DCHECK(b != NULL, "a flow was resumed with no parked path constraint — it would re-fork every branch it "
                      "had already decided, on a decision vector that replays the old answers");
    concolic_clear_pins();                 /* the live head and chain belong to the flow that just parked */
    g_pins_base = b->seg;
    if (g_pins_base) g_pins_base->refcount++;   /* the blob keeps its own reference until it is freed */
    /* AFTER THE CLEAR, WHICH IS WHERE THE ORDER IS LOAD-BEARING: the clear above is what resets every per-flow
       fact this component holds, so a restore written before it would be erased by it. */
    g_cand_delivered = b->delivered;
}
/* A CONSTRAINT THAT HAS LEARNED NOTHING YET, for a flow that RESUMES without ever having run in this session —
   the cold tier's. Such a flow is not fresh (it stands on a recorded decision chain, so the scheduler resumes it
   rather than entering it) and it holds no knowledge (its facts were narrowed from values this session's heap
   does not contain, and it re-derives every one of them as it replays the gates that produced them). The resume
   path asserts that a blob exists, and it is right to: a flow resumed with none would re-fork every branch it
   had already decided. This is the honest way to satisfy it — an empty chain, not a NULL the assert is taught
   to tolerate. */
void *concolic_pins_blob_empty(void) {
    PinBlob *b = reclaim_malloc(sizeof *b);
    CHECK(b, "concolic: a resumed flow's empty path constraint could not be allocated");
    b->seg = NULL;
    /* AND IT HAS DELIVERED NOTHING EITHER, for the same reason it knows nothing: such a flow REPLAYS the
       document from the baseline, so it re-reaches its own source read and the substitution happens again in
       THIS session. Carrying a 1 across would say the bytes are in a program this session never built. */
    b->delivered = 0;
    return b;
}

void concolic_pins_blob_free(void *blob) {
    PinBlob *b = blob; if (!b) return;
    cons_seg_unref(b->seg);
    free(b);
}
/* THE PIN THIS FLOW HOLDS FOR `src`, AS THE VALUE IT IS — the two read sites ask for a JSValue and never for
   bytes, because a caller handed bytes has to decide what they mean and the two would eventually decide
   differently. JS_UNINITIALIZED = no pin, which is distinguishable from a pin of `undefined` and is why it is
   the sentinel: a source pinned to undefined must answer undefined, not "unpinned". */
static JSValue pin_of(JSContext *ctx, const char *src) {
    const Cons *c = cons_lookup(src);
    if (!c || !c->val) return JS_UNINITIALIZED;
    return pin_mint(ctx, (ConcolicLit)c->valkind, c->val);
}

static JSClassID g_concolic_class = 0;   /* runtime-allocated; 0 until concolic_init */

static void concolic_finalizer(JSRuntime *rt, JSValueConst val) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (!c) return;
    free(c->shape);
    free(c->src);
    free(c->root);
    free(c->ident);
    free(c->br_key);
    free(c->cmp_tok);
    free(c->cmp_subj);
    free(c->cmp_subj_ident);
    free(c->rel_tok);
    free(c->rel_subj);
    free(c->member);
    free(c->sp_meth);
    ident_list_free(c->sp_args, c->sp_nargs);
    free(c->sp_subj);
    ident_list_free(c->conj, c->conj_n);
    JS_FreeValueRT(rt, c->example);
    /* …AND THE SECOND OWNED JSValue ON THIS RECORD. A field added to the struct creates an obligation here and
       at concolic_gc_mark together, and the two are read as a pair for that reason. JS_UNINITIALIZED is not
       refcounted, so the never-asked case costs nothing and needs no test. */
    JS_FreeValueRT(rt, c->proto);
    free(c);
}

static void concolic_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (c && !JS_IsUndefined(c->example)) JS_MarkValue(rt, c->example, mark_func);
    /* THE DERIVED PROTOTYPE IS REACHABLE ONLY FROM HERE, so a walk that does not report it counts a live
       object as garbage. It is another value of THIS class, which is what makes the report obligatory rather
       than merely tidy: the chain a page walks is these records pointing at each other. */
    if (c && !JS_IsUninitialized(c->proto)) JS_MarkValue(rt, c->proto, mark_func);
}

/* @S CANDIDATE injection: during a verification re-run, the attacker source identified by g_cand_src returns
   the concrete breakout payload instead of a concolic, so the REAL code builds the exploit and it fires. */
static char *g_cand_src = NULL, *g_cand_payload = NULL;

/* IS THIS SOURCE THE ONE A CANDIDATE RE-FIRE IS SUBSTITUTING? One spelling, because it is asked at every mint
   a source can be reached through, and two copies are how the question came to be asked in the field-read path
   alone while a source installed as a plain property value never passed through it at all (see
   concolic_derived) — a candidate that could not be delivered and a sink that never fired. */
static int cand_matches(const char *src) {
    return g_cand_src && src && !strcmp(src, g_cand_src);
}

/* THE DECLARED SOURCES and what their component does to an attacker's bytes. A source that declares nothing
   delivers as-is, which is right for injected server state (`window.__STATE`) — the attacker writes that JSON
   directly and no component transforms it.
   AND EVERY ROW CARRIES ITS CLAIMANT. The array is this component's storage and each row is another
   component's agent state (core/platform.h's fourth paragraph), so the row has to say WHOSE — that is what
   `concolic_undeclare_sources` is keyed by, what the duplicate assert reports the existing owner from, and
   what concolic_free names when a release did not finish. The pointer is the caller's static component name,
   held the way core/agent_state.h holds one and for the same reason: it outlives the agent it names. */
/* `principal` IS A SECOND COLUMN AND NOT A SECOND REGISTRY, because it is a fact about the SAME row: whether
   the attacker WRITES this value or merely OWNS it (concolic.h states the pair, and HTML §9.3.2.2 "User
   agents" states why one exists at all). Keeping it here is also what makes the give-back complete without a
   second give-back — the claimant releases its rows and the property goes with them. */
typedef struct { const char *component; char *src; char *encode; char prefix; SourceDeliverKind deliver;
                 int principal; }
        SourceDelivery;
static SourceDelivery *g_srcs;
static int g_srcs_n, g_srcs_cap;

/* THE JOINT DOMAIN'S SEPARATOR — see concolic.h for what reaches it, why the identity is the SET and not an
   expression over it, and why a narrowing of the joint narrows no member.
   THE SEPARATOR IS PART OF THE IDENTITY, so a member that contained it would let two different sets compose to
   one key — the truncation defect in a different costume, asserted at the mint rather than trusted.
   IT IS DECLARED HERE, ABOVE THE REGISTRY, BECAUSE THE REGISTRY IS ITS SECOND READER. It used to sit beside
   the mint three hundred lines below, which was right while the only thing that ever looked at a joint
   identity was the code that composed one. */
#define CONCOLIC_JOINT_SEP " & "

/* MEMBER `i` OF A ROOT, AS A SLICE OF IT — the walk that makes "a provenance can name a SET" a fact the
   registry can act on rather than a string it fails to match.
   A root is either one source's name or several joined by the separator above, and the mint asserts that no
   member contains that separator — which is exactly what makes this walk EXACT rather than a guess. `*p`/`*n`
   point into `root` and are never copied: the registry compares them against declared names and nothing here
   outlives the call. Answers 0 past the last member, so a caller LOOPS and never first asks whether the root
   it holds is joint — there is no second spelling for the single-source case, which answers at i == 0 and
   stops. */
static int root_member(const char *root, int i, const char **p, size_t *n)
{
    size_t seplen = strlen(CONCOLIC_JOINT_SEP);
    const char *at = root, *s;

    if (!root) return 0;
    for (; i > 0; i--) {
        if (!(s = strstr(at, CONCOLIC_JOINT_SEP))) return 0;
        at = s + seplen;
    }
    s = strstr(at, CONCOLIC_JOINT_SEP);
    *p = at;
    *n = s ? (size_t)(s - at) : strlen(at);
    return 1;
}

/* THE DECLARED ROW FOR ONE MEMBER, matched over the slice rather than a copy of it. The terminator test is
   half the comparison: `strncmp` alone would match `location.hash` against a declared `location.hashish`. */
static int src_row(const char *p, size_t n)
{
    int i;

    for (i = 0; i < g_srcs_n; i++)
        if (!strncmp(g_srcs[i].src, p, n) && g_srcs[i].src[n] == '\0') return i;
    return -1;
}

/* WHICH DECLARED SOURCE A ROOT NAMES, over a root that may name SEVERAL — the one walk both registry readers
 * below share, so the rule that decides an ambiguous root is stated once instead of twice.
 *
 * WHY THIS EXISTS AT ALL. Both readers were an exact strcmp over the whole root, so a JOINT root matched no row
 * and they answered 0 / NULL — and 0 / NULL is not "I could not tell", it is the POSITIVE statement "this
 * source declares no delivery", which the report renders as "no component carries these bytes to the victim".
 * That is the defaulted-field defect standing at the exact place a wrong answer becomes a wrong PoC, and it
 * would have arrived the moment anything composed a provenance out of two attacker sources.
 *
 * A JOINT WITH ONE DECLARING MEMBER HAS AN ANSWER, AND IT IS THAT MEMBER'S. A value that is a joint function of
 * `location.search` and the viewport width carries attacker bytes through the query and nothing at all through
 * the viewport: the delivery is the query's, the encode set is the query's, and there is no second constraint
 * to state. That case is the ordinary one, because most joints in this engine are over ENVIRONMENT facts that
 * declare nothing.
 *
 * TWO DECLARING MEMBERS IS THE ONE WITH NO SINGLE HONEST ANSWER, so it crashes. Each declares its own
 * percent-encode set and its own address component (`#` against `?` — the two sets deliberately differ, which
 * is the whole reason those are two sources), so answering with either states one source's constraint as the
 * whole constraint on bytes that came from both, and the envelope then tells a researcher to deliver through
 * the component the payload did not ride. §@S: a shape carrying one of its two facts is a WRONG report rather
 * than a partial one. Build the per-member emission — one candidate seeded per declaring member, each with its
 * own envelope, the one that FIRES emitted — rather than a rule for picking. */
static int root_declared_row(const char *root)
{
    const char *p;
    size_t n;
    int i, row, found = -1;

    for (i = 0; root_member(root, i, &p, &n); i++) {
        if ((row = src_row(p, n)) < 0) continue;
        DCHECK(found < 0,
               "a value's delivery ROOT names TWO declared attacker sources, and a report can state one "
               "percent-encode set and one address component. Answering with either would present one "
               "source's constraint as the whole constraint on bytes that arrived through both — the "
               "envelope would name the component the payload did not ride. Seed one candidate per declaring "
               "member, each carrying its own envelope, and emit the one that fires");
        found = row;
    }
    return found;
}

/* THE TOKEN A REPORT CARRIES for a mechanism, spelled once so the engine owns the whole source vocabulary —
   the declaration side is the enum, the emission side is this, and a delivery layer switches on what comes out
   of here. A mechanism added to the enum without a token would cross as nothing at all, so it says so. */
static const char *deliver_token(SourceDeliverKind k)
{
    switch (k) {
    case SRC_DELIVER_ADDRESS:           return "address";
    case SRC_DELIVER_PLANT:             return "plant";
    case SRC_DELIVER_REFERRING_ADDRESS: return "referring-address";
    case SRC_DELIVER_USER_FILE:         return "user-file";
    case SRC_DELIVER_CROSS_DOCUMENT_MESSAGE: return "cross-document-message";
    }
    DFAIL("a source declared a delivery mechanism with no report token — the token is what the reproduction "
          "envelope states and what the delivery layer switches on, so the mechanism would cross as nothing");
    return NULL;
}

void concolic_declare_source(const char *component, const char *src, const char *encode, char prefix,
                             SourceDeliverKind deliver)
{
    int i;
    CHECK(src != NULL, "a source was declared with no identity");
    DCHECK(component != NULL && *component,
           "an attacker source was declared by no component — the row is a CLAIM whose claimant gives it back "
           "at its own release, so a row with no owner is a row nobody can release and a registry whose "
           "emptiness asserts nothing about anybody");
    /* THE TWO HALVES OF ONE DECLARATION MUST AGREE. A value carried in the victim's own address has the
       component it is carried at, and a value carried by any other mechanism has none — so a `#` beside a
       `plant`, or an `address` with no component, is a declaration whose reproduction cannot be built from it
       and whose candidate delivery would prepend a character no browser puts there. */
    DCHECK((deliver == SRC_DELIVER_ADDRESS) == (prefix != 0),
           "a source's delivery mechanism and its address component disagree — only a value carried in the "
           "victim's own address has a component, and it always has one");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            /* WHO ALREADY OWNS IT IS THE HALF THAT SAYS WHAT WENT WRONG. The same component declaring twice is
               a declaration that ran twice or a claimant whose rows are dynamic and did not ask first; a
               DIFFERENT component is two claimants over one source, and then the give-back of either takes a
               row it does not own. The old message could not tell those apart. */
            DFAILF("a source declared its browser delivery twice — `%s` is already declared by %s, and one "
                   "component owns one source in BOTH directions. A second declaration by that same "
                   "component is a claimant with dynamic rows that did not ask concolic_source_declared_by "
                   "first; a declaration by any other is two claimants over one row, whichever of them "
                   "releases first taking a row it does not own", src, g_srcs[i].component);
            return;
        }
    if (g_srcs_n == g_srcs_cap) {
        int c = g_srcs_cap ? g_srcs_cap * 2 : 8;
        SourceDelivery *a = reclaim_realloc(g_srcs, (size_t)c * sizeof *a);
        CHECK(a != NULL, "concolic: OOM declaring a source's delivery");
        g_srcs = a; g_srcs_cap = c;
    }
    g_srcs[g_srcs_n].component = component;
    g_srcs[g_srcs_n].src = strdup(src);
    g_srcs[g_srcs_n].encode = strdup(encode ? encode : "");
    g_srcs[g_srcs_n].prefix = prefix;
    g_srcs[g_srcs_n].deliver = deliver;
    /* A SOURCE THE ATTACKER WRITES, WHICH IS WHAT NEARLY EVERY SOURCE IS. The exception says so with its own
       call (concolic_declare_source_principal) rather than a sixth argument every declaration would have to
       spell — and it is written here, at the mint, so the column can never be the uninitialized garbage a
       later reader would read as "principal". */
    g_srcs[g_srcs_n].principal = 0;
    CHECK(g_srcs[g_srcs_n].src && g_srcs[g_srcs_n].encode, "concolic: OOM declaring a source's delivery");
    g_srcs_n++;
}

/* THE CLAIM, GIVEN BACK BY THE COMPONENT THAT MADE IT — core/platform.h's fourth paragraph, for the one
 * registry in the solver half that a browser component writes into. A declared row is agent state whose
 * STORAGE is this file's array and whose CLAIM is the declaring component's, so the claimant removes its own
 * rows at its own release and concolic_free asserts, at this component's, that none are left.
 *
 * KEYED BY THE CLAIMANT AND NOT BY THE SOURCE, and the difference is not spelling. A release naming each
 * source reads well where the rows are FIXED and fails where they are not: core/file/file_system.c declares
 * `file:NAME` per file the device has held, so it would have to keep a parallel list of the names it declared
 * — a second copy of this array, with its own growth, its own OOM edge and its own dedup, which must agree
 * with this one and has nothing to make it. That is the hand-copied-list defect core/platform.h and
 * core/realm.h exist to abolish, in miniature, and the next claimant with dynamic rows writes it again. The
 * owner is on the row instead: one list, and the give-back reads it.
 *
 * IT IS NOT A BULK RESET, WHICH IS WHAT KEEPS THE HOLDER'S ASSERT FALSIFIABLE. A call that emptied the array
 * would be a release undoing a declaration it did not make — the shape core/platform.c's own table check
 * forbids one column up, and the shape three of core/dom/document.c's sub-components were in until b87eef36
 * moved them back to their declarer — and "every claimant gave its rows back" and "one claimant forgot and the
 * wipe covered it" would then be the same state. Removing exactly the caller's rows is what makes
 * `g_srcs_n == 0` a fact about the CLAIMANTS, and what lets the assert name the one that did not finish.
 *
 * ZERO ROWS IS A LEGITIMATE ANSWER and is deliberately not asserted against: a component whose rows are
 * dynamic declares as many as the agent gave it, and none is one of those numbers. What a claimant that
 * declared and did not release costs is the assert at concolic_free, which is where it is owed. */
void concolic_undeclare_sources(const char *component)
{
    int i = 0;

    DCHECK(component != NULL && *component,
           "the source registry was asked to give back the claims of no component — the claim is keyed by its "
           "claimant, and a release with no name would either take every row or none");
    while (i < g_srcs_n) {
        if (strcmp(g_srcs[i].component, component) != 0) { i++; continue; }
        free(g_srcs[i].src);
        free(g_srcs[i].encode);
        /* THE ORDER OF THE REST IS PRESERVED rather than closed with a swap. Every read of this array is a
           lookup by identity, so order decides no answer — but it decides the order a WALK would report, and a
           registry that reshuffles itself when an unrelated component releases is a registry whose contents
           depend on teardown. Declaration order is a fact about the browser; nothing here may make it a fact
           about the sequence of frees. */
        memmove(&g_srcs[i], &g_srcs[i + 1], (size_t)(g_srcs_n - i - 1) * sizeof *g_srcs);
        g_srcs_n--;
    }
}

int concolic_source_declared_by(const char *component, const char *src)
{
    int i;

    DCHECK(component != NULL && *component && src != NULL,
           "the source registry was asked whether a component owns a row, with no component or no source — "
           "the answer decides whether a claimant declares a second time, so a half-formed question would "
           "either drop a source or trip the duplicate assert");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            /* THE DUPLICATE ASSERT SEEN FROM THE OTHER END. A claimant asking about a source ANOTHER component
               declared is about to skip a declaration it owes, or — worse — to give back a row it does not
               own, and the delivery a report then builds is for a source that component does not answer for. */
            DCHECK(strcmp(g_srcs[i].component, component) == 0,
                   "a component asked whether it owns an attacker source that a DIFFERENT component declared "
                   "— one source is owned by one component in both directions, so this is either a source "
                   "identity two components spell the same way or a claimant reaching into another's rows");
            return 1;
        }
    return 0;
}

/* THE SECOND HALF OF A ROW — see concolic.h for what a principal IS and what it decides.
 * IT IS A SEPARATE CALL AND NOT A SIXTH ARGUMENT, for the reason core/platform.h gives about every other
 * optional property of a claim: a parameter every declaration must spell is a parameter five components would
 * spell `0` for, and a `0` written five times to describe a property four of them have never heard of is the
 * shape a reader has to decode rather than read. The call names the exception.
 * IT REFUSES A ROW THIS COMPONENT DOES NOT OWN, both directions, because the row is a CLAIM: marking somebody
 * else's would put a property on a source this component does not answer for, and marking one that does not
 * exist would be a claim whose claimant the give-back can never find. */
void concolic_declare_source_principal(const char *component, const char *src)
{
    int i;

    DCHECK(component != NULL && *component && src != NULL,
           "a source was declared a PRINCIPAL by no component, or with no identity — the property rides the "
           "row, and the row is a claim whose claimant releases it");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            DCHECK(strcmp(g_srcs[i].component, component) == 0,
                   "a component declared an attacker source a PRINCIPAL that a DIFFERENT component owns — one "
                   "source is owned by one component in both directions, and a property written onto another "
                   "claimant's row outlives every release this component makes");
            g_srcs[i].principal = 1;
            return;
        }
    DFAIL("a source was declared a PRINCIPAL before its delivery was declared. The property is a COLUMN of the "
          "delivery row, so there is nothing here to write it onto — and a registry that grew a row for it "
          "would hold a source whose browser delivery nothing states, which the reproduction envelope reads as "
          "\"no component carries these bytes to the victim\". Call concolic_declare_source first");
}

/* HAS THIS FLOW DEMANDED A PARTICULAR VALUE OF AN ATTACKER'S PRINCIPAL? — see concolic.h.
 * A WALK OF THE REGISTRY RATHER THAN A LOOKUP BY KEY, because the question is asked of the DECLARATIONS: the
 * flow does not know which sources are principals and must not be taught a name for one, and this array is the
 * one place that does know. It is O(declared principals) — the number of sources whose value the browser
 * stamps rather than the attacker writes — and each row costs one indexed probe of the constraint chain. */
int concolic_principal_pinned(void)
{
    int i;

    for (i = 0; i < g_srcs_n; i++) {
        const Cons *c;
        if (!g_srcs[i].principal) continue;
        c = cons_lookup(g_srcs[i].src);
        if (c && c->pinned_root) return 1;
    }
    return 0;
}

int concolic_source_delivery(const char *root, const char **kind, char *prefix)
{
    int row = root_declared_row(root);

    DCHECK(kind != NULL && prefix != NULL, "a source's declared delivery was asked for with nowhere to put it");
    if (row < 0) return 0;
    *kind = deliver_token(g_srcs[row].deliver);
    *prefix = g_srcs[row].prefix;
    return 1;
}

const char *concolic_source_encodes(const char *root)
{
    int row = root_declared_row(root);

    return row < 0 ? NULL : g_srcs[row].encode;
}

/* WHAT THE MECHANISM ITSELF DOES TO A BYTE NOBODY DECLARED — the OTHER half of a delivery, and the half that
   used to be answered by one hardcoded clause for every source at once.
 *
 * A declared `encode` set states what distinguishes ONE component from its siblings: location.c declares the
 * backtick for the fragment and the apostrophe and `#` for the query, and its own comment says that difference
 * is the whole reason those are two sources. What it deliberately does NOT state is the part every URL
 * component SHARES, because restating it on each row would be a second copy of the standard's own text — and a
 * copy `bytes_probe` could not even hold, since it addresses each declared byte by ONE decimal digit and
 * DCHECKs the set at ten.
 * SO THE SHARED PART IS ASKED OF THE MECHANISM, and the mechanism is a COLUMN this registry already carries.
 * Deriving it from `deliver` rather than from a list of source names is what makes a mechanism added later
 * answer the question instead of silently inheriting whichever answer was hardcoded: the switch has no
 * default, so a new kind does not compile until somebody decides.
 *
 * THE THREE ANSWERS ARE NOT TWO. A payload serialized INTO a URL inherits URL §1.3 "Percent-encoded bytes"'s
 * C0 control percent-encode set, which that section defines as "consisting of C0 controls and all code points
 * greater than U+007E (~)" and which the fragment, query and special-query sets are each defined as
 * "consisting of the C0 control percent-encode set and" their own few ASCII additions — the additions being
 * exactly what the rows declare. A payload carried VERBATIM inherits nothing, and this is not an omission:
 * concolic.h's own SRC_DELIVER_CROSS_DOCUMENT_MESSAGE paragraph states it as a measured property of HTML
 * §9.3.3 "Posting messages" (step 7 serializes and step 8.4 deserializes, "and neither transforms a string"),
 * and file_system.c states the same for a file, "read verbatim, with no percent-encoding and no leading
 * character". Those two declarations were already in the tree, and the loop below was contradicting both.
 * THE THIRD ANSWER IS THAT THERE IS NO ANSWER, which is why this returns a tri-state and not a flag. A PLANT's
 * `encode` column is not an encode set at all — document_metadata.c declares it as what a cookie value "cannot
 * carry", per RFC 6265 §4.1.1 "Syntax", whose `cookie-octet = %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E` is
 * annotated "US-ASCII characters excluding CTLs, whitespace DQUOTE, comma, semicolon, and backslash". The row
 * lists that production's PRINTABLE exclusions; what it leaves unlisted is the CTLs, DEL and everything above
 * US-ASCII, and for those a plant neither percent-encodes (Chrome does not) nor carries (the production
 * forbids it) — it is refused. Answering either way there would be a false PoC, so the caller crashes. */
typedef enum { CARRIER_URL = 0, CARRIER_VERBATIM, CARRIER_CONSTRAINED } CarrierKind;

static CarrierKind root_carrier(const char *root)
{
    int row = root_declared_row(root);

    /* A ROOT WITH NO ROW IS NOT A CASE — it is the state the ONE caller has already excluded, so a plausible
       answer here would be the defaulted field §Architecture names rather than a fact. The caller returns the
       payload untouched when the root declares nothing AND the injection point does too, and when the
       injection point DOES declare one the DCHECK beside the call has already said the two name the same
       source — so reaching this with no row means those two facts were threaded from different operands and
       the mechanism about to be applied belongs to neither of them. */
    if (row < 0) {
        DFAIL("a candidate's delivery ROOT declares no source while its injection point declares one — the "
              "encode set and the carrier are the two halves of ONE row's answer, so a root with no row means "
              "the payload is about to be delivered under a mechanism nothing in this registry states for it");
        return CARRIER_VERBATIM;   /* release only — dev stops at the assert above */
    }
    switch (g_srcs[row].deliver) {
    case SRC_DELIVER_ADDRESS:                return CARRIER_URL;        /* the VICTIM'S own address */
    case SRC_DELIVER_REFERRING_ADDRESS:      return CARRIER_URL;        /* the address the victim arrives FROM */
    case SRC_DELIVER_USER_FILE:              return CARRIER_VERBATIM;   /* file_system.c: read verbatim */
    case SRC_DELIVER_CROSS_DOCUMENT_MESSAGE: return CARRIER_VERBATIM;   /* HTML §9.3.3 steps 7/8.4 */
    case SRC_DELIVER_PLANT:                  return CARRIER_CONSTRAINED;/* RFC 6265 §4.1.1's cookie-octet */
    }
    DFAIL("a source declared a delivery mechanism that does not say what it does to an UNDECLARED byte — the "
          "declared set is only the part that distinguishes one component from its siblings, so every "
          "mechanism must also state whether it serializes its payload into a URL (and so inherits URL §1.3 "
          "\"Percent-encoded bytes\"'s C0 control percent-encode set), carries it verbatim, or refuses bytes "
          "its own production excludes. Without that a candidate is delivered under whichever answer this "
          "switch was written for first");
    return CARRIER_VERBATIM;
}

/* JSConcolicHooks.lead — the DOMAIN fact the delivery declaration already holds, read by a builtin that has to
   decide whether one of its completions is feasible at all. The prefix is what the component states its
   component's value carries (`#` for a fragment, `?` for a query), so this is not a guess about the value: it
   is the same declaration the candidate delivery is built from, asked the other way round.
   A DERIVED value (`location.hash.slice(1)`) has its own source identity and no declaration, so it answers 0
   and nothing is refined — which is right, since slicing is exactly what removes the prefix. */
static int concolic_lead_hook(JSValueConst v)
{
    const char *src = concolic_src_c(v);
    int i;
    if (!src) return 0;
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) return (unsigned char)g_srcs[i].prefix;
    return 0;
}

/* THE CANDIDATE AS THE PAGE READS IT. The solver's payload is what the ATTACKER puts in the URL; this is what
   the browser hands the page, which is the only thing re-execution can honestly decide a breakout against.
 *
 * THE TWO HALVES OF THE DECLARATION ARE ASKED WITH TWO DIFFERENT KEYS, because they are facts about two
 * different things and only one of them survives a derivation.
 *   The ENCODE SET is a fact about the ROOT: the browser percent-encoded the bytes on the way INTO the address,
 *   and `location.hash.slice(1)` does not decode them, so a candidate substituted at the slice's RESULT must
 *   still be the encoded form. Asked with `src` — an exact strcmp that a derived identity never matches — the
 *   whole set went missing for exactly the derivations real code is written in, and the payload was handed over
 *   RAW. That is not a reporting gap: an HTML-context breakout containing `<` fired in the model at
 *   `innerHTML = location.hash.slice(1)` and cannot fire in Chrome, which encodes `<` in a fragment. It is the
 *   precise false-PoC generator this declaration was introduced to end, arriving through the derivation door.
 *   The PREFIX is a fact about the INJECTION POINT: `#` is a character the fragment's own value carries, and
 *   slicing is exactly what removes it (concolic_lead_hook states the same thing from the other side). So it
 *   comes off `src`'s own row, which a derived identity correctly does not have.
 * WHEN `src` HAS A ROW AT ALL, IT IS THE ROOT'S ROW — a value carrying a declared source's own name as its
 * identity is one that inherited it unchanged, so it inherited the root unchanged too. Asserted rather than
 * assumed: the two facts are threaded from an operand each, and taking them from DIFFERENT operands would
 * encode for one source and prefix for another with nothing to say so. */
static JSValue concolic_deliver(JSContext *ctx, const char *src, const char *root, const char *payload)
{
    const SourceDelivery *at = NULL;   /* the INJECTION POINT's row, if it has one: the component's prefix */
    const char *encode;                /* …and the ROOT's percent-encode set, which every derivation inherits */
    CarrierKind carrier;               /* …and what the ROOT's MECHANISM does to a byte that set does not name */
    const unsigned char *p;
    char *out;
    size_t o = 0, n;
    int i;

    /* THE ONE POINT AT WHICH THE ATTACKER'S BYTES BECOME A VALUE IN THE PAGE'S OWN PROGRAM — which is what
     * makes it the observation site for concolic_candidate_delivered, and the reason that fact can be a fact
     * at all. Both callers reach here through cand_matches, so there is no second door: a source read whose
     * identity is the substitution's returns the payload HERE or the payload is not in the program.
     * THE PAYLOAD IS ASSERTED RATHER THAN DEFAULTED, and the `if (!payload) payload = ""` that stood here is
     * deleted with its own reason. It made "the scheduler installed a substitution with no bytes" into a
     * delivery of the empty string: the source read succeeds, the page composes with nothing, every rung
     * measures a zero-length payload, and solve.c's own denominator assert (`o.len > 0`) fires two subsystems
     * away from the cause. That is §Architecture's defaulted field exactly — a hole a `?:` fills, in the one
     * value the whole search is about. `CHECK` and not `DCHECK` because it is dereferenced immediately below,
     * which is the rule solve.c states about its own two.
     * AND THE BYTES BELONG TO THE FLOW THAT WILL BE CREDITED FOR THEM. concolic_set_candidate asserts the
     * installed substitution matches the running flow at SWITCH-IN; this asks it at the moment the bytes
     * actually enter, because everything downstream — the survival rung, the arrival rung, the PoC — is
     * recorded against `flow_running()` and would be recorded against the wrong flow if the two had drifted
     * between the switch and the read. */
    {
        Flow *f = flow_running();
        CHECK(payload != NULL,
              "concolic: an @S candidate is being delivered with no payload — the substitution is (src,payload) "
              "as one identity and solve.c refuses to seed a flow holding half of it, so a NULL here is an "
              "installation that skipped concolic_set_candidate's own pairing; delivered as the empty string "
              "it would read as a source the page composed nothing out of");
        /* THE FLOW IS `CHECK`ED AND NOT ONLY `DCHECK`ED because it is now DEREFERENCED on this path in every
           build: the ladder write below reads and raises a field of it, so a release build that reached here
           with nothing running would segfault where dev names the cause. It is the same rule the assert above
           states about `payload`, arriving one line later for the same reason. */
        CHECK(f != NULL,
              "concolic: an @S candidate's bytes are entering the program with no flow running — a delivery is "
              "an observation ABOUT a flow (its ladder rung, its survival fraction, its PoC), so a delivery "
              "belonging to nobody is a substitution installed outside the scheduler's switch");
        DCHECK(f->cand_payload != NULL && !strcmp(f->cand_payload, payload),
               "an @S candidate's bytes are entering the program under a flow that is not carrying them — the "
               "survival and arrival rungs, and any PoC, are recorded against the RUNNING flow, so this "
               "delivery is about to be credited to a flow whose own payload is a different string");
        g_cand_delivered = 1;
        /* AND §@S'S BOTTOM LADDER RUNG, WRITTEN HERE BECAUSE THIS IS THE SITE — not because the flow happens
           to be in hand. §@S(i) requires every rung to have an observation site STRICTLY BEFORE the thing it
           is a distance to, and every other rung on the ladder reports at a SINK: a candidate that has replayed
           800 gates without reaching its own source read and one the scheduler has never served stood at the
           same 0 and ranked identically for the whole of the runway. This is the one honest observation in
           between, and it is honest precisely because it is not a claim about where the bytes have got to —
           "the payload is downstream of this operand" would need a taint tracker or a recorded
           transform-expression, both of which §Re-execution bans. It is the narrower fact that the bytes ARE
           IN THE PROGRAM, which only the component that puts them there can state.
           WRITING IT AT SOLVE.C'S SINK ENTRIES INSTEAD WOULD MAKE IT WORTH NOTHING, which is worth saying
           because that is the shorter diff: a rung recorded where the candidate has already reached a sink
           adds no separation the sink rungs do not already carry, and leaves "delivered, still on the runway"
           reading 0 beside "never delivered" — the two states the rung exists to tell apart.
           IT IS NOT THE SAME FACT AS `g_cand_delivered` ABOVE, AND THE TWO MAY NOT BE COLLAPSED. That flag is
           the component's live answer to "are this flow's bytes in the program RIGHT NOW", and it is CLEARED
           by concolic_clear_pins when a flow is entered fresh — a candidate that is RESTARTED rather than
           resumed replays from the baseline and its bytes are genuinely not in that replay's program until it
           delivers again, so solve.c's sink entries must see the 0. The rung is the COMPARATOR: §@S requires
           it monotone, so a later lower sample is another look at a fixed question and never a demotion, and
           flow_observe_rung's early return is what makes the re-delivery cost nothing. Same event, two
           quantities with opposite reset rules — the ledger/comparator split of §@S(ii) one level down. */
        flow_observe_rung(f, FLOW_RUNG_DELIVERED);
        /* AND THE SAME EVENT AT THE THIRD ACCOUNTING UNIT — the SEARCH's, which is the only one a reader ever
           sees. The three are not copies and none of them can be derived from the others: `g_cand_delivered`
           above is the COMPONENT's live answer for THIS replay and is cleared when a flow is entered fresh;
           the rung is the flow's COMPARATOR, monotone and re-earned across a park, which the WFQ reads at the
           pick; and this is a COUNT on the search, which orders nothing, outlives every flow that produced it
           and is what the parked report is written out of.
           IT IS WHAT MAKES THE REPORT'S BOTTOM STATE SAYABLE. Every other number a parked entry carries is
           observed AT a sink, so a search whose candidates ended before their own source read reported
           `turns:N,reached:0,survived:0` — byte-identical to one whose bytes DID enter the program and never
           reached a sink, and to one whose sinks all ran carrying none of them. Three opposite instructions
           under one reading, which is §@S's named tell (a rung whose absence and whose zero read alike); the
           count that separates them can only be taken here, because this is the site.
           NOT A CREDIT: solve.c raises a counter and touches no rung, no crossing and no frontier generation,
           so nothing about the ordering moves on this line. */
        solve_observe_substitution(f);
    }
    for (i = 0; i < g_srcs_n; i++)
        if (src && !strcmp(g_srcs[i].src, src)) { at = &g_srcs[i]; break; }
    encode = concolic_source_encodes(root);
    DCHECK(!at || (root && !strcmp(root, src)),
           "a candidate is being delivered at an injection point that names a DECLARED source while its "
           "delivery root names a different one — the two are threaded from an operand each, so this value "
           "took them from two, and the payload would be percent-encoded for one source and prefixed for "
           "the other");
    if (!at && !encode) return JS_NewString(ctx, payload);   /* nothing carries these bytes: delivered as itself */
    if (!encode) encode = "";
    /* THE SAME KEY AS THE ENCODE SET, and it has to be: the two are the declared and the shared halves of ONE
       row's answer, so reading them from different rows would apply one mechanism's rule to another's bytes —
       the mistake the DCHECK above catches for the PREFIX, one column over. */
    carrier = root_carrier(root);

    n = strlen(payload);
    out = reclaim_malloc(n * 3 + 2);
    CHECK(out != NULL, "concolic: OOM delivering a candidate");
    if (at && at->prefix) out[o++] = at->prefix;
    for (p = (const unsigned char *)payload; *p; p++) {
        /* THE BYTE IS OUTSIDE PRINTABLE US-ASCII, so what happens to it is the MECHANISM's to say and not this
           row's — see root_carrier. `< 0x20` is Infra's C0 control exactly ("a code point in the range U+0000
           NULL to U+001F INFORMATION SEPARATOR ONE, inclusive"), and `> 0x7E` is URL §1.3's "all code points
           greater than U+007E (~)" — which is why DEL needs no clause of its own: 0x7F is greater than 0x7E,
           and the `*p == 0x7F` that stood here was this set's high range implemented for ONE of its 129
           members. Byte-wise `%XX` over a UTF-8 payload IS §1.3's UTF-8 percent-encode, whose own loop runs
           "for each byte of encodeOutput" and percent-encodes each one under a set it asserts "includes all
           non-ASCII code points". */
        int shared = *p < 0x20 || *p > 0x7E;

        if (shared && carrier == CARRIER_CONSTRAINED) {
            /* NEITHER ANSWER IS AVAILABLE, so neither is given. Percent-encoding it claims a transform the
               carrier does not perform (a `;` does not reach a cookie as `%3B`, and a CTL does not reach one at
               all), and passing it raw claims a plant RFC 6265 §4.1.1's cookie-octet forbids — so the candidate
               would be recorded as a search that failed rather than as one that was never deliverable.
               WHAT THE NEXT DIFF BUILDS: the derivation's own constraint table (solver/solve_filter.h's
               SolveDelivered, which solve.c seeds with solve_delivered_all and narrows only from an observed
               run) starts from the DECLARATION for a constrained carrier, so solve_html.c's and solve_js.c's
               emitters decline such an escape at construction — the site the two-sided constraint already runs
               at — instead of it arriving here with no delivery to give it. HOW ITS ABSENCE SHOWS: this abort,
               reached today by a cookie-sourced JS-context search whose hole sits in a §12.4 SingleLineComment,
               because that state's exit escape is the one derived breakout in this engine carrying a byte
               outside printable US-ASCII. */
            DFAILF("an @S candidate carries the byte 0x%02X to a source whose carrier can neither encode it nor "
                   "hold it — the declared set lists only the PRINTABLE bytes the carrier's own production "
                   "excludes, so a byte outside US-ASCII has no delivery this component can state and both "
                   "answers are a false PoC. The escape had to be declined where it was CONSTRUCTED",
                   (unsigned)*p);
        }
        if ((shared && carrier == CARRIER_URL) || strchr(encode, (char)*p)) {
            static const char HEX[] = "0123456789ABCDEF";
            out[o++] = '%'; out[o++] = HEX[*p >> 4]; out[o++] = HEX[*p & 15];
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
    {
        JSValue r = JS_NewStringLen(ctx, out, o);
        free(out);
        return r;
    }
}
/* THE INSTALLED SUBSTITUTION BELONGS TO THE RUNNING FLOW — asserted here because this is where it is installed
   and the scheduler is the only caller. An installed candidate that is not the running flow's means an
   exploring flow is about to read an attacker payload where its concolic source belongs: it silently stops
   forking and stops being a detectable sink, which produces a smaller learned surface and no error at all.
   Two-sided on purpose — a candidate flow must have its own payload installed, and any other flow must have
   none — so neither direction of the mismatch can return. */
void concolic_set_candidate(const char *src, const char *payload) {
    free(g_cand_src); free(g_cand_payload);
    g_cand_src = src ? strdup(src) : NULL;
    g_cand_payload = payload ? strdup(payload) : NULL;
    {
        const Flow *f = flow_running();
        DCHECK(!!src == !!(f && f->cand_src),
               "the installed @S substitution does not match the running flow — an exploring flow would read "
               "the previous candidate's payload in place of its concolic source");
    }
}

/* HAVE THE RUNNING FLOW'S OWN CANDIDATE BYTES ENTERED THE PROGRAM? — see the static above for where the fact
   lives and how it travels; this is what it is FOR.
   IT IS THE PRECONDITION OF EVERY OBSERVATION MADE ABOUT A CANDIDATE'S BYTES, and until it existed there was
   none. solve.c's sink entries test `concolic_is(arg)` — "the injection did not reach this read" — and that
   test is TRUE OF EVERY LITERAL THE PAGE WRITES, which solve_html_sink's own comment says in those words. So
   during a candidate run the page's own `eval`, its own `innerHTML` and its own `location =` all reach the
   measurement, and the survival rung (§@S's "filter-survived") takes the longest run of the candidate's bytes
   in whatever string the PAGE built. A breakout is punctuation — `'><svg onload=X9()>`, `';X9();//` — so a run
   of one or two characters is found in almost any markup, and the flow's fitness, the search's ratchet and the
   popup's "S of L bytes of the furthest candidate survived" are then a reading of the page's own text.
   THAT IS A NUMBER ABOUT NOTHING, WHICH IS WORSE THAN A ZERO: §@S's three indistinguishable zero-states — a
   flow that never started, one whose bytes were eaten, one killed by a gate on the runway — were being told
   apart by a coincidence, so the one rung that does exist read nonzero for candidates whose bytes were never
   in the program at all. This fact is what the classes could not answer for themselves: a locator or an X9
   partition identifies WHOSE bytes a string holds only once they are somewhere to be found.
   A `0` IS NOT "NOT YET" — it is the positive statement that nothing in this program is this flow's, so a
   string that looks like the candidate's is the page's own. Read at the sink, never latched by a reader.
   AND IT IS NOT THE LADDER RUNG THE SAME EVENT WRITES. flow.h's FLOW_RUNG_DELIVERED is the WFQ's comparator
   coordinate and is monotone by §@S; this is the component's live answer and is cleared for a flow entered
   fresh, because a RESTARTED candidate's bytes are not in the replay it is now running. Anyone who reads the
   two as one number will delete whichever they met second: see the writer in concolic_deliver for why the
   opposite reset rules are the whole point rather than a redundancy. */
int concolic_candidate_delivered(void) {
    DCHECK(!g_cand_delivered || g_cand_payload != NULL,
           "a flow is recorded as having delivered its @S payload while no substitution is installed — the "
           "delivery is set at concolic_deliver, which cand_matches only reaches with one installed, so the "
           "two have come apart and every sink observation below this is about to be attributed on a fact "
           "belonging to a candidate that is no longer running");
    return g_cand_delivered;
}

/* THE MEMBER'S OWN EXAMPLE — the third of §Solver's triple, computed by RUNNING THE REAL READ on the parent's
   example rather than by deriving anything from a recorded expression.
 *
 * A concolic whose example is a RECORD is what the real codec hands back: 25.5.1 over an unknown text runs the
 * engine's own `JSON.parse` on the source's example and the completion carries that parsed value. Until this
 * existed, every field read off it minted example-free, so a loaded config's `region` — a value the run had
 * concretely in hand — reached an @H parameter as a hole with no value under it, which is §@H's "a shape
 * states two facts" with one of them thrown away. Absence stays absence: a member the record does NOT hold
 * yields no example, which is the honest statement that the payload this visitor was served does not carry
 * that field, and is exactly what keeps the gate over it forking instead of inventing a value for it.
 *
 * NOT [[Get]] — JS_GetOwnSlotDesc, which runs NONE of the page's code. Three reasons and each is load-bearing:
 * a prototype walk would answer `hasOwnProperty` and `toString` for members no server ever wrote; a getter is
 * the page's own function and calling it from this C activation is the drive-to-completion with no flow base
 * the whole flow machinery exists to avoid; and the question being asked is whether the RECORD holds the
 * member, which is [[GetOwnProperty]]'s question and not [[Get]]'s.
 *
 * AN ACCESSOR ON AN EXAMPLE IS ANSWERED WITH NO EXAMPLE, and that is a positive statement rather than a
 * default: its value is whatever the page's getter would compute, this read may not run it, so there is no
 * concrete value here to report — the same answer as a member the record does not hold, for a different and
 * equally honest reason. `JSON.parse` builds only data properties, so the only route to one is a 25.5.1 step 9
 * reviver that defined one, which is the page's own choice. */
/* ONE READ OF THE EXAMPLE'S OWN SLOT, answering BOTH questions the record is asked about a member: WHAT it
   holds (the value a derived member carries) and WHETHER it holds it, with the attributes it states. They are
   one §6.2.6 Property Descriptor and two reads of it would be two answers about one record — the same
   one-fact-two-fields defect the Concolic struct's own comment records twice over.
   1 = the example holds an own slot for `atom`; `*out_value` is OWNED (JS_UNDEFINED for an accessor, per the
   paragraph above) and `*out_flags` is the slot's own C/W/E. 0 = it holds none. */
static int example_slot(JSContext *ctx, JSValueConst parent_example, JSAtom atom,
                        JSValue *out_value, int *out_flags)
{
    JSPropertyDescriptor pd;
    int has;

    *out_value = JS_UNDEFINED;
    *out_flags = 0;
    /* A primitive example has no members to read: `{hash}.length` off a concrete fragment is a DERIVATION and
       not a slot, and answering it from this object-shaped read would be this file inventing the operation. */
    if (!JS_IsObject(parent_example))
        return 0;
    has = JS_GetOwnSlotDesc(ctx, &pd, parent_example, atom);
    DCHECK(has >= 0,
           "reading a member off a concolic's own example threw — JS_GetOwnSlotDesc runs none of the page's "
           "code and aborts on a Proxy rather than trapping, so the only completion it has is a value, and an "
           "exception raised here would be left on a context with no flow base to hand it to");
    if (has <= 0)
        return 0;
    *out_flags = pd.flags;
    if (pd.flags & JS_PROP_GETSET) {
        JS_FreeValue(ctx, pd.getter);
        JS_FreeValue(ctx, pd.setter);
        return 1;
    }
    *out_value = pd.value;
    return 1;
}

static JSValue example_member(JSContext *ctx, JSValueConst parent_example, JSAtom atom)
{
    JSValue v;
    int flags;

    return example_slot(ctx, parent_example, atom, &v, &flags) ? v : JS_UNDEFINED;
}

/* Exotic [[Get]]: reading ANY field of a concolic value yields a DERIVED concolic — unknown injected/attacker
   state is unknown per-field, carrying the FIELD-PATH identity ("{state}.admin"), which doubles as the source
   identity for @S injection. This is what lets a gated `if (state.admin)` fork AND lets an @S candidate inject
   at a precise source. The derived value carries the parent's example READ THROUGH, so a field the record
   holds arrives with the bytes the server sent and a field it does not hold arrives with none — opaque for
   control flow either way, which is what §solver's trust boundary requires of a loaded config: "a loaded
   `features.admin:false` must NOT concretize the gate, or the admin endpoint is lost". */
static JSValue concolic_exotic_get(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst receiver) {
    (void)receiver;
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    const char *field, *f[2];
    char *shape, *ident;
    JSValue r;

    if (!c) return JS_UNDEFINED;
    field = JS_AtomToCString(ctx, atom);
    shape = shapef("%s.%s", c->shape ? c->shape : "{}", field ? field : "?");
    f[0] = c->ident; f[1] = field;                           /* a name that would not convert -> no identity */
    ident = concolic_ident_compose(".", f, 2);
    if (field) JS_FreeCString(ctx, field);
    if (cand_matches(shape)) {                               /* candidate run: this source -> the concrete breakout */
        r = concolic_deliver(ctx, shape, c->root, g_cand_payload);
        free(shape); free(ident);
        return r;
    }
    r = pin_of(ctx, shape);                                  /* an EQ gate pinned this source -> the real value */
    if (!JS_IsUninitialized(r)) {
        free(shape); free(ident);
        return r;
    }
    /* src = the field path (a precise @S injection point), root = the parent's, unchanged. A field of an
       unknown object is a datum the attacker controls SEPARATELY — which is why this mints a new injection
       identity at all — but it is not a datum that arrives by a different route: whatever carried the object's
       bytes in carried this field's, and a report has to say so. */
    r = concolic_alloc(ctx, shape, shape, c->root, ident, example_member(ctx, c->example, atom));
    /* THE NAME THIS READ WENT THROUGH, KEPT ON THE VALUE — the one honest source for the method name the call
       handler below reports, and the reason it is stamped here rather than parsed out of the shape: a page may
       name a property anything at all, `.` included, so recovering it from `{x}.a.b` would answer `b` for a
       property literally called `a.b`. The atom cannot be wrong about itself.
       IT IS RE-ASKED RATHER THAN REUSED because `field` was already given back above — the mint is what may
       fail and the free follows it, so this asks the atom again for the four bytes it costs instead of moving
       a lifetime. A name that would not convert leaves this NULL, which is the same positive statement the
       identity above makes for it: the call over it states no predicate and both arms of every branch stay. */
    {
        const char *name = JS_AtomToCString(ctx, atom);
        if (name) {
            Concolic *rc = JS_GetOpaque(r, g_concolic_class);
            DCHECK(rc != NULL, "a member read was minted as something that is not a concolic value");
            DCHECK(rc->member == NULL,
                   "a member read was given a second property name — a value is read under ONE name, so a "
                   "second write is two reads wearing one value and the call over it would report whichever "
                   "of the two landed last");
            rc->member = strdup(name);
            CHECK(rc->member, "concolic: OOM recording the name a member read went through");
            JS_FreeCString(ctx, name);
        }
    }
    free(shape);
    return r;
}

/* THE FOUR OPERATORS, SPELLED ONCE — the identity's operator field, and the ONE place the four equality
   spellings a program can write are turned into the four ids a constraint key is composed from. It is a
   function rather than four literals at the mint because concolic_new_cmp composes the same field for a
   component whose IDL member IS a comparison, and the two must agree exactly or a page writing the comparison
   the component declares would fork a second time over the same predicate. */
static const char *cmp_op_ident(int is_neq, JSConcolicEqOp op) {
    if (op == JS_CONCOLIC_EQ_STRICT) return is_neq ? "!==" : "===";
    DCHECK(op == JS_CONCOLIC_EQ_LOOSE,
           "an equality was spelled for an algorithm quickjs.h does not declare — the identity would name a "
           "predicate no operator produces, and two real ones would compose under it");
    return is_neq ? "!=" : "==";
}

/* MINT A COMPARISON RESULT — the boolean `if (x === 'admin')` and `if (x < 700)` branch on.
 *
 * ITS IDENTITY IS THE OPERATOR AND BOTH OPERANDS, which is the whole of the predicate, so a comparison result
 * is an ordinary derived value as far as decide.c is concerned and that file needs no second key rule for it.
 * `ia`/`ib` are the operands' identities in the order the composition wants them and are CONSUMED; `eq_kind`
 * and `tok` are the PIN, which only an equality against a concrete side has (an ordering narrows a domain and
 * determines no value — §Solver-half: a range-gated parameter stays a domain-annotated shape). */
static JSValue pred_new(JSContext *ctx, const char *op, const char *src, const char *root, char *ia, char *ib,
                        int eq_kind, int algo, ConcolicLit tok_kind, const char *tok, const char *subj)
{
    const char *f[3];
    char *ident;
    JSValue r;
    Concolic *c;

    DCHECK(op != NULL,
           "a comparison result was minted with no OPERATOR. The operator is half the predicate: without it "
           "`x < 700` and `x > 700` compose to one identity, the flow's record of either DECIDES the other, "
           "and the arm it deletes is one nothing contradicts");
    /* THE PIN IS THREE FIELDS. The operator says an arm determines something, the token says WHAT, and the
       subject says WHICH HOLE the report must state it under — decide.c pins on one arm and EXCLUDES on the
       other, and it reads all three at the same line.
       THE SUBJECT IS THE ONE OF THE THREE THAT MAY BE HONESTLY ABSENT, and its absence is a POSITIVE
       statement rather than a hole: a value whose shape is the unnameable `{}` has no hole the @H surface
       will print (endpoint.c's path scan mints no param for one either), so there is no name to file a domain
       under. The pin is unaffected, because a pin is keyed by `src` and every concolic has one. */
    DCHECK(eq_kind == OPCMP_NONE ? (tok == NULL && subj == NULL) : (tok != NULL),
           "a comparison result's operator and pin token disagree about whether its arms determine anything "
           "— the two are written together at this mint and read together by decide.c, which pins on one arm "
           "and records the exclusion on the other");
    /* THE TOKEN AND ITS KIND ARE ONE ARGUMENT IN TWO HALVES, asserted where they meet. A spelling with no
       kind is what decide.c would pin as a string whatever the page compared against — the nine characters of
       `undefined` for a value the flow proved falsy — and a kind with no spelling is a type stated about
       nothing. Neither is recoverable downstream: by the time a later read asks the store, the operand is
       gone and only the pair is left. */
    DCHECK((tok != NULL) == (tok_kind != CONCOLIC_LIT_NONE),
           "a comparison result's pin token and its KIND disagree about whether the concrete side can be "
           "spelled at all — the two come from one classification (operand_kind, through literal_tok), so a "
           "value carrying one without the other is a pin whose own mint cannot say what type it pinned");
    /* AND THE ALGORITHM IS THE PIN'S FOURTH HALF, asserted here for the reason the kind is: it is what says
       what the holding arm may CONCLUDE, and no consumer can recover it later — by the time decide.c reaches
       the arm, the operands are gone and only this record is left. An ordering has no algorithm and says so;
       an equality has one and may not omit it, because JS_CONCOLIC_EQ_LOOSE is zero and an omission would read
       as the page having written `==`, which is the arm that refuses the pin — safe, and a lie about the
       source text either way. */
    DCHECK(eq_kind == OPCMP_NONE ? algo == CMP_ALGO_NONE
                                 : (algo == JS_CONCOLIC_EQ_LOOSE || algo == JS_CONCOLIC_EQ_STRICT),
           "a comparison result's operator and its equality ALGORITHM disagree about whether it is an equality "
           "at all — an ordering carrying an algorithm names a §7.2.13/§7.2.14 reading of a predicate neither "
           "defines, and an equality without one is a pin whose arm cannot say whether it determined a value "
           "or merely bounded it");
    f[0] = op; f[1] = ia; f[2] = ib;
    ident = concolic_ident_compose("?", f, 3);
    free(ia); free(ib);
    /* NOT a source read, so no candidate substitution — see concolic_alloc's declaration. */
    r = concolic_alloc(ctx, "{cmp}", src, root, ident, JS_UNDEFINED);
    c = JS_GetOpaque(r, g_concolic_class);
    DCHECK(c != NULL, "a comparison result was minted as something that is not a concolic value");
    c->cmp_op = eq_kind;
    c->cmp_tok = tok ? strdup(tok) : NULL;
    CHECK(!tok || c->cmp_tok, "concolic: OOM recording the value an equality pins to");
    c->cmp_kind = (signed char)tok_kind;
    c->cmp_algo = (signed char)algo;   /* what the arm may CONCLUDE — written with `cmp_op`, for its reason */
    c->cmp_subj = subj ? strdup(subj) : NULL;
    CHECK(!subj || c->cmp_subj, "concolic: OOM recording the hole an equality is a fact about");
    return r;
}

/* THE UNKNOWN OPERAND'S OWN IDENTITY, STAMPED ON A PREDICATE pred_new HAS JUST MINTED — a second call for
   pred_set_bound's reason exactly, and the reason is stronger here: only ONE of pred_new's three callers has
   an operand VALUE at all. concolic_new_cmp mints a comparison result for a component whose IDL member IS a
   comparison (HTML §6.2's `hidden`), so there is no separate operand this engine ever minted and nothing whose
   example could be filed against — an absence that is a fact about that entry rather than a field it forgot.
   THE PAIR IS ASSERTED, because the hole and the identity are two names for ONE operand and a predicate
   carrying one without the other is a decide.c that can exclude a value it cannot degrade, or degrade one it
   cannot name in the report. */
static void pred_set_subject_ident(JSValueConst pred, const char *ident) {
    Concolic *c = JS_GetOpaque(pred, g_concolic_class);

    DCHECK(c != NULL, "an operand identity was stamped on something that is not a comparison result");
    DCHECK(c->cmp_subj != NULL,
           "a predicate was given the identity of an unknown operand it records no HOLE for — the two are "
           "written under one condition and read by one line of decide.c, so a result carrying only the "
           "identity is a fact the report cannot name and a fact the domain cannot be filed under");
    DCHECK(c->cmp_subj_ident == NULL,
           "a comparison result was given a second subject identity — a predicate is about ONE unknown "
           "operand, so a second write is two values wearing one predicate");
    c->cmp_subj_ident = strdup(ident);
    CHECK(c->cmp_subj_ident, "concolic: OOM recording the value an equality is a fact about");
}

/* THE ORDERING'S OBSERVATION, STAMPED ON A PREDICATE pred_new HAS JUST MINTED. It is a second call rather than
   four more parameters because only ONE of pred_new's callers has an ordering to state, and a signature every
   caller has to spell `REL_NONE, NULL, 0, NULL` into is a signature that invites a caller to spell it wrong.
   The three facts are written HERE, together, so the assert that they are one observation has a single site. */
static void pred_set_bound(JSValueConst pred, RelOp rel, const char *tok, double num, const char *subj) {
    Concolic *c = JS_GetOpaque(pred, g_concolic_class);

    DCHECK(c != NULL, "an ordering's bound was stamped on something that is not a comparison result");
    DCHECK(rel != REL_NONE && tok != NULL && subj != NULL,
           "an ordering's relation, token and subject were not all present at the one line that writes them — "
           "decide.c reads all three at once to state a bound, so a result carrying some of them is a "
           "constraint that can be observed and never reported");
    DCHECK(c->rel_op == REL_NONE, "a comparison result was given a second bound — a predicate is one relation "
                                  "over one pair, so a second write is two facts wearing one identity");
    c->rel_op = rel;
    c->rel_num = num;
    c->rel_tok = strdup(tok);
    CHECK(c->rel_tok, "concolic: OOM recording the literal an ordering bounds a value by");
    c->rel_subj = strdup(subj);
    CHECK(c->rel_subj, "concolic: OOM recording the hole an ordering is a fact about");
}

/* THE CALL PREDICATE'S OBSERVATION, STAMPED ON THE RESULT concolic_call HAS JUST MINTED. It is a second call
   for pred_set_bound's reason exactly: only ONE minting path has a call predicate to state, and a signature
   every other caller has to spell `NULL, NULL, 0, NULL` into is a signature that invites a caller to spell it
   wrong. The four facts are written HERE, together, so the assert that they are one observation has one site.
   `args` is CONSUMED — the array and every string in it — because the caller has just built the only copy and
   two owners of one list is the lifetime rule this file refuses to keep true by hand. */
static void pred_set_strpred(JSValueConst v, const char *method, char **args, int nargs, const char *subj) {
    Concolic *c = JS_GetOpaque(v, g_concolic_class);

    DCHECK(c != NULL, "a call predicate was stamped on something that is not a concolic value");
    DCHECK(method != NULL && subj != NULL,
           "a call predicate's method and subject were not both present at the one line that writes them — "
           "decide.c reads them together to state a domain, so a result carrying one of them is a constraint "
           "that can be observed and never reported");
    DCHECK(nargs == 0 || args != NULL,
           "a call predicate was stamped with an argument COUNT and no list — the caller builds both or "
           "neither, so a count alone is an unspellable argument arriving as a spellable one");
    DCHECK(c->sp_meth == NULL,
           "a call result was given a second call predicate — a call is one method over one receiver, so a "
           "second write is two predicates wearing one identity");
    c->sp_meth = strdup(method);
    CHECK(c->sp_meth, "concolic: OOM recording the method a gate called on an unknown");
    c->sp_args = args;   /* consume */
    c->sp_nargs = nargs;
    c->sp_subj = strdup(subj);
    CHECK(c->sp_subj, "concolic: OOM recording the hole a call predicate is a fact about");
}

/* CARRY A PREDICATE'S OBSERVATION ONTO ITS NEGATION — UNCHANGED, WHICH IS THE POINT AND NOT AN OVERSIGHT.
 *
 * decide.c reads the equality, the ordering and the call predicate off the value a branch tests, and it reads
 * them TOGETHER WITH THE ARM: it pins on the arm that makes the equality true, excludes on the other, states
 * `rel` or `rel_op_negate(rel)` by the arm, files `holds` as the arm. So there are two places the negation
 * could live — in the RECORD or in the ARM — and putting it in the record would mean negating three different
 * kinds of fact in three different ways, one of which (a call predicate) has no negation at all: §RUN-DON'T-
 * MATCH forbids deciding what `startsWith` MEANS, so there is no paired method to flip to.
 * Putting it in the ARM costs one XOR and is the same statement for all three. So the observation is carried
 * verbatim and `br_neg` carries the polarity — which is why this function copies and never inverts.
 *
 * IT IS NOT A DERIVATION AND SO IT IS NOT THE INVERSION §Re-execution FORBIDS. `x.slice(1).startsWith("/api")`
 * is a fact about `x.slice(1)` and carrying it onto `x` would require undoing `slice`, which is banned by
 * name. Nothing is undone here: `!p` and `p` are the SAME boolean, and §7.1.2 ToBoolean — the only operation
 * between the value and the branch — is applied identically whichever way the page spelled the test.
 *
 * `c->member` IS DELIBERATELY NOT CARRIED. It is the property NAME a member read went through, and a negation
 * is not a read of anything; carrying it would make `!x.foo` answer that it was read under `foo`, and the call
 * handler names the method it reports from exactly that field.
 *
 * `c->conj` IS NOT CARRIED EITHER, AND FOR A SHARPER REASON THAN THAT ONE. The three records above are
 * OBSERVATIONS a branch reads together with its arm, so carrying them verbatim is exactly right; the conjunct
 * list is a statement about WHAT THIS VALUE IS, and `!(p ∧ q)` is not a conjunction — it is a disjunction of
 * two negations. Copying the list would let a later `(!(p ∧ q)) ∧ r` flatten `p` and `q` into its own set and
 * compose the key of `p ∧ q ∧ r`, which is a DIFFERENT proposition that the flow would then decide off this
 * one's record. The negation keeps its own identity and enters any parent set as one opaque member, which is
 * both correct and all this engine claims: it files one entry per predicate and reasons inside none of them. */
static void pred_carry_through_not(JSValueConst dst, JSValueConst src) {
    Concolic *d = JS_GetOpaque(dst, g_concolic_class);
    Concolic *s = JS_GetOpaque(src, g_concolic_class);
    int k;

    DCHECK(d != NULL && s != NULL, "a predicate was carried through a negation between values that are not "
                                   "both concolic — the record read from and the record written to are the "
                                   "same class or there is no observation to carry");
    DCHECK(d->cmp_op == OPCMP_NONE && d->rel_op == REL_NONE && d->sp_meth == NULL,
           "a negation was given a second predicate — a value negates ONE operand, so a record that already "
           "holds an observation is two predicates wearing one identity and decide.c's own asserts would then "
           "fire naming a defect one mint away from here");
    DCHECK(d->conj_n == 0,
           "a negation was minted onto a record that already names a set of CONJUNCTS — a negation is not a "
           "conjunction (`!(p ∧ q)` is a disjunction of two negations), so a parent conjunction would flatten "
           "this value's members into its own set and compose the key of a proposition nothing here states");
    d->cmp_op = s->cmp_op;
    if (s->cmp_tok)        { d->cmp_tok        = strdup(s->cmp_tok);        CHECK(d->cmp_tok,        "concolic: OOM carrying an equality's token through a negation"); }
    d->cmp_kind = s->cmp_kind;   /* the token's other half — carried on the same line, for `cmp_tok`'s reason */
    d->cmp_algo = s->cmp_algo;   /* …and what the arm may CONCLUDE. `!(x == L)` is `x == L` read the other way
                                    round — the operand, the token and the ALGORITHM are all the same, and a
                                    negation that dropped this would let a `==` reach decide.c looking like a
                                    predicate with no algorithm, whose pin is refused for the wrong reason. */
    if (s->cmp_subj)       { d->cmp_subj       = strdup(s->cmp_subj);       CHECK(d->cmp_subj,       "concolic: OOM carrying an equality's hole through a negation"); }
    if (s->cmp_subj_ident) { d->cmp_subj_ident = strdup(s->cmp_subj_ident); CHECK(d->cmp_subj_ident, "concolic: OOM carrying an equality's subject identity through a negation"); }
    d->rel_op  = s->rel_op;
    d->rel_num = s->rel_num;
    if (s->rel_tok)  { d->rel_tok  = strdup(s->rel_tok);  CHECK(d->rel_tok,  "concolic: OOM carrying an ordering's token through a negation"); }
    if (s->rel_subj) { d->rel_subj = strdup(s->rel_subj); CHECK(d->rel_subj, "concolic: OOM carrying an ordering's hole through a negation"); }
    if (s->sp_meth)  { d->sp_meth  = strdup(s->sp_meth);  CHECK(d->sp_meth,  "concolic: OOM carrying a call predicate's method through a negation"); }
    if (s->sp_subj)  { d->sp_subj  = strdup(s->sp_subj);  CHECK(d->sp_subj,  "concolic: OOM carrying a call predicate's hole through a negation"); }
    d->sp_nargs = s->sp_nargs;
    if (s->sp_nargs) {
        DCHECK(s->sp_args != NULL,
               "a call predicate carries an argument COUNT with no list — the mint writes both or neither, so "
               "the negation is about to read a list its own operand does not have");
        d->sp_args = malloc((size_t)s->sp_nargs * sizeof(char *));
        CHECK(d->sp_args, "concolic: OOM carrying a gate's argument list through a negation");
        for (k = 0; k < s->sp_nargs; k++) {
            d->sp_args[k] = strdup(s->sp_args[k]);
            CHECK(d->sp_args[k], "concolic: OOM carrying an argument a gate passed through a negation");
        }
    }
}

/* A COMPARISON-RESULT bool carrying `src <op> tok`, for a component whose IDL member IS a comparison over its
   own source (HTML §6.2's `document.hidden` is `visibilityState === "hidden"`). It composes the identity the
   page's own `x === tok` composes for the same source and token, which is what makes the two ONE constraint
   entry — the property page_visibility.h states and now the encoding rather than a coincidence of spelling. */
JSValue concolic_new_cmp(JSContext *ctx, const char *src, int op, ConcolicLit kind, const char *tok) {
    const char *sf[1], *kf[2];

    DCHECK(op == OPCMP_EQ || op == OPCMP_NE,
           "a comparison result was declared with an operator that is neither an equality nor an inequality — "
           "an ordering is minted by the relational hook, which composes the engine's own operator id");
    /* THE COMPONENT STATES THE KIND AND THIS ENTRY NO LONGER ASSUMES IT. It spelled `"s"` unconditionally,
       which is a claim about every component that will ever declare such a member and not about this call:
       a member comparing against a number would have composed the key of a comparison against the STRING of
       its digits — a different predicate from the one the page's own `x === 5` composes, so the two would
       fork each other's settled gate — and pinned the source to those digits. The kind is an argument for the
       same reason the token is: only the caller knows it, and there is no default that is right. */
    DCHECK(kind != CONCOLIC_LIT_NONE,
           "a component declared its member as a comparison against an operand this engine cannot spell — an "
           "Object or a Symbol has no identity that survives the park a resumed flow replays through, so a "
           "member whose IDL says it compares against one is stating a predicate no key can name");
    sf[0] = src;
    kf[0] = lit_tag(kind); kf[1] = tok;
    /* AND IT IS THE **STRICT** SPELLING, WHICH IS WHAT MAKES THIS ONE ENTRY WITH THE PAGE'S OWN TEST. The
       specs that define these members define them as strict equalities of strings — HTML §6.2 "Page
       visibility" states this one as "The `hidden` getter steps are to return true if this's visibility state
       is "hidden", otherwise false" — so a page that writes the comparison out writes
       `===`, reaches concolic_cmp_hook with JS_CONCOLIC_EQ_STRICT, and composes `===` there. Spelling `==`
       here would make the component's own member and the page's own test two predicates over one source, each
       forking the other's settled gate. A component whose IDL member is a LOOSE comparison does not exist and
       would have to say so at this entry rather than be assumed by it. */
    /* THE COMPONENT'S OWN MEMBER IS A SOURCE READ, so it is its own root exactly as concolic_new's is. */
    /* THE COMPONENT'S SOURCE IS ITS OWN HOLE. A declared source's shape is its provenance in braces
       (concolic_source_wrap asserts exactly that), so stripping them gives `src` back — stated here rather
       than by calling the stripper on a shape this function was never handed. */
    return pred_new(ctx, cmp_op_ident(op == OPCMP_NE, JS_CONCOLIC_EQ_STRICT), src, src,
                    concolic_ident_compose("s", sf, 1), concolic_ident_compose("k", kf, 2),
                    op, JS_CONCOLIC_EQ_STRICT, kind, tok, src);
}
/* …AND THE TWIN FOR A RELATION OVER TWO LIVE VALUES, for a browser component whose own algorithm compares two
 * operands either of which may be unknown (HTML §8.7's timer task source orders one expiry against another).
 *
 * IT EXISTS SO THERE IS STILL ONE SPELLER OF THE KEY. The component could not reach concolic_rel_hook: that
 * entry takes the INTERPRETER'S OWN OPCODE for the operator, which is an identity quickjs never exports, and a
 * host that invented a number for it would be spelling a second opcode namespace beside the engine's. Nor could
 * it compose the key itself — decide.c keys a constraint by the value's identity precisely so that the format
 * lives in one place. So the component states the SPEC RELATION IT IS PERFORMING as the operator's name and
 * this file composes the identity, exactly as it composes `rel%d` for the interpreter; the two namespaces
 * cannot collide because concolic_ident_compose writes every field length-prefixed.
 *
 * IT PINS NOTHING, and that is the relation's own property rather than a shortcut. §Solver-half: an ordering
 * narrows a domain and determines no value, and an equality whose other side is also unknown has no concrete
 * value to pin to — so the pin stays where a pin can be honest, in the equality hook over a source and a
 * literal token. Both operands are BORROWED. */
JSValue concolic_new_rel(JSContext *ctx, const char *op, JSValueConst a, JSValueConst b) {
    JSValueConst opq;

    DCHECK(op != NULL,
           "a component asked for a relation over two values without naming the RELATION — the operator is "
           "half the predicate, so two different comparisons of one pair would compose to one identity and "
           "the flow's record of either would decide the other");
    DCHECK(concolic_is(a) || concolic_is(b),
           "a component asked the solver to relate two values NEITHER of which is unknown — a relation over "
           "two concrete operands is decided by running it, and minting a predicate for it would put a fork "
           "in the frontier over a question the engine can already answer");
    opq = concolic_is(a) ? a : b;
    return pred_new(ctx, op, concolic_src_c(opq), concolic_root_c(opq),
                    ident_of_operand(ctx, a), ident_of_operand(ctx, b),
                    OPCMP_NONE, CMP_ALGO_NONE, CONCOLIC_LIT_NONE, NULL, NULL);
}

/* THE TAG THAT PUTS A CONJUNCTION IN ITS OWN NAMESPACE. It cannot collide with any other composition in this
   file — every field is written `<length>:<bytes>`, so no tag's bytes can spell another's boundary and a
   two-member `&` (the bitwise arithmetic operator, carith_name) is a different string of a different length
   from this one anyway. Spelled once because both the identity and the record's own invariant are about it. */
#define CONCOLIC_CONJ_TAG "&&"

/* THE CONJUNCTION'S EXAMPLE — the real `&&` run on the conjuncts' own examples, attached only when BOTH carry
   one, because a conjunction whose second operand has no concrete answer has none itself.
   IT IS READ THROUGH concolic_example AND NEVER OFF THE RECORD, which is what makes the example this flow's
   rather than the value's: that reader withholds an example the RUNNING FLOW has proved wrong, so a
   conjunction over a contradicted conjunct arrives with no example instead of with a witness its own path
   already refuted — and decide.c then marks NEITHER arm primary rather than the wrong one. */
static JSValue conj_example(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue exa = concolic_example(ctx, a), exb = concolic_example(ctx, b), r = JS_UNDEFINED;

    if (!JS_IsUndefined(exa) && !JS_IsUndefined(exb)) {
        DCHECK(JS_IsBool(exa) && JS_IsBool(exb),
               "a conjunct carries a concrete example that is not a BOOLEAN — a predicate's example is the "
               "answer the comparison gave on the operands' own examples, so a third kind here is a value "
               "that was minted as a predicate by something that computed something else");
        r = JS_NewBool(ctx, JS_ToBool(ctx, exa) && JS_ToBool(ctx, exb));
    }
    JS_FreeValue(ctx, exa);
    JS_FreeValue(ctx, exb);
    return r;
}

/* …AND THE MINT OVER TWO PREDICATES — see concolic.h for why a component cannot answer this itself, why the
 * identity is the SET of conjuncts, and why a duplicate is deduplicated here where a joint provenance's is a
 * crash. Both operands are BORROWED; the result is OWNED. */
JSValue concolic_new_conj(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    Concolic *side[2], *c;
    const char **members, **sorted;
    int *order;
    int n = 0, u, i, k;
    char *ident;
    JSValue res;

    DCHECK(concolic_is(a) && concolic_is(b),
           "a conjunction was minted over an operand that is NOT unknown — a decided boolean is one the "
           "caller folds away by handing back the other operand, and composing a key over it would put a "
           "fork in the frontier over a question this engine has already answered by running it");

    side[0] = JS_GetOpaque(a, g_concolic_class);
    side[1] = JS_GetOpaque(b, g_concolic_class);
    /* THE MEMBERS, FLATTENED. A conjunct that is ITSELF a conjunction contributes its own members, so
       `(p ∧ q) ∧ r` and `p ∧ (q ∧ r)` are ONE set and ONE key: associativity is a property of `∧`, and a fold
       that could spell it two ways would let a flow decide one nesting and re-fork the other. */
    for (i = 0; i < 2; i++) {
        DCHECK(side[i] != NULL,
               "a conjunct passed concolic_is and carries no record — the class and its opaque are written "
               "together at the one mint every value goes through");
        DCHECK(side[i]->conj_n == 0 || (side[i]->conj_n >= 2 && side[i]->conj != NULL),
               "a value names a CONJUNCT COUNT that is not a set: a conjunction holds at least two distinct "
               "members and a list to hold them, because a one-member set IS its member and this mint hands "
               "that member back rather than composing a second key for it");
        n += side[i]->conj_n ? side[i]->conj_n : 1;
    }
    members = reclaim_malloc((size_t)n * sizeof *members);
    order   = reclaim_malloc((size_t)n * sizeof *order);
    CHECK(members && order, "concolic: OOM composing a conjunction's identity — a predicate whose key could "
                            "not be spelled would be decided by whatever constraint another value left under "
                            "the key it fell back to");
    for (i = 0, k = 0; i < 2; i++) {
        if (side[i]->conj_n) { int j; for (j = 0; j < side[i]->conj_n; j++) members[k++] = side[i]->conj[j]; }
        else members[k++] = side[i]->ident;
    }
    DCHECK(k == n, "a conjunction collected a different number of members than it measured for — the two "
                   "loops read the same two records one after the other, so a mismatch is a record mutated "
                   "between them");

    /* AN UNSPELLABLE MEMBER MAKES THE WHOLE CONJUNCTION UNSPELLABLE, which is the same POSITIVE statement the
       struct makes for every other absent identity: a value with no name is never decided from another
       value's record, so both arms of every branch over it stay. It costs forks; a wrong name costs an arm. */
    for (i = 0; i < n; i++)
        if (!members[i]) {
            free(members); free(order);
            return concolic_alloc(ctx, "{cmp}", NULL, NULL, NULL, conj_example(ctx, a, b));
        }

    ident_set_order(members, n, order);
    /* DEDUPLICATED, WHERE A JOINT PROVENANCE'S DUPLICATE IS A CRASH — concolic.h states which caller could
       have known. `order[u - 1]` is the last member KEPT and `order[i]` has not been written yet, because
       `u <= i` holds at every step. */
    for (i = 1, u = 1; i < n; i++)
        if (strcmp(members[order[u - 1]], members[order[i]]) != 0) order[u++] = order[i];
    DCHECK(u >= 1 && u <= n, "a conjunction's set came back with a member count outside the multiset it was "
                             "compacted from");
    n = u;

    if (n == 1) {
        /* `p ∧ p` IS `p`, so the answer is that predicate and not a second key naming it. Only two ATOMS can
           collapse this far: a conjunction operand already holds distinct members, so it contributes at least
           two and the set cannot be one. */
        DCHECK(side[0]->conj_n == 0 && side[1]->conj_n == 0,
               "a conjunction over a set that collapsed to ONE member had a conjunction as an operand — that "
               "operand's own members are distinct by construction, so this is a set that was stored without "
               "going through this mint's deduplication");
        free(members); free(order);
        return JS_DupValue(ctx, a);
    }

    sorted = reclaim_malloc((size_t)n * sizeof *sorted);
    CHECK(sorted, "concolic: OOM ordering a conjunction's conjuncts");
    for (i = 0; i < n; i++) sorted[i] = members[order[i]];
    ident = concolic_ident_compose(CONCOLIC_CONJ_TAG, sorted, n);
    DCHECK(ident != NULL, "a conjunction's identity came back absent over members this mint had already "
                          "established are all spellable — concolic_ident_compose answers NULL only for a "
                          "NULL member, and the scan above rejected every one of those");

    /* NOT A SOURCE READ (concolic_alloc, never concolic_derived): a conjunction is a boolean this engine
       COMPUTED, so an @S candidate substituted into it would answer a predicate with a payload. */
    res = concolic_alloc(ctx, "{cmp}", NULL, NULL, ident, conj_example(ctx, a, b));
    c = JS_GetOpaque(res, g_concolic_class);
    DCHECK(c != NULL, "a conjunction was minted as something that is not a concolic value");
    c->conj = reclaim_malloc((size_t)n * sizeof *c->conj);
    CHECK(c->conj, "concolic: OOM recording the conjuncts a predicate is the conjunction of");
    for (i = 0; i < n; i++) {
        c->conj[i] = strdup(sorted[i]);
        CHECK(c->conj[i], "concolic: OOM copying a conjunct's identity");
    }
    c->conj_n = n;
    free(members); free(order); free(sorted);
    return res;
}

int concolic_cmp(JSValueConst v, const char **psrc, ConcolicLit *pkind, const char **ptok) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->cmp_op == OPCMP_NONE) {
        if (pkind) *pkind = CONCOLIC_LIT_NONE;
        return OPCMP_NONE;
    }
    /* THE KIND IS ANSWERED WHEREVER THE TOKEN IS, and the assert is here rather than at the caller because
       this is the only place that can still see the value both were written on. A caller handed a spelling
       and no kind pins the spelling as a string — nine characters where the page wrote `undefined`. */
    DCHECK(c->cmp_tok == NULL || c->cmp_kind != (signed char)CONCOLIC_LIT_NONE,
           "a comparison result carries a pin token with no KIND — the two are written together at pred_new "
           "and asserted together there, so a value holding only the spelling is one minted somewhere that "
           "does not go through that mint, and its pin would concretize a source to the wrong type");
    if (psrc) *psrc = c->src;
    if (pkind) *pkind = (ConcolicLit)c->cmp_kind;
    if (ptok) *ptok = c->cmp_tok;
    return c->cmp_op;
}

/* WHICH EQUALITY THE PAGE WROTE — see concolic.h. It is a SECOND read rather than a fifth out-parameter on
   concolic_cmp for the reason pred_set_subject_ident is a second call: only the arm that PINS asks it, and a
   signature every caller has to answer would hand the three that do not a value they must then ignore. The
   refusal below is what keeps that safe — a caller that reaches this on a non-equality is not defaulted, it
   aborts naming the predicate it is standing on. */
JSConcolicEqOp concolic_cmp_algo(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;

    DCHECK(c != NULL && c->cmp_op != OPCMP_NONE,
           "the equality ALGORITHM was asked of a value that records no equality — an ordering and a call "
           "predicate have none, so the only answer available here would be a §7.2.13/§7.2.14 reading of a "
           "predicate neither defines, and the caller would pin off it");
    DCHECK(c != NULL && (c->cmp_algo == JS_CONCOLIC_EQ_LOOSE || c->cmp_algo == JS_CONCOLIC_EQ_STRICT),
           "an equality result carries an algorithm quickjs.h does not declare — pred_new writes it beside "
           "`cmp_op` and asserts the pair there, so a value holding one without the other is minted somewhere "
           "that does not go through that mint");
    /* AND THE RELEASE BUILD ANSWERS THE REFUSING ARM RATHER THAN DEREFERENCING, which is a POSITIVE statement
       and not the `|| default` that hides a producer's hole. Both asserts above vanish at APICLIENT_DEV=0, so
       without this the first of them stops guarding the pointer the line below reads — the promotion CLAUDE.md
       names, traded for a segfault. `LOOSE` is chosen because it is the arm that makes NO pin: a release build
       standing somewhere dev aborts loses one narrowing and the source forks again at its next gate, which is
       sound and is what this engine did before any pin existed. The other value would concretize a source off
       a record that could not say it had determined anything. */
    if (!c || c->cmp_op == OPCMP_NONE) return JS_CONCOLIC_EQ_LOOSE;
    return (JSConcolicEqOp)c->cmp_algo;
}

RelOp concolic_rel(JSValueConst v, const char **ptok, double *pnum, const char **psubj) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->rel_op == REL_NONE) return REL_NONE;
    DCHECK(c->rel_tok != NULL && c->rel_subj != NULL,
           "an ordering result carries a relation with no token or no subject — the three are one observation, "
           "written together at the mint, so a result holding only some of them is a bound whose own producer "
           "does not know what it is about");
    if (ptok)  *ptok  = c->rel_tok;
    if (pnum)  *pnum  = c->rel_num;
    if (psubj) *psubj = c->rel_subj;
    return c->rel_op;
}

int concolic_strpred(JSValueConst v, ConcolicCallPred *out) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;

    DCHECK(out != NULL, "a call predicate was asked for with nowhere to put it — a predicate returned only as "
                        "a yes/no is a constraint the caller cannot state");
    out->method = NULL; out->args = NULL; out->nargs = 0; out->subject = NULL;
    if (!c || !c->sp_meth) return 0;
    DCHECK(c->sp_subj != NULL,
           "a call result carries a method with no subject — the two are one observation, written together at "
           "the mint, so a result holding only one is a predicate whose own producer cannot say what it is "
           "about");
    DCHECK(c->sp_nargs == 0 || c->sp_args != NULL,
           "a call result carries an argument COUNT with no list — the mint writes both or neither, so a "
           "count alone is a predicate the emission would print with an argument it does not have");
    out->method = c->sp_meth;
    out->args = (const char *const *)c->sp_args;
    out->nargs = c->sp_nargs;
    out->subject = c->sp_subj;
    return 1;
}

const char *concolic_cmp_subject(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->cmp_op == OPCMP_NONE) return NULL;
    return c->cmp_subj;   /* NULL = the operand's shape names no hole this surface prints — see pred_new */
}

const char *concolic_cmp_subject_ident(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->cmp_subj_ident : NULL;
}

/* JSConcolicCmpHook for == / === : a concolic operand -> a concolic bool whose IDENTITY is the operator and
   both operands, and which additionally carries the PIN when one side is concrete. Matches the slow-eq stack
   effect (both operands freed, result in sp[-2]). is_neq flips EQ->NE.
   LOOSE AND STRICT EQUALITY ARRIVE HERE AS ONE OPERATOR, because the hook is handed only `is_neq` — so
   `x == 'a'` and `x === 'a'` still compose to one identity and either decides the other. That is the same
   defect this file just removed from the ordering hook, in the one place the host cannot see the distinction:
   it needs quickjs to say which of the two called, the way JS_CARITH_* already spares the solver the opcode
   numbers. */
/* IS THIS EXAMPLE ONE THE COMPARISON MAY BE RUN ON — asked of both sides before §7.2.13 or §7.2.14 is run on
   them, and the answer for an OBJECT or a SYMBOL is no, for one reason that covers both algorithms.
   §7.2.13 IsLooselyEqual step 12 converts an object with ToPrimitive, which runs the PAGE's own valueOf or
   toString, and naming that call's result here would be inventing it — the same refusal literal_tok already
   makes for the same operand. §7.2.14 IsStrictlyEqual runs no code for an object (SameType holds and
   SameValueNonNumber compares identity), so it would be SAFE and is still refused, because the question it
   would answer is the wrong one: an example object is a value THIS ENGINE minted to stand for an unknown, never
   the page's own object, so comparing its identity against anything reports a fact about an allocation the
   program never made. One rule for both, and the absence is a positive statement rather than a gap. */
static int example_comparable(JSValueConst v) {
    return !JS_IsObject(v) && !JS_IsSymbol(v);
}

/* THE COMPARISON'S OWN EXAMPLE — §Solver-half's "every op propagates the example" applied to the operator this
 * hook is for, and it propagates for the reason that section gives in its next sentence: "the example
 * propagates because the engine RUNS THE REAL OP on the concrete". So this runs the engine's own equality —
 * `JS_IsStrictEqual` is §7.2.14 and `JS_IsEqual` is §7.2.13 — over values the run already holds, and never a
 * re-implementation of either. A hand-rolled comparison here would be the second evaluator §Re-execution
 * forbids, and it would be wrong within a week: the two algorithms disagree at step 1 and only the engine's
 * own code knows every way they do.
 *
 * WHICH ALGORITHM IS THE CALLER'S TO STATE and arrives as `op` (quickjs.h's JSConcolicEqOp). The hook used to
 * be told only `is_neq`, which is why this could not exist: `"1" == 1` is true and `"1" === 1` is false, so
 * with the algorithm unnamed there is no answer to run.
 *
 * AN OPERAND WITH NO EXAMPLE YIELDS A RESULT WITH NO EXAMPLE — attacker input and server-injected absent state
 * are example-free by design, and a comparison over one determines nothing about which arm a real session
 * takes. Collapsing that to `false` would invent it, which is the §@H fabrication this whole triple exists to
 * refuse; decide.c reads the absence as REAL_ARM_UNOBSERVED and keeps both arms unmarked.
 *
 * A CONCRETE OPERAND ALWAYS HAS A VALUE, INCLUDING `undefined`, and that distinction is the whole of `hava`
 * and `havb`. JS_UNDEFINED is concolic.h's spelling of "this value carries no example"; it is ALSO an ordinary
 * ECMAScript value that a page compares against constantly (`if (cfg.admin === undefined)`). Reading the
 * second as the first would drop the example of every such gate — the commonest shape after a string literal
 * — so absence is asked of the CONCOLIC side only, where it is what the encoding means. */
static JSValue cmp_example(JSContext *ctx, JSValueConst a, JSValueConst b, int ca, int cb,
                           int is_neq, JSConcolicEqOp op) {
    JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    int hava = ca ? !JS_IsUndefined(exa) : 1;
    int havb = cb ? !JS_IsUndefined(exb) : 1;
    JSValue out = JS_UNDEFINED;

    if (hava && havb && example_comparable(exa) && example_comparable(exb)) {
        int eq;
        DCHECK(!concolic_is(exa) && !concolic_is(exb),
               "an example that is itself a concolic value reached the real comparison — `JS_IsEqual` would "
               "re-enter this very hook, and a value whose example is another unknown is a producer having "
               "attached a symbol where a concrete stands");
        if (op == JS_CONCOLIC_EQ_STRICT) {
            eq = JS_IsStrictEqual(ctx, exa, exb) ? 1 : 0;
        } else {
            DCHECK(op == JS_CONCOLIC_EQ_LOOSE,
                   "an equality reached the concolic derivation naming an algorithm quickjs.h does not "
                   "declare — 7.2.13 and 7.2.14 disagree, so an unnamed caller has no answer here");
            eq = JS_IsEqual(ctx, exa, exb);
            /* §7.2.13's only failure is an abrupt ToPrimitive, which the operand test above has already made
               unreachable: neither side is an object. A -1 here is that test and this call disagreeing about
               what the operands are, and the throw it left standing would surface in the page's next
               statement as a TypeError it never wrote. */
            CHECK(eq >= 0, "concolic ==: 7.2.13 IsLooselyEqual refused two operands that carry no object");
        }
        out = JS_NewBool(ctx, eq ^ (is_neq ? 1 : 0));
    }
    JS_FreeValue(ctx, exa);
    JS_FreeValue(ctx, exb);
    return out;
}

int concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq, JSConcolicEqOp op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    JSValueConst opq, other;
    const char *src, *root;
    char *tok = NULL, *iu, *io, *subj;
    ConcolicLit kind = CONCOLIC_LIT_NONE;
    JSValue res;

    if (!ca && !cb) return 0;
    opq = ca ? a : b; other = ca ? b : a;
    src = concolic_src_c(opq);
    root = concolic_root_c(opq);   /* THE SAME OPERAND, or the assert at concolic_alloc has two facts about two values */
    /* THE PIN TOKEN IS THE OTHER OPERAND SPELLED, AND ONLY AN OPERAND THIS ENGINE CAN SPELL HAS ONE — which is
       the same question literal_ident answers, asked through the same classification so the two cannot
       disagree. It read the operand with a bare JS_ToCString, which for an OBJECT is §7.1.19 ToString step 10
       -> ToPrimitive from C with no flow base, and for a Symbol is step 2's TypeError left pending on ctx.
       ABSENCE IS THE RIGHT ANSWER AND NOT A GAP. §7.2.14 IsStrictlyEqual step 1 returns false whenever
       SameType(x, y) is false, so no string this solver could substitute for the unknown is ever === a given
       object: there is nothing to pin to. §7.2.13 IsLooselyEqual step 12 converts the OBJECT with ToPrimitive,
       i.e. runs the page's own code, so naming the object here would be inventing that call's result — which
       §@H forbids in the same words it forbids inventing `6` for `x > 5`. The predicate still forks: the
       comparison result below carries the operator and both operands' identities, and only the PIN is absent. */
    /* THE KIND IS TAKEN FROM THE SAME CLASSIFICATION AND ON THE SAME LINE AS THE SPELLING, because it is the
       spelling's other half: §7.1.19 ToString flattens `undefined`, `null`, `0` and `false` onto text that is
       also a legal STRING operand, so a token travelling alone reaches decide.c as a string whatever the page
       compared against. `operand_kind` is what literal_tok already consults to decide there is a token at
       all, so asking it here cannot disagree with that answer. */
    if (!concolic_is(other)) { tok = literal_tok(ctx, other); kind = operand_kind(other); }
    /* EQUALITY IS SYMMETRIC, so `x === 'a'` and `'a' === x` compose to ONE identity: the unknown operand is
       written first, and where BOTH are unknown the two identities are ordered between themselves. Without
       that the same predicate written the other way round would be a second fact and fork a second time —
       which costs work but is sound; the ordering is what makes the sound answer also the cheap one. */
    iu = ident_of_operand(ctx, opq);
    io = ident_of_operand(ctx, other);
    /* THE TWO SPELLINGS OF ONE OPERAND AGREE IN THE ONE DIRECTION THEY STILL MUST, asserted where they meet.
       Only a CONCRETE other operand is spelled at all; a concolic one carries its own identity and pins
       nothing. A TOKEN WITH NO IDENTITY is the defect it always was: a flow that PINS its source to a value no
       predicate key mentions, where the pin outlives the branch and every later read of that source computes
       it.
       IT IS AN IMPLICATION AND NOT A BICONDITIONAL, AND NAMING INTRINSICS IS WHAT RETIRED THE OTHER HALF. AN
       IDENTITY WITH NO TOKEN read as "the arm that was silently dropped when the classifications diverged"
       only while literal_ident composed its answer OUT OF the token; it now has a second name source — an
       intrinsic's registry slot, which is not a spelled VALUE — so `n !== Object.prototype` legitimately names
       its predicate and pins nothing. §7.2.14 IsStrictlyEqual step 1 is why the absent pin is correct rather
       than lost: SameType(x, y) is false for every string this solver could substitute, so no object operand
       ever had anything to pin the unknown TO. */
    DCHECK(concolic_is(other) ? tok == NULL : (tok == NULL || io != NULL),
           "an equality's other operand was spelled as a pin token with NO predicate identity beside it — the "
           "two come from one classification, so a token the identity speller cannot name is a pin filed under "
           "a key no branch over that source will ever look up");
    if (ca && cb && iu && io && strcmp(iu, io) > 0) { char *t = iu; iu = io; io = t; }
    /* THE HOLE THE REPORT WILL PRINT FOR THIS OPERAND — taken from the SHAPE and not from `src`, which for
       every derived value is the braced shape itself and would be looked up under a name the emission never
       spells. Minted only where there is a token, because the subject and the token are one observation. */
    subj = tok ? concolic_hole_key(concolic_shape_c(opq)) : NULL;
    /* THE OPERATOR IN THE IDENTITY IS THE ONE THE PAGE WROTE, ALL FOUR OF THEM, and this spelled two. It was
       `is_neq ? "!=" : "=="` for BOTH algorithms, so `x == '1'` and `x === '1'` composed to ONE constraint key
       — two different predicates over one operand, and §7.2.13 and §7.2.14 answer differently for exactly the
       pairs a minified bundle writes `==` for. A flow that decided either then DECIDED the other through
       decide.c's feasible refinement, pruning an arm nothing had contradicted, which is the soundness bug this
       file already fixed once for the relational operators ("`x < 700` and `x > 700` were one fact too"). The
       algorithm is now stated by the caller, so the identity can say which. */
    /* …AND THE ALGORITHM TRAVELS WITH THE ARM, because the arm alone does not say what it PROVED. `cmp_op`
       records which arm makes the equality HOLD, which is the same fact for both algorithms; what differs is
       the conclusion the holding arm licenses, and only §7.2.13 IsLooselyEqual ( x, y ) versus §7.2.14
       IsStrictlyEqual ( x, y ) decides that. This line used to hand decide.c the arm and nothing else, so a
       held `x == undefined` pinned the source exactly as a held `x === undefined` did — while §7.2.13 steps 2
       and 3 ("If x is null and y is undefined, return true" and its converse) leave the operand as either of
       TWO values. The pin that followed picked one of them and concretize-on-pin then decided every later
       branch over that source from the choice. See concolic.h's concolic_pin for the full reading. */
    res = pred_new(ctx, cmp_op_ident(is_neq, op), src, root, iu, io,
                   tok ? (is_neq ? OPCMP_NE : OPCMP_EQ) : OPCMP_NONE,
                   tok ? (int)op : CMP_ALGO_NONE, kind, tok, subj);
    /* AND WHICH VALUE THE PREDICATE IS ABOUT, BY ITS OWN IDENTITY — read off `opq` BEFORE any reordering, and
       under exactly the condition the hole is minted under (`subj`, which is `tok`, which is "one side is a
       spellable literal"). `iu` above is NOT this: it is swapped with `io` when both operands are unknown, so
       reading the subject out of it would name whichever identity happened to sort first. */
    /* AN OPERAND THIS ENGINE CANNOT SPELL GETS NO SUBJECT IDENTITY, and the predicate is unaffected in every
       other way: it still forks, still pins and still excludes under the hole. What is lost is only the
       per-flow degrade, which has nowhere to be filed — the same answer, for the same reason, that
       decide.c gives for a condition whose own identity is absent. */
    if (subj && concolic_ident_c(opq)) pred_set_subject_ident(res, concolic_ident_c(opq));
    /* …AND THE RESULT'S OWN EXAMPLE, run rather than derived. It is attached AFTER the mint for the reason
       page_visibility.c attaches one: pred_new's parameters are the PREDICATE (its operator, operands, pin and
       subject), and the example is not part of the predicate — it is what this run computed the predicate to
       be, which is a different fact about the same value. */
    concolic_set_example(ctx, res, cmp_example(ctx, a, b, ca, cb, is_neq, op));
    free(subj);
    free(tok);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    sp[-2] = res;
    return 1;
}
/* CALLING an unknown yields an unknown. `document.cookie.indexOf("role=admin")` reads a field off a concolic —
   which concolic_exotic_get already answers with another concolic — and then CALLS it. A concolic that is not
   callable throws "not a function" there, and the throw takes the whole program with it: every statement after
   the first method call on an unknown string is unreachable, which silently truncated exploration at exactly
   the checks that gate a session. The result is concolic because that is what it IS: nothing here knows what
   indexOf would return over a cookie jar this engine was never given, so the comparison after it FORKS instead
   of collapsing. The ARGUMENTS still ran — they are the page's own expressions and their effects have already
   happened by the time this is reached. */
static JSValue concolic_call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                             int argc, JSValueConst *argv, int flags)
{
    Concolic *c = JS_GetOpaque(func_obj, g_concolic_class);
    char *shape, *ident, **owned;
    const char **f;
    JSValue r;
    int i;

    DCHECK(c != NULL, "the concolic call handler ran on something that is not a concolic value");
    /* `this_val` IS THE RECEIVER, AND THIS IS WHAT MAKES THAT TRUE. quickjs.h's JSClassDef states that under
       JS_CALL_FLAG_CONSTRUCTOR this parameter is new.target instead, and that a constructor call happens only
       where the object's constructor bit is set — which this class never sets. So the invariant holds by
       construction and is asserted rather than branched on: a guard that silently declined the constructor
       case would be a narrowing nobody could find, while the day something sets the bit this names the exact
       line that has to learn the difference. */
    DCHECK(!(flags & JS_CALL_FLAG_CONSTRUCTOR),
           "a concolic value was called as a CONSTRUCTOR — this class never sets the constructor bit, so "
           "`this_val` here is the receiver and nothing else, and a new.target arriving in that slot would "
           "be filed as the object a page's own predicate tested");
    /* THE ARGUMENTS ARE PART OF THE CALL'S IDENTITY, AND THEREFORE PART OF ITS SHAPE. `x.startsWith("/api")`
       and `x.startsWith("javascript:")` are two gates over one source, and a call identity that dropped its
       arguments made the second DECIDED by the first — the same collapse the relational operator's missing
       operand made, reached by another route.
       THE SHAPE DROPPED THEM FOR LONGER THAN THE IDENTITY DID, AND THAT WAS THE SAME DEFECT ONE FIELD OVER.
       shapef's own banner already states the rule this line now obeys — a shape "is ALSO the field path an @S
       candidate is injected at", so two derivations sharing one shape share one injection address — and it was
       written there about TRUNCATION, which was fixed. This was the identical loss by OMISSION: `h.slice(1)`
       and `h.slice(2)` composed two identities and ONE shape `{location.hash}.slice()`, so keyname_record's
       assert fired the moment both were used as property keys, and with the assert compiled out the two
       distinct unknown keys bought ONE atom and landed in ONE slot on the page's own object. Reproduced at
       origin/main 9c757178 in four lines, and the same document answered `Object.keys(o).length === 1`.
       IT IS ALSO THE HONEST DISPLAY, which is why this is a repair rather than a disambiguator bolted on:
       §@H asks for "the expression the run actually built", and the page wrote the argument.
       An argument this engine cannot spell (a plain object) makes the whole identity absent, so both arms of
       every branch over the result stay — and it renders "?", which is the one pair this rule cannot separate;
       see the NAMED RESIDUAL at keyname_record. */
    {
        char *args = NULL;
        for (i = 0; i < argc; i++) {
            char *a = derived_operand_shape(ctx, argv[i]);
            char *next = args ? shapef("%s, %s", args, a) : shapef("%s", a);
            free(a);
            free(args);
            args = next;
        }
        shape = shapef("%s(%s)", c->shape ? c->shape : "{}", args ? args : "");
        free(args);
    }
    owned = reclaim_malloc((size_t)(argc + 1) * sizeof *owned);
    f = reclaim_malloc((size_t)(argc + 2) * sizeof *f);
    CHECK(owned && f, "concolic: OOM identifying a call over an unknown value");
    f[0] = c->ident;
    for (i = 0; i < argc; i++) { owned[i] = ident_of_operand(ctx, argv[i]); f[i + 1] = owned[i]; }
    ident = concolic_ident_compose("()", f, argc + 1);
    for (i = 0; i < argc; i++) free(owned[i]);
    free(owned); free(f);
    /* The RESULT of calling an unknown is a new injection identity — nothing here knows what the call returned,
       so a candidate substitutes at the call — and it is NOT new bytes: whatever carried the callee's bytes in
       carried these. This is the derivation the reported defect walked through (`{location.hash}.slice()`). */
    r = concolic_derived(ctx, shape, shape, c->root, ident, JS_UNDEFINED);
    /* AND THE PREDICATE THE PAGE JUST WROTE, IF THIS RESULT CAN NAME ONE. `if (path.startsWith("/api"))` is
       a gate over `path` exactly as `path === "/api"` is; it determines no VALUE, so it pins nothing and
       carries no bound, and until this line it carried nothing at all — a parameter a prefix check gated
       rendered with the same bytes as one nothing had ever tested, which §Solver-half calls a wrong report
       rather than a partial one. (The `!`-spelled gate reaches no branch at all, for a reason one opcode
       upstream of this file — concolic.h's residual at concolic_strpred names it.)
       The three facts that make it nameable are asked for here and each absence is
       a positive statement: no `member` on the callee means the page called the unknown ITSELF rather than a
       method of it (`x("/api")`, whose receiver is undefined and about which nothing is claimed); a receiver
       that is not an unknown, or whose shape is the unnameable `{}`, has no hole a report could print it
       under; and an argument this engine cannot spell — a plain object, a function — makes the whole
       predicate absent, exactly as an unspellable member makes an identity absent, because a claim missing
       one of its operands is a different claim.
       IT DOES NOT LOOK AT WHAT THE METHOD IS. Nothing here knows or asks what `startsWith` means, and no
       consumer of the record re-implements one: what travels is the name the page wrote, the arguments it
       passed and the answer the run got. That is the line between this and the recorded transform-expression
       §Re-execution forbids — a transform is a rule for COMPUTING a value, and this is a predicate that was
       EVALUATED. */
    /* A CANDIDATE RE-FIRE IS ASKED FIRST, and it is the reason this tests `r` and not just its operands.
       concolic_derived hands back the attacker's PAYLOAD — a real String — where this call's identity is the
       source under substitution, so the value there is not a concolic and has no predicate to carry. Stamping
       one would DCHECK on a NULL opaque during exactly the run @S exists to perform. It is also the right
       answer rather than a guard: the domain is what an @H record states about a parameter, and a verify run
       is not building one (endpoint.c suppresses its requests wholesale for the same reason). */
    if (concolic_is(r) && c->member && concolic_is(this_val)) {
        char *subj = concolic_hole_key(concolic_shape_c(this_val));
        if (subj) {
            char **spelled = NULL;
            int ok = 1;
            if (argc) {
                spelled = reclaim_calloc((size_t)argc, sizeof *spelled);
                CHECK(spelled, "concolic: OOM spelling the arguments a gate passed over an unknown");
                for (i = 0; i < argc; i++) {
                    spelled[i] = literal_tok(ctx, argv[i]);
                    if (!spelled[i]) { ok = 0; break; }
                }
            }
            if (ok) pred_set_strpred(r, c->member, spelled, argc, subj);   /* consumes `spelled` */
            else ident_list_free(spelled, argc);
            free(subj);
        }
    }
    free(shape);
    return r;
}

/* ORDERING over an unknown is unknown. < <= > >= coerce with ToPrimitive, which a concolic cannot satisfy —
   it is an object whose coercion answers with another concolic — so the operator threw TypeError and took the
   whole program with it: `document.cookie.indexOf("role=admin") >= 0` explored NEITHER arm, losing both the
   session path and the anonymous one. The result is a concolic BOOL, so the branch forks exactly as an equality
   gate does; it carries no {op,tok} constraint because an ordering does not PIN a value the way `=== 'admin'`
   does — it narrows a domain, and the arm that is taken says which way.
   IT CARRIES ITS OPERATOR AND BOTH OPERANDS ALL THE SAME, and dropping them was a soundness bug and not a
   simplification. The equality hook above composed `{src, op, tok}`; this one composed nothing but the
   operand's SOURCE, so every ordering over one source was one predicate: in one flow `parseInt(gCS(a).width)
   < 700` and `parseInt(gCS(b).width) < 300` produced the same key and the second was DECIDED by the first,
   pruning an arm the flow's constraint does not contradict. `x < 700` and `x > 700` were one fact too.
   ORDERING IS NOT SYMMETRIC, so the operands keep the order the program wrote them: `x < 700` and `700 < x`
   are two predicates and neither may answer the other.
   AND IT NOW STATES ITS BOUND, which is the other half of §Solver-half's two-facts rule and the half the
   identity above cannot carry. An identity only tells predicates APART; a report has to SAY what one requires.
   `rel169` is a fact this run observed and could not print, so a parameter gated by `> 5` rendered with the
   same bytes as one nothing had ever tested — the exact asymmetry that made an equality's false arm a wrong
   report rather than a partial one, arriving on the second-most-frequent gate class in a real bundle.
   IT INVENTS NO VALUE. What is recorded is the relation the page wrote, the literal the page wrote, and the
   arm this run took; §@H's "never invent `6` for `x > 5`" is about the VALUE, and no value is determined here
   — the emission stays a domain-annotated shape. */
int concolic_rel_hook(JSContext *ctx, JSValue *sp, int op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    char opid[24];
    RelOp rel;
    int w;

    if (!ca && !cb) return 0;
    /* THE OPERATOR ARRIVES AS THE ENGINE'S OWN OPCODE FOR IT (see the hook's contract in quickjs.h), which is
       an identity and not a name. The IDENTITY keeps the raw opcode — the four operators need only stay TOLD
       APART for the constraint key, and re-spelling them there would put a second encoding beside this file's
       one. The NAME is asked of solver/rel_op.h, which reads it off the engine's own opcode table.
       IT IS ASKED HERE, ON EVERY ORDERING, and not down beside the one branch that uses it: rel_op_of_opcode
       is what catches the host's rebuild of the opcode table drifting from quickjs.c's, and an assert placed
       under the numeric-operand test would only fire on a bundle that happens to compare against a literal —
       so a drifted build could run a whole document naming no relation and reporting nothing wrong. */
    rel = rel_op_of_opcode(op);
    w = snprintf(opid, sizeof opid, "rel%d", op);
    DCHECK(w > 0 && (size_t)w < sizeof opid, "a relational operator's id did not fit its own buffer");
    (void)w;
    {
        JSValueConst opq = ca ? a : b, other = ca ? b : a;
        JSValue res = pred_new(ctx, opid, concolic_src_c(opq), concolic_root_c(opq),
                               ident_of_operand(ctx, a), ident_of_operand(ctx, b),
                               OPCMP_NONE, CMP_ALGO_NONE, CONCOLIC_LIT_NONE, NULL, NULL);
        /* A BOUND EXISTS ONLY WHERE THE RELATION'S OWN MEANING IS DETERMINED, and §7.2.12 IsLessThan is what
           decides that. Step 3 takes the STRING comparison when px and py are BOTH Strings and the numeric
           one otherwise, so a concrete side that is a Number puts the comparison on the numeric path whatever
           the unknown turns out to be — and a concrete side that is a String leaves the page performing
           either a lexicographic or a numeric comparison depending on a value this engine does not have.
           Stating a bound there would be naming which comparison the page ran, which is the same invention as
           naming a value, so it is left absent.
           NON-FINITE IS THE SAME ANSWER FOR A DIFFERENT REASON: `x < Infinity` narrows nothing, `x < NaN` is
           false for every x (§7.2.12 answers undefined and §13.10.1 turns that into false), and neither
           Infinity nor NaN is a JSON number a report could carry. */
        /* Where neither holds, the predicate still forks and only the BOUND is absent. */
        if (!concolic_is(other) && JS_IsNumber(other)) {
            double num = 0;
            int gotn = JS_ToFloat64(ctx, &num, other);
            DCHECK(gotn == 0, "a Number operand did not convert to a double — §7.1.4 ToNumber over a value "
                              "JS_IsNumber has already answered for cannot fail, so this is the tag test and "
                              "the conversion disagreeing about what the operand is");
            if (gotn == 0 && isfinite(num)) {
                char *subj = concolic_hole_key(concolic_shape_c(opq));
                char *txt = subj ? literal_tok(ctx, other) : NULL;
                /* NORMALISED SUBJECT-LEFT EXACTLY ONCE. `700 < x` is `x > 700`; recording the relation as
                   written would make every consumer downstream re-derive which operand the page put first,
                   and the first one to forget reports the bound inverted. */
                if (txt) pred_set_bound(res, ca ? rel : rel_op_mirror(rel), txt, num, subj);
                free(txt);
                free(subj);
            }
        }
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        sp[-2] = res;
    }
    return 1;
}

/* `typeof` an unknown is UNKNOWN. A concolic is a real object of a host class, and that class is callable so
   `document.cookie.indexOf(...)` works — which made typeof report "function" and a bundle testing
   `typeof x === "function"` take an arm decided by the solver's representation rather than the value. The
   answer is a concolic STRING carrying the same source, so the comparison after it forks like every other gate
   over an unknown. */
static char *cstr_dup(JSContext *ctx, JSValueConst v);   /* defined with the + hook below */

/* §7.1.4 ToNumber ( arg ) AND §7.1.19 ToString ( arg ) OVER UNKNOWN INPUT — answered where the operator
 * computes, never at the conversion boundary, which owes C a real primitive.
 *
 * The RESULT keeps the SOURCE, so `-location.hash` still forks a later branch and still solves at a later sink;
 * a value that collapsed to NaN or "[object Object]" here would end the exploration at the first coercion, which
 * is exactly what §Solver means by opacity surviving coercion.
 *
 * The EXAMPLE propagates by RUNNING THE REAL OP on the operands' examples — the engine actually negates, actually
 * multiplies — which is what makes this the concolic TRIPLE rather than a taint label with a derived note. When
 * an operand has no example there is nothing concrete to run and the result is example-free, which is the honest
 * answer: @H never invents. */
static const char *carith_name(int op, int *unary)
{
    *unary = (op <= JS_CARITH_DEC);
    switch (op) {
    case JS_CARITH_NEG:  return "-";
    case JS_CARITH_PLUS: return "+";
    case JS_CARITH_NOT:  return "~";
    case JS_CARITH_INC:  return "++";
    case JS_CARITH_DEC:  return "--";
    case JS_CARITH_SUB:  return "-";
    case JS_CARITH_MUL:  return "*";
    case JS_CARITH_DIV:  return "/";
    case JS_CARITH_MOD:  return "%";
    case JS_CARITH_POW:  return "**";
    case JS_CARITH_AND:  return "&";
    case JS_CARITH_OR:   return "|";
    case JS_CARITH_XOR:  return "^";
    case JS_CARITH_SHL:  return "<<";
    case JS_CARITH_SAR:  return ">>";
    case JS_CARITH_SHR:  return ">>>";
    default: DFAIL("concolic arithmetic with an unknown operator id"); return "?";
    }
}

/* WHICH OF THE TWO REAL OPERATIONS THIS OPERATOR IS. §6.1.6.1's numeric methods split cleanly in two: most
   compute over the Number itself, and seven begin by narrowing their operands to 32 bits — §6.1.6.1.2
   Number::bitwiseNOT ( number ), §6.1.6.1.9 Number::leftShift ( x, y ), §6.1.6.1.10 Number::signedRightShift
   ( x, y ), §6.1.6.1.11 Number::unsignedRightShift ( x, y ) and §6.1.6.1.16 NumberBitwiseOp ( op, x, y ). A
   double is the wrong carrier for the second group: §7.1.8 ToInt32 ( arg ) is "Let int be ? ToIntegerOrInfinity
   (arg). Return 𝔽(ToFixedSizeInteger(int, signed, 32))", which is defined for every Number including the
   infinities, while a C cast of a double outside the int32 range is undefined behaviour. So the engine's OWN
   §7.1.8/§7.1.9 run on the example and the result is an exact integer JSValue. */
static int carith_is_32bit(int op)
{
    return op == JS_CARITH_NOT || (op >= JS_CARITH_AND && op <= JS_CARITH_SHR);
}

static int carith_apply(int op, double a, double b, double *out)
{
    DCHECK(!carith_is_32bit(op),
           "a 32-bit operator reached the double arithmetic — §6.1.6.1.2/.9/.10/.11/.16 begin with ToInt32 or "
           "ToUint32, so casting their operands from a double is both wrong at the edges and undefined "
           "behaviour outside the int32 range; carith_apply32 is the one that runs them");
    switch (op) {
    case JS_CARITH_NEG:  *out = -a; return 1;
    case JS_CARITH_PLUS: *out = a; return 1;
    case JS_CARITH_INC:  *out = a + 1; return 1;
    case JS_CARITH_DEC:  *out = a - 1; return 1;
    case JS_CARITH_SUB:  *out = a - b; return 1;
    case JS_CARITH_MUL:  *out = a * b; return 1;
    case JS_CARITH_DIV:  *out = a / b; return 1;
    case JS_CARITH_MOD:  *out = fmod(a, b); return 1;
    case JS_CARITH_POW:  *out = pow(a, b); return 1;
    default: return 0;
    }
}

/* THE SEVEN 32-BIT OPERATIONS, run on the operands' EXAMPLES by the engine's own conversions. `exa`/`exb` are
   borrowed; `*out` is a new owned JSValue on success. Returns 0 when a conversion threw — an example that
   cannot convert produces NO example, which is the honest answer rather than a fabricated number. */
static int carith_apply32(JSContext *ctx, int op, JSValueConst exa, JSValueConst exb, JSValue *out)
{
    int32_t ia = 0, ib = 0;
    uint32_t ua = 0, ub = 0, sc;

    DCHECK(carith_is_32bit(op), "carith_apply32 reached with an operator §6.1.6.1 computes over the Number");
    DCHECK(op != JS_CARITH_NOT || JS_IsUndefined(exb),
           "§6.1.6.1.2 Number::bitwiseNOT takes ONE argument — a second operand here means the interpreter "
           "pushed a binary window for a unary operator");

    if (op == JS_CARITH_SHR) {
        /* §6.1.6.1.11: "Let leftNumber be ! ToUint32(x)." — the ONE operator whose left side is unsigned, which
           is why >>> carries its own id instead of sharing >>'s. */
        if (JS_ToUint32(ctx, &ua, exa) || JS_ToUint32(ctx, &ub, exb)) return 0;
        sc = ub % 32;   /* §6.1.6.1.11: "Let shiftCount be ℝ(rightNumber) modulo 32." */
        *out = JS_NewUint32(ctx, ua >> sc);
        return 1;
    }
    if (JS_ToInt32(ctx, &ia, exa)) return 0;
    if (op == JS_CARITH_NOT) {
        /* §6.1.6.1.2: "Let oldValue be ! ToInt32(number). Return the bitwise complement of oldValue." */
        *out = JS_NewInt32(ctx, ~ia);
        return 1;
    }
    if (op == JS_CARITH_SHL || op == JS_CARITH_SAR) {
        /* §6.1.6.1.9/.10: ToInt32 on the left, ToUint32 on the RIGHT, shiftCount modulo 32. */
        if (JS_ToUint32(ctx, &ub, exb)) return 0;
        sc = ub % 32;   /* §6.1.6.1.9/.10: "Let shiftCount be ℝ(rightNumber) modulo 32." */
        /* The left shift is performed on the unsigned bit string §6.1.6.1.9 names, so a negative left operand
           and an overflow are both defined here rather than undefined in C. */
        *out = JS_NewInt32(ctx, op == JS_CARITH_SHL ? (int32_t)((uint32_t)ia << sc) : (ia >> sc));
        return 1;
    }
    /* §6.1.6.1.16 NumberBitwiseOp: ToInt32 on both, then the bit string operation. */
    if (JS_ToInt32(ctx, &ib, exb)) return 0;
    *out = JS_NewInt32(ctx, op == JS_CARITH_AND ? (ia & ib)
                          : op == JS_CARITH_OR  ? (ia | ib)
                                                : (ia ^ ib));
    return 1;
}

int concolic_arith_hook(JSContext *ctx, JSValue *sp, int op, int nops) {
    JSValue a = sp[-nops], b = nops == 2 ? sp[-1] : JS_UNDEFINED;
    int ca = concolic_is(a), cb = nops == 2 && concolic_is(b);
    const char *name;
    int unary = 0;
    char *shape, *ident;
    const char *src, *root;
    JSValue example = JS_UNDEFINED, res;

    if (!ca && !cb) return 0;
    DCHECK(nops == 1 || nops == 2,
           "a concolic arithmetic hook was handed an arity no ECMAScript operator has — the operand window is "
           "sp[-nops..sp[-1]] and the result is written to sp[-nops], so a third arity has no stack shape");
    name = carith_name(op, &unary);
    DCHECK(unary == (nops == 1),
           "the operator's declared arity disagrees with the operand count the interpreter pushed — the two "
           "are one fact (JS_CARITH_*'s unary ids come first) and the identity composition reads the arity "
           "from carith_name while the stack effect reads it from nops");
    src = ca ? concolic_src_c(a) : concolic_src_c(b);
    root = ca ? concolic_root_c(a) : concolic_root_c(b);

    /* THE OPERATOR AND ITS OPERANDS ARE THE IDENTITY, and the ARITY is part of it too: `carith_name` spells
       negation and subtraction both `-`, and a one-member composition can never write the same bytes as a
       two-member one. Without this `x` and `x*2` were one fact and `if (x) … if (x*2)` decided each other. */
    if (unary) {
        const char *f[1];
        char *ia = ident_of_operand(ctx, a);
        shape = shapef("%s%s", name, concolic_shape_c(a) ? concolic_shape_c(a) : "{}");
        f[0] = ia;
        ident = concolic_ident_compose(name, f, 1);
        free(ia);
    } else {
        const char *f[2];
        char *ia = ident_of_operand(ctx, a), *ib = ident_of_operand(ctx, b);
        /* derived_operand_shape AND NOT A SECOND SPELLING OF IT. This was `ca ? shape(a) : cstr_dup(ctx, a)`,
           which is that function hand-rolled, minus the KIND and plus a hazard: cstr_dup is an unconditional
           JS_ToCString, so `x * obj` sent §7.1.19 ToString step 10 to ToPrimitive and ran the page's own
           valueOf from a C activation with no flow base under it — the same call operand_kind's own banner
           records taking four real bundles' engine instances down at concolic_cmp_hook. Routing to the one
           speller fixes both at once: `x * 2` and `x * "2"` are two identities and are now two shapes, and an
           operand this engine refuses to spell renders `?` instead of running the page's code. */
        char *sa = derived_operand_shape(ctx, a);
        char *sb = derived_operand_shape(ctx, b);
        shape = shapef("%s%s%s", sa, name, sb);
        free(sa); free(sb);
        f[0] = ia; f[1] = ib;
        ident = concolic_ident_compose(name, f, 2);
        free(ia); free(ib);
    }

    /* RUN THE REAL OP on the examples when there are any — the concrete half of the triple. */
    {
        JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
        JSValue exb = nops == 2 ? (cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b)) : JS_UNDEFINED;

        /* AN EXAMPLE IS A PRIMITIVE. Every conversion below is §7.1.4 ToNumber, whose object case is
           "Let primitiveValue be ? ToPrimitive(arg, number)" — running the page's valueOf from this C frame,
           which has no flow base under it. A source or a derivation that attached an OBJECT example put a value
           here that only crashes, so the assert names the producer rather than the coercion. */
        DCHECK(!JS_IsObject(exa) && !JS_IsObject(exb),
               "a concolic carries an OBJECT as its concrete example — an example is the value the engine "
               "COMPUTED, so it is a primitive; ToNumber over an object one reaches ToPrimitive from C");

        if (!JS_IsUndefined(exa) && (nops == 1 || !JS_IsUndefined(exb))) {
            if (carith_is_32bit(op)) {
                JSValue r32 = JS_UNDEFINED;
                if (carith_apply32(ctx, op, exa, exb, &r32))
                    example = r32;
            } else {
                double da = 0, db = 0, out = 0;
                int oka = JS_ToFloat64(ctx, &da, exa) == 0;
                int okb = oka && (nops == 1 || JS_ToFloat64(ctx, &db, exb) == 0);
                DCHECK(oka && okb,
                       "a concolic's example could not be converted by §7.1.4 ToNumber — its steps 2 and 3 "
                       "throw only for a Symbol or a BigInt, and no source in this engine mints either as an "
                       "example, so the producer attached a value it never computed");
                if (oka && okb && carith_apply(op, da, db, &out))
                    example = JS_NewFloat64(ctx, out);
            }
            /* A CONVERSION THAT THREW LEFT ITS EXCEPTION STANDING IN THE CONTEXT, and the operator is about to
               return 1 — a success — so the page's very next statement would have thrown a TypeError it never
               wrote. The example is dropped (@H never invents) and the throw is dropped with it, because the
               throw belongs to a coercion the PROGRAM did not perform. */
            if (JS_IsUndefined(example) && JS_HasException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);
    }

    res = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, example);
    free(shape);
    JS_FreeValue(ctx, sp[-nops]);
    if (nops == 2) JS_FreeValue(ctx, sp[-1]);
    sp[-nops] = res;
    return 1;
}

/* §7.1.19 ToString ( arg ) over unknown input: unknown, source kept, example computed by actually stringifying
   the example when there is one.
   IT IS THE ANSWER FOR 22.1.1.1 String ( value ) AND FOR NOTHING ELSE, which is what the shape says: `String(x)`
   is the algorithm whose RESULT is the coercion. A builtin that merely CONSUMES the string derives its own
   result instead, named by its own operation — concolic_builtin_hook, reached from the engine's one shared
   ToString sub-sequence (JS_STEP_UNKNOWN). Two derivations, not two spellings of one. */
JSValue concolic_tostr_hook(JSContext *ctx, JSValueConst v) {
    const char *src, *root, *sh, *f[1];
    char *shape, *ident;
    JSValue ex, example = JS_UNDEFINED, r;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    src = concolic_src_c(v);
    root = concolic_root_c(v);
    sh = concolic_shape_c(v);
    shape = shapef("String(%s)", sh ? sh : "{}");
    f[0] = concolic_ident_c(v);
    ident = concolic_ident_compose("String", f, 1);
    ex = concolic_example(ctx, v);
    if (!JS_IsUndefined(ex)) {
        const char *p = JS_ToCString(ctx, ex);
        if (p) { example = JS_NewString(ctx, p); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, ex);
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, example);
    free(shape);
    return r;
}

/* §7.1.2 ToBoolean ( arg ) OVER UNKNOWN INPUT, and §13.5.7 Logical NOT Operator ( ! ) with it — see the hook's
 * contract in quickjs.h for why the operator answers at all.
 *
 * THE WHOLE OF THE DESIGN IS THAT THE RESULT IS A NEW VALUE WITH AN OLD PREDICATE. It is a new VALUE because a
 * program holds it (`var q = !p`), passes it, concatenates it — so it needs an identity of its own or `"" + !p`
 * and `"" + p` would compose to one derivation. It is an old PREDICATE because a branch over it asks exactly
 * the question a branch over the operand asks: `if (!p)` is `if (p)` with the arms swapped, and §7.1.2 is
 * applied on both sides alike. Those two facts are the two fields — `ident` for derivations, `br_key`/`br_neg`
 * for the branch — and keeping them apart is what buys the feasible-refinement a fresh `("!", p)` constraint
 * key would have thrown away: with one key, a flow that decided `p` DECIDES `!p` instead of forking a second
 * time over one predicate and then standing on two arms that contradict each other.
 *
 * `!!x` COMPOSES BACK, which is the test that the polarity is a parity and not a wrapper. The inner call gives
 * `br_key = ident(x), br_neg = 1`; the outer reads those and gives `br_key = ident(x), br_neg = 0`. So
 * `if (!!x)`, `if (Boolean(x))` and `if (x)` are ONE constraint entry, which is what they are in the program.
 *
 * THE EXAMPLE IS THE REAL OPERATION RUN ON THE OPERAND'S — §Solver-half's only sound propagation. §7.1.2 has
 * no ToPrimitive step, so running it re-enters nothing: an object is true, a string is its length. A value
 * carrying no example produces none, which is the positive statement decide_real_arm reads as "nothing this
 * run observed says which arm is real".
 *
 * IT MINTS THROUGH concolic_alloc AND NOT concolic_derived, for pred_new's reason exactly: this is a BOOLEAN
 * the operator computed, so an @S candidate substitution here would answer a predicate with the attacker's
 * payload string. */
JSValue concolic_tobool_hook(JSContext *ctx, JSValueConst v, int negate) {
    const char *src, *root, *sh, *bkey, *f[2];
    char *shape, *ident;
    Concolic *rc;
    JSValue ex, example = JS_UNDEFINED, r;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    DCHECK(negate == 0 || negate == 1,
           "§7.1.2 ToBoolean was asked for with a polarity that is neither the value nor its complement — the "
           "parameter is one bit and a third value would compose an identity no operator produces");
    src  = concolic_src_c(v);
    root = concolic_root_c(v);
    sh   = concolic_shape_c(v);
    shape = shapef(negate ? "!%s" : "Boolean(%s)", sh ? sh : "{}");
    /* THE POLARITY IS IN THE IDENTITY because `!p` and `Boolean(p)` are OPPOSITE values and a derivation off
       one must never compose to the other's key. It is NOT in the branch key, which is the same predicate for
       both — the two fields disagree here on purpose, and that disagreement is the mechanism. */
    f[0] = negate ? "!" : "ToBoolean"; f[1] = concolic_ident_c(v);
    ident = concolic_ident_compose("tb", f, 2);
    ex = concolic_example(ctx, v);
    if (!JS_IsUndefined(ex)) {
        int b = JS_ToBool(ctx, ex);
        DCHECK(b == 0 || b == 1,
               "§7.1.2 ToBoolean over a concolic's example answered neither true nor false — it answers -1 "
               "only for an exception value, so a producer has attached a pending throw to a value as its "
               "example");
        example = JS_NewBool(ctx, b ^ negate);
    }
    JS_FreeValue(ctx, ex);
    r = concolic_alloc(ctx, shape, src, root, ident, example);
    free(shape);
    rc = JS_GetOpaque(r, g_concolic_class);
    DCHECK(rc != NULL, "a ToBoolean result was minted as something that is not a concolic value");
    /* THE BRANCH KEY AND ITS POLARITY, WRITTEN TOGETHER — the operand's own branch key, so a chain of
       negations keys the innermost predicate and never a tower of wrappers. An operand this engine cannot
       spell leaves BOTH absent, which is the sound answer: no key, no polarity, and every branch over the
       result keeps both arms. */
    bkey = concolic_branch_ident_c(v);
    if (bkey) {
        rc->br_key = strdup(bkey);
        CHECK(rc->br_key, "concolic: OOM recording the predicate a negation is the complement of");
        rc->br_neg = (signed char)(concolic_branch_neg(v) ^ negate);
    }
    /* …AND THE OBSERVATION THE OPERAND CARRIES, VERBATIM — see pred_carry_through_not for why it is not
       inverted here and why the arm is where the negation lives. */
    pred_carry_through_not(r, v);
    return r;
}

/* A BUILTIN OVER AN UNKNOWN OPERAND — see the hook's contract in quickjs.h. The shape records WHICH operation
   produced it, so an @H shape reads as the expression the page actually wrote and an @S search knows which
   source to solve for.
   THIS IS NOT EXAMPLE-FREE, AND THE SENTENCE THAT SAID SO OUTLIVED THE PARAMETER THAT REFUTES IT. The header
   used to state that this engine does not yet run the operation on the operand's example — while the signature
   already TOOK one and the body fifteen lines down already said it "is what the operator got by RUNNING THE
   REAL OPERATION on this operand's own example". Two paragraphs of one comment contradicting each other is the
   stale-claim failure at its shortest range, and the wrong half was the one a reader meets FIRST: it reads as
   a completed statement about the engine, so nobody greps, and a caller that could have run its real operation
   passes JS_UNDEFINED because the comment told it there was no point. Which operators DO run it is a fact
   about their call sites and is stated at each of them, never here — a list here would be the next sentence to
   go stale. What is true of THIS function is only its contract: it derives from the operand and attaches
   whatever example it is handed, JS_UNDEFINED included, and never computes or predicts one itself. */

/* SEE concolic.h — the VALUE twin of concolic_new_rel, over an ORDERED operand list, and the one speller
   concolic_builtin_hook is the n == 1 case of. */
JSValue concolic_new_derived(JSContext *ctx, const char *op, const JSValueConst *operands, int n,
                             JSValue example)
{
    const char *src, *root, **fields;
    char **parts, *shape, *args = NULL, *ident;
    JSValue r;
    int i, first = -1;

    DCHECK(op != NULL,
           "a component derived an unknown result without naming the OPERATION it was performing — the "
           "operation is what tells two derivations over one operand list apart, and a value that dropped it "
           "would be decided by whichever of them this flow reached first");
    DCHECK(n >= 1, "a derivation was minted over NO operands — a value derived from nothing is a source read, "
                   "which is concolic_new's question and carries a provenance this entry has no operand to "
                   "take one from");
    DCHECK(operands != NULL, "a derivation was minted with an operand count and no operands");
    /* AN OBJECT EXAMPLE IS NOT REFUSED HERE, AND THE ASSERT THAT USED TO REFUSE ONE IS RETIRED RATHER THAN
       MOVED. It read "an example is the value the caller COMPUTED by running the real operation on the
       operands' own examples, so it is a primitive; a later §7.1.4 ToNumber over an object one reaches
       ToPrimitive from C" — and every clause of that is true of a COERCION and none of it is true of this
       function, which runs no coercion at all. It composes a shape and an identity and hands the example to
       concolic_derived to store.
       IT WAS THE ONLY COPY OF THAT ARGUMENT STANDING ANYWHERE BUT AT A COERCION. The others are
       concolic_arith_hook's, over the operands it is about to put through §7.1.4 ToNumber; this file's `+`
       hook's, over §13.15.3 step 1.c's two primitives; and quickjs.c's two, over the JS_ToInt32 and the
       numeric comparison each of them is standing in front of. Each of those guards a conversion it performs
       on the next line and each of them stays. This one guarded a store.
       AND IT MADE THE DERIVED PATH DISAGREE WITH THE SOURCE PATH ABOUT A STATE THIS CLASS IS BUILT ON.
       concolic_new attaches an object example with no assert, because a server-injected record's example IS an
       object — example_slot reads its slots, concolic_exotic_own_names walks it with
       JS_GetOwnPropertyNames, concolic_exotic_delete removes members from it, and every one of those is what
       models a record at all. Two seams over one primitive, one asserting and one not, and the silent one had
       been exercised on the whole population of injected state without firing.
       WHAT FIRED IT was §25.5.2 JSON.parse ( text [ , reviver ] ) over an unknown text: js_json_parse_vfini
       derives its completion from the source and attaches what the real codec made of that source's example,
       which for a text whose example is `{"limit":7}` is an Object — the one concrete thing the run performed
       the real parse to obtain, and the value `page.limit` is then read back out of. (Its own comment argues
       for exactly that and is not quoted here: a run of this tree's prose in quotation marks under a spec
       citation is indistinguishable, to citegen and to a reader, from a sentence claimed to be the spec's.) The
       comparison path already refuses an object example at its own door for its own reason (example_comparable,
       which declines the identity of a value this engine minted); nothing else here coerces one. */
    for (i = 0; i < n; i++)
        if (concolic_is(operands[i])) { first = i; break; }
    /* NO OPERAND IS UNKNOWN, so the caller already has the answer and there is nothing to derive — the same
       answer concolic_builtin_hook gives for a known operand, and the reason this is JS_UNINITIALIZED rather
       than a mint: a derivation over wholly concrete operands would put a fork in the frontier over a question
       the engine can already answer. */
    if (first < 0) { JS_FreeValue(ctx, example); return JS_UNINITIALIZED; }
    /* See concolic.h: the FIRST UNKNOWN operand's, which is the rule concolic_arith_hook applies to the
       interpreter's own binary arithmetic. */
    src = concolic_src_c(operands[first]);
    root = concolic_root_c(operands[first]);

    parts = malloc((size_t)n * sizeof *parts);
    fields = malloc(((size_t)n + 1) * sizeof *fields);
    CHECK(parts != NULL && fields != NULL, "concolic: OOM composing a derivation over several operands");
    for (i = 0; i < n; i++) {
        parts[i] = derived_operand_shape(ctx, operands[i]);
        fields[i] = ident_of_operand(ctx, operands[i]);   /* OWNED; NULL is an unspellable operand */
    }
    /* THE OPERATION IS THE LAST FIELD AND THE OPERANDS COME FIRST, which is concolic_builtin_hook's own order
       written out — so its two-field `(ident, op)` is literally this composition at n == 1 and no key that
       entry has ever minted moves. A different order here would be a second namespace for one question. */
    fields[n] = op;
    ident = concolic_ident_compose("b", fields, n + 1);
    /* THE SHAPE. One operand renders as a method ON that operand, which is what every derivation minted before
       this entry existed already reads as; two or more render as the operation APPLIED to them, because a
       result that no one operand is the subject of has no subject to hang a method off. */
    if (n == 1) {
        shape = shapef("%s.%s()", parts[0], op);
    } else {
        for (i = 0; i < n; i++) {
            char *next = args ? shapef("%s, %s", args, parts[i]) : shapef("%s", parts[i]);
            free(args);
            args = next;
        }
        shape = shapef("%s(%s)", op, args);
    }
    /* `example` is what the CALLER got by RUNNING THE REAL OPERATION on the operands' own examples. It is
       never computed here and never predicted. */
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, example);
    for (i = 0; i < n; i++) { free(parts[i]); free((char *)fields[i]); }
    free(parts);
    free((void *)fields);
    free(args);
    free(shape);
    return r;
}

JSValue concolic_builtin_hook(JSContext *ctx, JSValueConst v, const char *op, JSValue example) {
    if (!concolic_is(v)) { JS_FreeValue(ctx, example); return JS_UNINITIALIZED; }
    /* ONE OPERAND, THROUGH THE ONE SPELLER. This body composed `("b", {ident, op})` and the shape
       `{x}.op()` itself; both are now concolic_new_derived's n == 1 case, byte for byte, so a component that
       reaches for the several-operand entry with one operand and a component that reaches for this one compose
       the same key instead of two keys for one derivation. */
    return concolic_new_derived(ctx, op, &v, 1, example);
}

/* THE BYTES A DOM MEMBER NEEDS FROM AN ARGUMENT THAT MAY BE UNKNOWN — a selector, an attribute name, a class
   token, an element id. Every one of those call sites did JS_ToCString under a comment saying "a real string by
   now: the declaration converted it", and that comment is FALSE for a concolic: the IDL boundary passes unknown
   input through as itself on purpose, so the coercion crashed and `document.querySelector(location.hash)` ended
   the document. An unknown NAME denotes its SHAPE, which is the same rule the key_name hook already states for
   `obj[x]` and the same one setAttribute already applies to an unknown VALUE: a real string, stable per source,
   so two lookups through one source agree and two sources never collide. OWNED either way, so the call site
   keeps one free path and does not grow a branch. */
const char *concolic_name_cstr(JSContext *ctx, JSValueConst v) {
    if (concolic_is(v)) {
        const char *sh = concolic_shape_c(v);
        JSValue s = JS_NewString(ctx, sh ? sh : "{}");
        const char *r = JS_ToCString(ctx, s);
        JS_FreeValue(ctx, s);
        return r;
    }
    return JS_ToCString(ctx, v);
}

/* THE NAMES THIS FILE HAS SPENT AN UNKNOWN ON — what makes the inverse below possible, and the reason one has
   to exist at all.
   `concolic_key_name_hook` buys an ATOM with a value's PROVENANCE and its DOMAIN, and ECMAScript prices that
   correctly: §6.1.7 The Object Type says "A property key is either a String or a Symbol.", so a key IS bytes
   and an unknown cannot be one. The purchase is FINAL and CORRECT at every site that wanted bytes — a lookup,
   an atom a builtin consumes, a `@WHY` message's display form. It is a LOSS at the one shape of site that
   wanted a VALUE: an ENUMERATION hands the name back to the page, and a value that was unknown and is now a
   real string forks nothing (`k === "admin"` compares two concrete strings and prunes an arm no constraint
   contradicts) and reports itself as computed — §@H one step worse than the case it names, because nothing
   marks these bytes as a shape at all.
   SO THE MINT WRITES DOWN WHAT IT SPENT. FOUR STRINGS AND NO JSValue, deliberately: a table of live values
   would be a GC root whose lifetime this component does not own, and §State-isolation's rule about platform
   data a flow queues is about exactly this shape — a global holding heap pointers reverts nothing at a context
   switch and parks as nothing. The four are what `concolic_alloc` takes, so the restore is a RE-MINT through
   the one derivation door rather than a second constructor.
   IT IS NOT PER-FLOW STATE AND MUST NOT BE CAPTURED. The map from a value to its shape is a pure function of
   the value, so two flows minting one unknown record ONE entry and neither can disagree with the other. What
   is per-flow is the SLOT the name goes on, and that is an ordinary property write the COW delta already
   carries. It is AGENT state, released with the sources and the paths at concolic_free. */
typedef struct { char *shape, *src, *root, *ident; } KeyName;
static KeyName *g_keynames; static int g_keynames_n, g_keynames_cap;
static int *g_keynames_hash; static int g_keynames_hash_cap;   /* the index (shape -> idx+1) */

static int keyname_find(const char *shape) {
    uint32_t m, h;
    if (!g_keynames_hash) return -1;
    m = (uint32_t)g_keynames_hash_cap - 1; h = cons_hash(shape) & m;
    while (g_keynames_hash[h]) {
        if (!strcmp(g_keynames[g_keynames_hash[h] - 1].shape, shape)) return g_keynames_hash[h] - 1;
        h = (h + 1) & m;
    }
    return -1;
}
static void keyname_hash_put(int idx) {   /* caller guarantees room */
    uint32_t m = (uint32_t)g_keynames_hash_cap - 1, h = cons_hash(g_keynames[idx].shape) & m;
    while (g_keynames_hash[h]) h = (h + 1) & m;
    g_keynames_hash[h] = idx + 1;
}
static void keyname_hash_rebuild(void) {
    int i, nc = 16;
    int *nh;
    /* SIZED IN A LOCAL AND PUBLISHED AFTER — cons_hash_rebuild's rule, for its reason: the allocation can sell
       a flow (solver/reclaim.h), and a `cap` advertising the new size over the old table is an out-of-bounds
       read for anything that looks a name up while the sale runs. */
    while (nc < g_keynames_n * 2) nc *= 2;
    nh = reclaim_realloc(g_keynames_hash, (size_t)nc * sizeof(int));
    CHECK(nh, "concolic: OOM indexing the names spent on unknown keys");
    g_keynames_hash = nh; g_keynames_hash_cap = nc;
    memset(g_keynames_hash, 0, (size_t)g_keynames_hash_cap * sizeof(int));
    for (i = 0; i < g_keynames_n; i++) keyname_hash_put(i);
}
/* ABSENT ON BOTH SIDES IS ONE FACT AND NOT A MISS: `src` and `root` are NULL together for a value with no
   provenance, and `ident` is NULL for one this engine cannot spell, so a comparison that treated NULL as
   unequal-to-everything would report ONE value as two and the assertion below would fire on agreement.
   COMPILED WITH ITS ONE READER, which is why the `#if` is here and around the assert below rather than being
   left to the macro: a release DCHECK is `(void)sizeof(cond)` — type-checked and never evaluated — so the name
   must still RESOLVE in a build that emits no call to it, and a static function in that state is exactly what
   -Wunneeded-internal-declaration reports. The pair is dev-only together or neither is. */
#if APICLIENT_DEV
static int keyname_str_same(const char *a, const char *b) { return a ? (b && !strcmp(a, b)) : !b; }
#endif

static void keyname_record(const char *shape, const char *src, const char *root, const char *ident)
{
    KeyName *e;
    int i = keyname_find(shape);

    if (i >= 0) {
        /* ONE SHAPE, ONE VALUE — the claim this engine's whole slot model rests on, asserted at the one place
           a second value can arrive under a name the first already holds. quickjs.h states it in prose at
           .key_name ("two writes through the same unknown source land in the SAME slot, two different sources
           in different ones"), concolic.h repeats it and concolic_name_cstr repeats it again, and until this
           line nothing checked any of them.
           IT IS AN INVARIANT AND NOT A PREFERENCE, because the restore below is a FUNCTION OF THE SHAPE: two
           values under one name make it answer one of them for a key that was the other, which does not
           de-taint a value — it MIS-ATTRIBUTES one, so a sink reached through the second source is reported
           against the first and its @S candidate is solved at an address that never fed it. Two concrete
           strings would be the smaller error.
           WHERE THE FIX GOES IF IT FIRES, which is never here. This paragraph named ONE function —
           concolic_new_derived — and that was wrong, in exactly the way CLAUDE.md predicts a remedy clause is
           wrong: the SPEC half held and the claim about THIS TREE did not. It was refuted by reading the
           spellers rather than the named line. concolic_new_derived is not the speller that fires and was
           never the only one; THE RULE IS THE THING, and it is checked at every speller that composes a shape
           and an identity out of one operand list:

               A SHAPE MUST SEPARATE EVERY PAIR OF OPERANDS ITS IDENTITY SEPARATES,
               WHEREVER THE SHAPE IS AN EXPRESSION.

           The second line is not a hedge on the first, it is the boundary the sweep found, and stating the
           rule without it would be an absolute one counterexample retires. concolic_add_hook's shape is NOT
           an expression: it is the STRING the concatenation produced, which is what makes `"/api/" + x` render
           `/api/{x}` — the @H provenance the endpoint surface is composed of. So `x + 5` and `x + "5"` are two
           identities under one shape THERE and must stay that way; that pair belongs to the residual below and
           not to any speller. Every other site in this file renders an expression and owes the rule outright.

           The one that actually fired is concolic_call, which spelled `%s()` over the CALLEE ALONE while
           composing `()` over the callee AND every argument — so `h.slice(1)` and `h.slice(2)` were two
           identities under one shape. Reproduced at origin/main 9c757178 in four lines of page script, and
           the fix is at that speller. derived_operand_shape carried a smaller instance of the same loss at
           the leaf (§7.1.19 ToString spells the Number 5 and the String "5" alike while the identity separates
           them by kind); it is now kind-faithful and sits beside ident_of_operand so the pair reads as one
           rule.
           concolic_key_read_hook was a THIRD: it spelled `{}` for the OBJECT while composing the object's
           identity into `[]`, so `__FLAGS[k]` and `__NEXT_DATA__[k]` were two identities under one shape. A
           draft of this paragraph claimed that site was exempt because an ordinary object has no identity
           either; that is true of an ordinary object and FALSE of an unknown RECORD, which has one — and the
           four-line document proving it fired this assert. The claim was written, then checked, then refuted,
           which is the only reason it is not still standing here as an exemption.
           concolic_typeof_hook was a FOURTH: it spelled `typeof %s` over the operand's SRC, which every
           derivation off one source inherits, so `typeof (x+1)` and `typeof (x+2)` were one shape — and it
           was the wrong display besides, naming the source where the page wrote the derived value.
           concolic_arith_hook was a FIFTH, by hand-rolling derived_operand_shape's job as `cstr_dup`: the same
           loss of the kind, plus an unconditional JS_ToCString that ran the page's own valueOf from a C
           activation with no flow base under it. All five now route through the two spellers that sit
           together, which is what makes the rule checkable by reading one place instead of five.

           NAMED RESIDUAL — THE ONE PAIR NO SHAPE CHANGE REACHES, and it is the case this assert is BLIND to
           rather than the case it catches. Where an operand is an Object or a Symbol, concolic_ident_compose
           makes the WHOLE identity absent, so two genuinely different derivations carry ident == NULL, equal
           src and equal root: every field this assert compares AGREES, the assert passes, and the two still
           buy one atom and land in ONE slot. Measured on the artifact of 9c757178:
           `o[h.slice({})] = "a"; o[h.slice({z:1})] = "b";` emitted `Object.keys(o).length` as 1 and read `b`
           back through the first key, with no abort anywhere.
           WHAT THE NEXT DIFF BUILDS: not a finer shape — there is none, because the identity is absent too —
           but an ANSWER for a key whose value has no spellable identity at all. That is a question about what
           .key_name may spend, not about what a speller renders, and it is the only half of this defect that
           is a missing capability rather than a wrong rendering.
           HOW ITS ABSENCE SHOWS: a page whose unknown key is derived through an unspellable operand (a RegExp
           handed to `replace`, an options object handed to a formatter) loses members off its own object —
           `Object.keys(o).length` short by one per collision — while this assert stays silent. */
#if APICLIENT_DEV
        DCHECK(keyname_str_same(g_keynames[i].src, src) &&
               keyname_str_same(g_keynames[i].root, root) &&
               keyname_str_same(g_keynames[i].ident, ident),
               "two different unknowns denote ONE property name — a display shape is what this engine spends "
               "to buy an atom, so two values that spell one shape land in one slot and the name->value "
               "restore answers one of them for a key that was the other. The repair is at whichever SPELLER "
               "composed this value: a shape must separate every pair of operands its identity separates. Read "
               "the paragraph above before reaching for a function name — this message used to carry one and "
               "it was the wrong one");
#endif
        return;
    }
    if (g_keynames_n == g_keynames_cap) {
        int nc = g_keynames_cap ? g_keynames_cap * 2 : 16;
        KeyName *nv = reclaim_realloc(g_keynames, (size_t)nc * sizeof *nv);
        CHECK(nv, "concolic: OOM recording the name spent on an unknown key");
        g_keynames = nv; g_keynames_cap = nc;
    }
    e = &g_keynames[g_keynames_n];
    e->shape = strdup(shape);
    CHECK(e->shape, "concolic: OOM copying an unknown key's name");
    e->src   = src   ? strdup(src)   : NULL;
    CHECK(!src   || e->src,   "concolic: OOM copying an unknown key's provenance");
    e->root  = root  ? strdup(root)  : NULL;
    CHECK(!root  || e->root,  "concolic: OOM copying an unknown key's delivery root");
    e->ident = ident ? strdup(ident) : NULL;
    CHECK(!ident || e->ident, "concolic: OOM copying an unknown key's identity");
    g_keynames_n++;
    if (g_keynames_hash_cap < g_keynames_n * 2) keyname_hash_rebuild();
    else keyname_hash_put(g_keynames_n - 1);
}

/* THE NAME an unknown key denotes: its own SHAPE, as a real string. Stable per source, so every key-taking
   operation agrees with every other — see the contract at JS_ToPropertyKeyInternal. */
JSValue concolic_key_name_hook(JSContext *ctx, JSValueConst key) {
    const char *sh;
    if (!concolic_is(key)) return JS_UNINITIALIZED;
    sh = concolic_shape_c(key);
    /* WHAT THIS TRADE COSTS IS WRITTEN DOWN AS IT IS SPENT — see the table above. The atom is bytes, the
       value was an unknown, and the only place that trade can be undone is the one place it is made. */
    keyname_record(sh ? sh : "{}", concolic_src_c(key), concolic_root_c(key), concolic_ident_c(key));
    return JS_NewString(ctx, sh ? sh : "{}");
}

/* JSConcolicHooks.key_value — the trade above, UNDONE where a property name is handed back to the program as a
   VALUE. See that member for why the engine asks, and the table above for what is recorded. */
JSValue concolic_key_value_hook(JSContext *ctx, JSValueConst name)
{
    const char *s;
    char *ident;
    int i;

    if (!JS_IsString(name)) return JS_UNINITIALIZED;
    s = JS_ToCString(ctx, name);
    /* ASSERTED AND NOT TAKEN FOR A MISS. A NULL here is an allocation failure inside the conversion of a value
       that already IS a string, and it renders identically to "no unknown was ever minted under this name" —
       so the one arrival this lookup cannot distinguish from its ordinary answer is the one that silently
       de-taints a key. It is the same reason concolic_deliver asserts its payload rather than defaulting it. */
    CHECK(s != NULL,
          "concolic: the bytes of a delivered property name could not be read — a name this file minted for "
          "an unknown key would then answer as an ordinary string and the key's provenance and domain would "
          "be gone with nothing anywhere to say so");
    i = keyname_find(s);
    JS_FreeCString(ctx, s);
    if (i < 0) return JS_UNINITIALIZED;
    ident = g_keynames[i].ident ? strdup(g_keynames[i].ident) : NULL;
    CHECK(!g_keynames[i].ident || ident, "concolic: OOM re-minting an unknown key's identity");
    /* THROUGH concolic_derived AND NOT concolic_alloc, which is what makes a restored key reach an @S
       candidate: that entry is the one door every mint goes through ("Minting is the one place every source
       goes through, whichever way it is reached"), so a name restored while a substitution is installed for
       its source delivers the attacker's payload exactly as the original read did, and a name restored with
       none re-mints the value the original read produced.
       EXAMPLE-FREE, which is a positive statement and not a dropped field: which property the attacker names
       is exactly what is not known, so §@H has nothing to carry here — the same rule concolic_key_read_hook
       states about the value such a key reads. */
    return concolic_derived(ctx, g_keynames[i].shape, g_keynames[i].src, g_keynames[i].root, ident,
                            JS_UNDEFINED);
}

/* `obj[x]` WITH AN UNKNOWN KEY. Not a coercion of the operand: nothing about x says WHICH slot was meant, so
   the read's result is unknown and the honest answer is a concolic that keeps the key's source — a gate on the
   value still forks, and a sink still solves for the key that would reach it. Example-free on purpose: which
   property the attacker names is exactly what is not known, and @H never invents one. */
JSValue concolic_key_read_hook(JSContext *ctx, JSValueConst obj, JSValueConst key) {
    const char *src, *root, *f[2];
    char *shape, *ident, *io, *ik;
    JSValue r;

    if (!concolic_is(key)) return JS_UNINITIALIZED;
    src = concolic_src_c(key);
    root = concolic_root_c(key);
    /* WHICH OBJECT WAS READ IS HALF THE IDENTITY: `a[x]` and `b[x]` are two reads and one must not decide the
       other. An ordinary object has no identity that survives the park a resumed flow replays through, so the
       composition is ABSENT there and every branch over the result keeps both arms.
       AND IT IS HALF THE SHAPE FOR THE SAME REASON, WHICH THIS LINE SPELLED `{}` FOR AND DID NOT SAY. The
       object was in the identity and NOT in the display, so two unknown RECORDS read with one unknown key —
       `__FLAGS[k]` and `__NEXT_DATA__[k]` — composed two identities under the one shape `{}[{x}]`; used as
       property keys those bought ONE atom, which keyname_record asserts against and which, with the assert
       compiled out, is one slot for two members. Reproduced on the artifact of origin/main 9c757178.
       derived_operand_shape IS THE SPELLER and not a `%s` of concolic_shape_c, because it is the twin of the
       ident_of_operand call directly below: an unknown renders as its own shape, a spellable primitive as the
       literal the page wrote, and an ordinary object as `?` — which is the honest display for the operand
       whose identity is absent, and the one pair this rule cannot separate. */
    io = ident_of_operand(ctx, obj);
    ik = ident_of_operand(ctx, key);
    { char *os = derived_operand_shape(ctx, obj);
      shape = shapef("%s[%s]", os, concolic_shape_c(key) ? concolic_shape_c(key) : "{}");
      free(os); }
    f[0] = io; f[1] = ik;
    ident = concolic_ident_compose("[]", f, 2);
    free(io); free(ik);
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, JS_UNDEFINED);
    free(shape);
    return r;
}

JSValue concolic_typeof_hook(JSContext *ctx, JSValueConst v) {
    const char *sh, *f[1];
    char *shape, *ident;
    JSValue r;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    /* THE OPERAND'S SHAPE AND NOT ITS SRC, which is what concolic_tostr_hook eight lines up already does and
       what this line did not. A src is inherited by every derivation off one source — `x+1` and `x+2` carry
       x's — so spelling the display off it made `typeof (x+1)` and `typeof (x+2)` ONE shape over two
       identities, which is one property atom for two values at keyname_record. It was also simply the wrong
       display: `typeof {location.hash}` names the source, and the page wrote typeof of the derived value. */
    sh = concolic_shape_c(v);
    shape = shapef("typeof %s", sh ? sh : "{}");
    f[0] = concolic_ident_c(v);
    ident = concolic_ident_compose("typeof", f, 1);
    r = concolic_derived(ctx, shape, shape, concolic_root_c(v), ident, JS_UNDEFINED);
    free(shape);
    return r;
}

/* `x in concolic` / property existence: a concolic collection "has" any key (so a membership gate still runs). */
static int concolic_exotic_has(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    (void)ctx; (void)atom;
    return JS_GetOpaque(obj, g_concolic_class) != NULL;
}

/* ── THE RECORD'S OWN SURFACE ────────────────────────────────────────────────────────────────────────────────
 *
 * A KEY THE PAGE NAMES AND A REQUEST TO ENUMERATE ARE TWO DIFFERENT QUESTIONS, and the answers above are the
 * first kind. [[Get]] and [[HasProperty]] are handed a NAME the page wrote, so "unknown, and here is a derived
 * unknown for it" is both honest and the whole of the exploration: `if (blk.props.pageProps.user)` forks on a
 * member the payload does not hold, which is what reaches the logged-in arm. ECMAScript §10.1.11
 * [[OwnPropertyKeys]] ( ) is asked with NO name at all — the program is asking the record to enumerate ITSELF
 * — so there is no name to be honest about, and every key answered that the record does not hold is a
 * FABRICATED field in exactly §@H's sense ("must NEVER INVENT"), performed by a `for..in`.
 *
 * SO THE OWN SURFACE IS THE EXAMPLE'S, AND ONLY THE EXAMPLE'S. That is not a weaker answer than [[Get]]'s, it
 * is an answer to a different question: the example is the record the server actually sent this visitor, so
 * its key set is an OBSERVATION exactly as a held member's bytes are, and a key it does not hold is a key this
 * run observed the record not to have.
 *
 * FOUR SPELLINGS REACH IT AND EVERY ONE OF THEM NEEDS BOTH HALVES, which is why [[GetOwnProperty]] is
 * installed beside [[OwnPropertyKeys]] and not after it. §20.1.2.19 Object.keys ( obj ) and §14.7.5.9
 * EnumerateObjectProperties ( obj ) run §7.3.23 EnumerableOwnProperties ( obj, kind ), and §7.3.25
 * CopyDataProperties ( target, source, excludedItems ) — the object spread — and §20.1.2.1 Object.assign (
 * target, ...sources ) run the same two steps: `obj.[[OwnPropertyKeys]]()`, then `obj.[[GetOwnProperty]](key)`
 * for the [[Enumerable]] test. A class answering only the first has every key dropped at the second (the C key
 * walk drops it under JS_GPN_ENUM_ONLY, the resumable cursor drops it as "gone between the snapshot and the
 * read"), so the report is the same empty list either way and nothing says a capability is missing.
 *
 * THE VALUE IS THE ONE [[Get]] ANSWERS, never the example's own bytes. §Solver's trust boundary is explicit
 * that a loaded `features.admin:false` must not concretize the gate over it; a descriptor carrying the raw
 * example would concretize it through `Object.getOwnPropertyDescriptor`, through
 * `Object.defineProperties(t, Object.getOwnPropertyDescriptors(cfg))`, and through every C consumer of the
 * descriptor — the same loss the spread and the assign would suffer if their `Get` did not go through
 * concolic_exotic_get. One value, one derivation, one place.
 *
 * §6.1.7.3 Invariants of the Essential Internal Methods DECIDES THE ATTRIBUTES rather than taste. Its
 * [[GetOwnProperty]] note: "if a property is described as a data property and it may return different values
 * over time, then either or both of the [[Writable]] and [[Configurable]] attributes must be true even if no
 * mechanism to change the value is exposed" — and this one does, because each read mints the derivation
 * afresh. So CONFIGURABLE and WRITABLE are the spec's answer and not a choice; only ENUMERABLE is read off the
 * example, where it is an observation about the record. (Nothing in this file is the PROXY's §10.5.5 /
 * §10.5.11 — those are §10.5 Proxy Object Internal Methods and Internal Slots, a different exotic object whose
 * invariants are enforced by re-checking a trap's result. A concolic has no handler; it maintains §6.1.7.3
 * Invariants of the Essential Internal Methods directly, which is what that clause requires of "any
 * implementation provided exotic objects".)
 */

/* ANSWER FROM REAL SLOTS ONLY — see concolic.h. A plain static like every other in this file: the host is one
   agent per instance and nothing here is entered from a second thread. */
static int g_slots_only;

void concolic_slots_only_begin(void)
{
    DCHECK(!g_slots_only,
           "a slots-only read nested inside another — the mode is entered around ONE lookup that runs no page "
           "code and can therefore not suspend, so a second entry is a caller holding it across work");
    g_slots_only = 1;
}

void concolic_slots_only_end(void)
{
    DCHECK(g_slots_only, "a slots-only read was ended without having been begun");
    g_slots_only = 0;
}

/* Does `obj` carry a REAL own slot for `atom` — the ordinary layer under this class's own surface? The mode is
   what makes the question answerable at all: with the exotic hook armed, quickjs's [[GetOwnProperty]] answers
   1 for a shape slot AND for an example member, and the two are exactly what has to be told apart.
   IT IS NOT DEV-ONLY ANY MORE. It was, when its only caller was the disjointness assert below; the unknown-
   member materialisation asks the same question to decide whether it has anything to do, and that runs in every
   build. One speller for one question, rather than an assert's copy and a live copy that can disagree. */
static int concolic_has_own_slot(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
    int r;

    concolic_slots_only_begin();
    r = JS_GetOwnPropertyNoUserCode(ctx, NULL, obj, atom);
    concolic_slots_only_end();
    DCHECK(r >= 0, "a slots-only own-property lookup threw — this class's hook answers 0 under the mode and a "
                   "shape lookup has no other completion");
    return r > 0;
}

/* §10.1.5 [[GetOwnProperty]] ( propertyKey ) over the record — reached only after quickjs's ordinary lookup
   found no slot, so a member the flow has MATERIALISED (see the define below) never arrives here. */
static int concolic_exotic_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom atom)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSValue ev;
    int eflags;

    if (!c)
        return 0;
    /* THE DELTA READS STORAGE AND THIS CLASS HAS NONE FOR ITS MEMBERS — concolic.h states the whole of it. */
    if (g_slots_only)
        return 0;
    if (!example_slot(ctx, c->example, atom, &ev, &eflags))
        return 0;
    if (!desc) {
        JS_FreeValue(ctx, ev);
        return 1;
    }
    /* The example's own bytes are NOT the answer — they ride the derivation as its EXAMPLE, which is what
       concolic_exotic_get composes. An ACCESSOR member arrives here with no value for the same reason
       example_slot gives, and is still reported as a data property: what the page's getter would compute is
       unknown, and an unknown is what this class answers with. */
    JS_FreeValue(ctx, ev);
    desc->flags = (eflags & JS_PROP_ENUMERABLE) | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = concolic_exotic_get(ctx, obj, atom, obj);
    return 1;
}

/* THE ENUMERATION'S QUESTION, MINTED ONCE — see concolic.h for what it is and why the fork cannot live at the
   internal method below. The identity is the record's own, under §10.1.11's name, so two consumers asking
   about ONE record ask ONE question and a flow's record of it decides that question and no neighbour's. A
   record this engine cannot spell composes no identity, which is the same positive statement `ident` makes
   everywhere else — no key, nothing decided from it — and the internal method says so at its own site rather
   than being handed a predicate that silently answers -1 forever. */
static JSValue own_keys_pred(JSContext *ctx, const Concolic *c)
{
    const char *f[1];
    char *ident;

    f[0] = c->ident;
    ident = concolic_ident_compose("[[OwnPropertyKeys]]", f, 1);
    if (!ident)
        return JS_UNINITIALIZED;
    /* NOT a source read (concolic_alloc, never concolic_derived): this is a boolean about the record, so an @S
       candidate substituted into it would answer a question with a payload. */
    return concolic_alloc(ctx, "{cmp}", NULL, NULL, ident, JS_UNDEFINED);
}

JSValue concolic_own_keys_pred(JSContext *ctx, JSValueConst record)
{
    Concolic *c = g_concolic_class ? JS_GetOpaque(record, g_concolic_class) : NULL;

    /* THE ONE ANSWER, REACHED TWO WAYS, AND IT IS ONE FACT: this enumeration has no question in it. An
       ordinary object's key set is its shape; a record whose EXAMPLE is present has the example's, which is an
       OBSERVATION about the payload this visitor was served and is answered by the internal method below. Only
       an example-free record has an unknown key set, and the condition here is written the same way the method
       below writes it so the asker and the reader cannot disagree about which records carry the question. */
    if (!c || !JS_IsUndefined(c->example))
        return JS_UNINITIALIZED;
    return own_keys_pred(ctx, c);
}

/* THE NAME OF THE RECORD'S n-TH UNKNOWN OWN MEMBER — see concolic.h. Two spellers and no third: the identity
   and the display form come from concolic_new_derived, and the conversion from that value to a real property
   key is the engine's own JS_ValueToAtom, which is where §7.1.21 ToPropertyKey ( arg )'s answer for an unknown
   key lives (the key's shape, through .key_name). Composing the string here instead would be a second copy of
   both rules, and the one that drifts is the copy no read goes through.
   `#n` AND NOT `>n`: the chain's per-position QUESTION is spelled `[[OwnPropertyKeys]]>n` by the engine, and
   this is a different question about the same position — what the member is CALLED, not whether it exists — so
   it is a different key by construction rather than by luck.
   A CANDIDATE RUN LEGITIMATELY ANSWERS A STRING HERE: concolic_new_derived goes through concolic_derived,
   which substitutes the attacker's payload for a derivation over the source under test, and a member named by
   that payload is exactly what the re-fire is for. JS_ValueToAtom takes either. */
static JSAtom own_member_atom(JSContext *ctx, JSValueConst record, int n)
{
    char op[40];
    JSValue key;
    JSAtom atom;
    int w = snprintf(op, sizeof op, "[[OwnPropertyKeys]]#%d", n);

    CHECK(w > 0 && (size_t)w < sizeof op,
          "concolic: an unknown member's name did not fit its operation buffer — a truncated one names two "
          "positions with one string, so the record would hold one member where the flow decided two");
    key = concolic_new_derived(ctx, op, &record, 1, JS_UNDEFINED);
    /* ASSERTED AND NOT BRANCHED PAST: both callers have already established that `record` is one of this
       class's values, and concolic_new_derived answers JS_UNINITIALIZED for exactly one reason — no operand is
       unknown — so this cannot hold unless the caller and this file disagree about what the operand is. */
    DCHECK(!JS_IsUninitialized(key),
           "an unknown member was named over a value this class does not hold — the callers are the "
           "enumeration's own-member chain and its own arm-1 assertion, both of which run only for a record "
           "whose own-key-set predicate this file minted");
    atom = JS_ValueToAtom(ctx, key);
    JS_FreeValue(ctx, key);
    return atom;
}

int concolic_own_key_mint(JSContext *ctx, JSValueConst record, int n)
{
    Concolic *c = g_concolic_class ? JS_GetOpaque(record, g_concolic_class) : NULL;
    JSAtom atom;
    JSValue val;
    int r;

    if (!c)
        return 0;
    /* THE SAME CONDITION concolic_own_keys_pred ASKS, written the same way, because the two are one decision:
       a record whose EXAMPLE is present has a key set this run OBSERVED, so there is no unknown member to name
       and no question was ever minted for the caller to be standing on. */
    DCHECK(JS_IsUndefined(c->example),
           "a record whose example holds its key set was asked to materialise an UNKNOWN member — its key set "
           "is an observation about the payload this visitor was served, so the caller forked over a question "
           "concolic_own_keys_pred never minted");
    DCHECK(n >= 0, "an unknown member was asked for at a negative position — the chain's cursor is a count");
    atom = own_member_atom(ctx, record, n);
    if (atom == JS_ATOM_NULL)
        return -1;
    /* ALREADY MATERIALISED, WHICH IS THE ORDINARY CASE AND NOT AN ERROR. The name is a function of the record
       and `n` alone, so a second enumeration in this flow replays the same arms and arrives back at the member
       this flow already made. Re-defining it would replace a value the page may have written since, and on a
       record the page has frozen it would be refused outright — a refusal about a member that is already there
       and already correct.
       NAMED RESIDUAL — a [[Delete]] of a materialised member is UNDONE by the next enumeration of that record
       in the same flow. §10.1.10 [[Delete]] ( propertyKey ) removes a configurable slot, and these are
       configurable (§6.1.7.3 requires it of a data property whose value may differ over time), so the slot goes;
       the flow's recorded answers do not, and the chain replays them and defines the member again. WHAT THE
       NEXT DIFF BUILDS: a per-flow record of how many of this record's unknown members this flow has already
       PERFORMED, so the chain resumes past them instead of replaying from position 0. Where that lives is the
       work and not a detail — this class keeps no delta-captured C state at all (nothing here calls
       cow_capture_host_record, and the header's slots-only argument is that the delta records SLOTS), so a
       plain field on the Concolic would be state a context switch does not swap, and the count has to sit
       somewhere the delta already carries. HOW ITS ABSENCE SHOWS: `for (k in x) delete x[k]` followed by a
       second enumeration of `x` lists the deleted member again, and `k in x` answers true after the delete. */
    if (concolic_has_own_slot(ctx, record, atom)) {
        JS_FreeAtom(ctx, atom);
        return 1;
    }
    /* THE MEMBER'S VALUE IS THE ONE A READ OF THAT NAME ANSWERS, through the same entry every other read of
       this record goes through — one derivation and one identity, so `for (k in r) r[k]` and a later
       `r[<that name>]` are the same value and an @S candidate substituted for the member reaches both. */
    val = concolic_exotic_get(ctx, record, atom, record);
    /* JS_PROP_NO_EXOTIC because this IS the class's own materialisation: routing it back through
       concolic_exotic_define_own would ask this class what it reports about a member it is in the middle of
       making. The attributes are the ones concolic_exotic_get_own reports for every member of a record, which
       is what §6.1.7.3 Invariants of the Essential Internal Methods requires of a data property that "may
       return different values over time". */
    r = JS_DefinePropertyValue(ctx, record, atom, val, JS_PROP_C_W_E | JS_PROP_NO_EXOTIC);
    JS_FreeAtom(ctx, atom);
    if (r < 0)
        return -1;
    DCHECK(r == 1,
           "materialising an unknown member was refused on a record that did not already hold it — the only "
           "ordinary refusal is a non-extensible object, and this record's own enumeration has just decided a "
           "member INTO it, so the two answers are about one record and disagree");
    return 1;
}

/* §10.1.11 [[OwnPropertyKeys]] ( ) over the record. quickjs MERGES what this returns with the object's
   ordinary keys and dedups neither, so the two lists must be DISJOINT — §6.1.7.3 Invariants of the Essential
   Internal Methods states "The returned List must not contain any duplicate entries". That is maintained by
   the define below, which takes a member OUT of the example as it materialises it into the shape, and
   asserted here rather than filtered for. */
static int concolic_exotic_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;

    *ptab = NULL;
    *plen = 0;
    if (!c)
        return 0;
    if (JS_IsUndefined(c->example)) {
        /* NO EXAMPLE IS NOT AN EMPTY RECORD, and answering the empty List AS A FACT makes them one. An empty
           example is the positive statement "the payload this visitor was served holds no members"; no example
           at all is unknown input, whose key set is unknown — and the empty List says the first about the
           second, which is the defaulted-read defect one layer out: `Object.keys(x).length === 0` is then
           DECIDED for a program branching on a record the run never saw.
           SO IT IS ANSWERED AS AN ARM AND NEVER AS A FACT, and the arm is the FLOW'S OWN. The question is
           concolic.h's `concolic_own_keys_pred`, asked by the CONSUMER — which is where it can be asked,
           because a fork needs a resume point and this internal method is reached from inside a C activation
           that has none — and this is where the recorded answer is read back. CONCRETIZE ON THE DECISION, the
           same rule a browser component's C read follows where the page's own `if` over the same unknown is a
           fork: take the arm this flow committed to rather than answering with something the sibling arm
           contradicts.
           THE READ HANDS OVER THE VALUE AND NEVER REBUILDS THE KEY. solver/decide.h states why at
           decide_value_arm: the constraint key's FORMAT lives in one file, and a component that spelled its
           own would file this question under a second name that no consumer's fork could ever answer. */
        JSValue pred = own_keys_pred(ctx, c);
        int arm = decide_value_arm(pred);

        JS_FreeValue(ctx, pred);
        if (arm == 0)
            return 0;   /* THIS FLOW'S decided world: the record holds no own member this run can name. */
        if (arm == 1) {
            /* THE ARM WHERE THE RECORD HOLDS MEMBERS ANSWERS THE EMPTY EXOTIC LIST TOO, AND THAT IS NOT THE
               TWO ARMS COLLAPSING. What this hook contributes is the members the EXAMPLE holds, and there is
               no example; the members the flow decided on are ORDINARY SLOTS on the record by the time this
               runs, materialised by concolic_own_key_mint, so quickjs's own key walk has already listed them
               and adding them here would be the duplicate §6.1.7.3 Invariants of the Essential Internal
               Methods forbids ("The returned List must not contain any duplicate entries"). The two arms
               differ in what the RECORD holds, which is what the caller sees; they do not differ in what this
               hook owes on top of it.
               AND IT IS NOT ASSERTED HERE, WHICH IS A DECISION AND NOT AN OMISSION. The assert a reader
               reaches for — on this arm the record carries a slot for member 0 — is FALSE OF CORRECT
               RUNS: the members are configurable, so `delete x[k]` removes one and the record legitimately
               carries none while this arm still stands. It is also not free, because naming member 0 MINTS a
               derivation, and concolic_derived is the observation site an @S candidate's delivery is recorded
               at — an assert that mints would write a ladder rung for a substitution nobody delivered, which
               is an instrument corrupting the measurement it is watching. What does assert the arm is the
               engine's own chain, at every position, in every build. */
            return 0;
        } else {
            /* -1 is "this flow has not been asked", which is a fact about the CONSUMER and not about the
               record: whichever spelling reached this internal method did so without first forking on the
               record's key set, so there is no arm to answer with and the empty List would be the fabricated
               fact again. It also covers a record with no identity to key the question by, which is the same
               unbuilt consumer one step earlier — the ask must not be reachable for a record this engine
               cannot spell. */
            DFAIL("an unknown with NO EXAMPLE was asked to enumerate itself and its enumeration had not been "
                  "FORKED — the empty List would state that the record holds nothing, which is a fact this run "
                  "never observed. §Attacker-sources' \"Iterating an opaque collection FORKS unbounded (each "
                  "iteration a parkable flow)\" is what belongs here, and it belongs at the CONSUMER: ask "
                  "concolic_own_keys_pred over the record and fork on it before requesting "
                  "§10.1.11 [[OwnPropertyKeys]] ( ), so that this line answers the arm the flow decided");
        }
        return 0;
    }
    if (JS_IsString(c->example)) {
        /* §10.4.3 String Exotic Objects gives a String's own keys as its index properties plus `length`, so
           the empty List is FALSE for a non-empty one — a wrong positive statement rather than a missing
           capability. This class models a RECORD's members and declines a primitive example's at every other
           door too (see example_slot); the wrapper surface is the thing to build. */
        DFAIL("an unknown whose example is a STRING was asked to enumerate itself — §10.4.3 String Exotic "
              "Objects' index properties are the answer and this class does not model them; build the "
              "primitive-example wrapper surface rather than reporting the record as empty");
        return 0;
    }
    if (!JS_IsObject(c->example))
        return 0;   /* a number/boolean/bigint/null record HAS no own keys — `Object.keys(5)` is [] */
    DCHECK(!JS_IsProxy(c->example),
           "a concolic's example is a Proxy — enumerating it is that Proxy's `ownKeys` trap, which is the "
           "page's code, and an internal method reached from C has no flow base to run one on");
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, c->example, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return -1;
#if APICLIENT_DEV
    {
    uint32_t i;
    for (i = 0; i < n; i++)
        DCHECK(!concolic_has_own_slot(ctx, obj, tab[i].atom),
               "a member is BOTH an own slot of the unknown and a member of its example — quickjs appends this "
               "list to the object's ordinary keys and dedups neither, so the key would be enumerated twice "
               "and §6.1.7.3 Invariants of the Essential Internal Methods' rule for [[OwnPropertyKeys]] "
               "broken; the define that materialised it did not take it out of the example");
    }
#endif
    *ptab = tab;
    *plen = n;
    return 0;
}

/* §10.1.6 [[DefineOwnProperty]] ( propertyKey, propertyDesc ) — reached only when the ordinary own-property
 * scan found nothing, so this is §10.1.6.3 ValidateAndApplyPropertyDescriptor with `current` = whatever THIS
 * class reported, and the attributes the incoming descriptor does not restate are CURRENT'S, not false.
 *
 * WITHOUT IT THE WRITE PATH SILENTLY NARROWS EVERY HELD MEMBER. §10.1.9.2 OrdinarySetWithOwnDescriptor step
 * 2.c performs `Receiver.[[DefineOwnProperty]](P, { [[Value]]: V })` once [[GetOwnProperty]] has reported the
 * member writable — a descriptor with a VALUE and no attributes — and quickjs's ordinary define has no
 * `current` to merge it against here, so `cfg.region = "x"` on a member the payload HOLDS would materialise a
 * NON-ENUMERABLE, NON-WRITABLE, NON-CONFIGURABLE slot: the member drops out of Object.keys and the next write
 * to it throws. That is a regression this class's own [[GetOwnProperty]] creates, so this is where it is paid.
 *
 * AND MATERIALISING TAKES THE MEMBER OUT OF THE EXAMPLE, which is the whole of how the two key lists stay
 * disjoint. The example is not a pristine copy of the server's payload — it is the record's CONCRETE STATE,
 * and a page that overwrites a member has changed that state, so the server's byte for it is no longer what
 * the record holds. The removal is an ordinary property delete on the example, so the COW delta captures it on
 * the object that owns the storage and a context switch puts it back, exactly as it does the slot. */
static int concolic_exotic_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                                      JSValueConst getter, JSValueConst setter, int flags)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSPropertyDescriptor cur;
    int ret, nf = flags;

    if (!c || !concolic_exotic_get_own(ctx, &cur, obj, prop))
        return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);

    DCHECK(!(cur.flags & JS_PROP_GETSET),
           "this class reported one of its own members as an ACCESSOR — every one of them is a data property "
           "carrying a derived unknown, so a getter here is a descriptor built somewhere this file does not "
           "know about");
    if (!(nf & JS_PROP_HAS_CONFIGURABLE))
        nf |= JS_PROP_HAS_CONFIGURABLE | (cur.flags & JS_PROP_CONFIGURABLE);
    if (!(nf & JS_PROP_HAS_ENUMERABLE))
        nf |= JS_PROP_HAS_ENUMERABLE | (cur.flags & JS_PROP_ENUMERABLE);
    if (!(nf & (JS_PROP_HAS_GET | JS_PROP_HAS_SET))) {
        if (!(nf & JS_PROP_HAS_WRITABLE))
            nf |= JS_PROP_HAS_WRITABLE | (cur.flags & JS_PROP_WRITABLE);
        /* A define that restates no VALUE keeps current's — `Object.defineProperty(cfg, "region",
           {enumerable:false})` must not turn a held member into `undefined`. */
        if (!(nf & JS_PROP_HAS_VALUE)) {
            nf |= JS_PROP_HAS_VALUE;
            val = cur.value;
        }
    }
    /* OUT OF THE EXAMPLE BEFORE INTO THE SHAPE, so no window exists in which both lists name it. */
    ret = JS_DeleteProperty(ctx, c->example, prop, 0);
    DCHECK(ret > 0, "a member this class reported could not be removed from the example it was read out of — "
                    "the example's own slots are the ones JSON.parse and the reply decoders build, all of them "
                    "configurable, so a refusal is a record some other path froze");
    if (ret > 0)
        ret = JS_DefineProperty(ctx, obj, prop, val, getter, setter, nf | JS_PROP_NO_EXOTIC);
    JS_FreeValue(ctx, cur.value);
    return ret;
}

/* §10.1.10 [[Delete]] ( propertyKey ) — reached only when the ordinary own-property scan found nothing. The
   descriptor this class reports is CONFIGURABLE, so §10.1.10.1 step 4 makes a member the example holds one
   [[Delete]] must actually REMOVE, and the only place it exists is the example. Without this, `delete
   cfg.region` answered true and Object.keys went on listing `region` — [[Delete]] and [[OwnPropertyKeys]]
   disagreeing about one record. */
static int concolic_exotic_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSValue ev;
    int eflags, ret;

    if (!c || !example_slot(ctx, c->example, prop, &ev, &eflags))
        return 1;   /* nothing there to remove, which [[Delete]] reports as success */
    JS_FreeValue(ctx, ev);
    DCHECK(!JS_IsProxy(c->example),
           "a concolic's example is a Proxy — removing a member is that Proxy's `deleteProperty` trap, which "
           "is the page's code, and an internal method reached from C has no flow base to run one on");
    ret = JS_DeleteProperty(ctx, c->example, prop, 0);
    DCHECK(ret >= 0, "removing a member from a concolic's example threw — the example holds only the slots "
                     "JSON.parse and the reply decoders build and this call runs none of the page's code");
    return ret;
}

/* §10.1.1 [[GetPrototypeOf]] ( ) OVER AN UNKNOWN — AND WHY THE ANSWER IS NOT A CONCRETE ONE.
 *
 * WHAT THIS CLASS ANSWERED BEFORE THE HOOK EXISTED, AND WHY NOTHING REPORTED IT. A concolic is an ordinary
 * JSObject minted through JS_NewObjectClass, and this class registers no class_proto — so its objects carry a
 * NULL [[Prototype]], which JS_GetPrototype reads straight off the shape and hands the page as JS_NULL. That
 * is a FABRICATION in §Attacker-sources' exact sense: nothing was observed about the value, and `null` is not
 * "unknown", it is the positive claim that this value HAS NO PROTOTYPE. It is also the one answer that makes
 * an ordinary prototype-chain walk THROW instead of fork, so it did not merely mislead a report — it killed
 * the program that would have produced one, and it did so silently, because `null` is a legal answer that no
 * assert anywhere had a reason to question.
 *
 * THE DEFECT SHAPE, which is the part that stays true: an operator with no concolic arm collapses opacity to
 * whichever concrete answer its ordinary path already held. §Solver states the rule for the coercions —
 * opacity survives ToNumber, so control flow keeps forking rather than collapsing to NaN — and a chain link is
 * that same rule one internal method over. `operand_kind` above already records what a prototype walk costs
 * when it does not fork; this is the walk's other half.
 *
 * THE ANSWER IS A DERIVATION OVER THE RECEIVER, MEMOISED. See the `proto` field for why holding it is not an
 * optimisation: the page can compare two answers, and §10.4.7.2 SetImmutablePrototype ( obj, proto ) step 2
 * compares one of them against a value it was given, so an answer that differs per call is wrong twice over.
 *
 * IT DOES NOT TERMINATE ON ITS OWN, WHICH IS WHY THE LINK WALK'S FORK LANDS WITH IT. §7.3.21
 * OrdinaryHasInstance ( ctor, instance )'s step 6 asks instance.[[GetPrototypeOf]]() per link and stops on
 * null or on SameValue — both decided in C, with no fork — so a chain of derived unknowns reaches neither and
 * the walk runs for ever. The truthful model is not an infinite chain: a real chain is FINITE of unknown
 * length, so "how long" is a QUESTION and never a bound, and the question is asked at the walk. It is asked
 * there and not here because only the walk holds `proto`, which is half of what step 6.c compares.
 *
 * AND THAT QUESTION HAS THREE ANSWERS AND NOT TWO. §7.3.21's step 6 is a Repeat whose body is exactly three
 * steps — 6.a "Set instance to ? instance.[[GetPrototypeOf]]()", 6.b "If instance is null, return false",
 * 6.c "If SameValue(proto, instance) is true, return true" — so one iteration over an unknown link has three
 * feasible completions: the chain ENDS here, this link IS proto, and neither. An earlier statement of this
 * design said "is this link the end?" and therefore "a concolic bool", and a bool is a two-way partition of a
 * three-way question: it names 6.b and 6.c at once and then cannot say which, and a two-arm fork keeping only
 * 6.b DELETES the world in which `e instanceof Object` is TRUE — which on a server-injected record is the
 * world a real session is standing in. The completions had been enumerated from the branch of the text rather
 * than from what the page can observe. js_instanceof_step declares three.
 *
 * THE THIRD DOOR IS §10.4.7.2, reached from JS_SetPrototypeInternal, AND IT CRASHES RATHER THAN ANSWERING.
 * Its step 2 SameValue is a pointer test between two Objects, so a derived unknown as `current` can never
 * satisfy it: `Object.setPrototypeOf` over an unknown would report the write as refused, and its throwing
 * form would raise a TypeError, at a line real Chrome does not refuse at. It is NOT a named residual, and the
 * distinction is this file's own mechanical test rather than a preference — a residual is for code that is
 * CORRECT and narrower, and there is no case that door handles correctly today, so the answer beneath it is
 * WRONG and the rule is that wrong code crashes. Giving this entry an answer is what MADE it wrong: before,
 * the door aborted here on the way past, and a diff that fixes a fabrication must not quietly convert someone
 * else's abort into a plausible answer. The DCHECK now stands at that site, naming the ask to build — the
 * same two-completion question step 6.c asks, with nothing else attached, so it is one ask and not a walk.
 *
 * IT RUNS NONE OF THE PAGE'S CODE, which the entry's contract requires rather than suggests: it is reached
 * from JS_GetPrototype, whose C callers have no flow base to suspend into. The mint is an allocation and a
 * composition; neither calls anything of the page's. */
static JSValue concolic_exotic_get_prototype(JSContext *ctx, JSValueConst obj) {
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);

    DCHECK(c != NULL, "§10.1.1 [[GetPrototypeOf]] ( ) was dispatched to this class's entry for an object that "
                      "carries none of its state — the exotic table is reached through the class id, so an "
                      "object answering to it with no record is one this file did not mint");
    if (!c)
        return JS_NULL;
    /* DERIVED ONCE AND HELD. The operand is the RECEIVER, so the answer's identity is composed from this
       value's own — which is what makes two asks about one record one question, here and at the outcome seam
       the link walk asks through. */
    if (JS_IsUninitialized(c->proto))
        c->proto = concolic_new_derived(ctx, "[[GetPrototypeOf]]", &obj, 1, JS_UNDEFINED);
    /* concolic_new_derived answers JS_UNINITIALIZED for exactly one reason — NO operand is unknown — and the
       operand here is the receiver, which reached this entry through this class's own exotic table. So the
       sentinel surviving the mint is that dispatch having delivered a value that is not one of ours, and it
       must be said HERE: JS_GetPrototype's own check would report only that the answer is neither an Object
       nor null, which is true of the sentinel and names nothing. */
    DCHECK(!JS_IsUninitialized(c->proto),
           "the derivation of an unknown's prototype answered the no-unknown-operand sentinel over a receiver "
           "this class's own exotic table dispatched on");
    /* A NEW REFERENCE, because [[GetPrototypeOf]] is a spec internal method whose callers own what it returns —
       JS_GetPrototype's do, and so does §10.4.7.2 step 1's. The memo keeps its own. */
    return JS_DupValue(ctx, c->proto);
}

static JSClassExoticMethods g_concolic_exotic = {
    .get_own_property = concolic_exotic_get_own,
    .get_own_property_names = concolic_exotic_own_names,
    .delete_property = concolic_exotic_delete,
    .define_own_property = concolic_exotic_define_own,
    .get_property = concolic_exotic_get,
    .get_prototype = concolic_exotic_get_prototype,
    .has_property = concolic_exotic_has,
    /* The lookup is a slot read of this value's own example and a derivation composed in this file — no page
       code, by construction, which is what lets the engine's own accessor walk and its COW slot read run it
       from C. */
    .get_own_property_no_user_code = true,
};

/* HOW MANY VALUES THE SOURCE OVERLAY HAS MINTED — see concolic_source_wrap for what the number is for. Reset
   with the agent, because it is a fact about ONE document's run and the result document that reports it is per
   document; a counter that survived would make the second page in an instance report the first one's reading.
   DECLARED HERE, ABOVE ITS FIRST USE, and not beside the overlay flag it belongs with three hundred lines
   down: `concolic_init` zeroes it, so a declaration after that line does not compile at all. The accessor
   stays beside the overlay, where the reader looking for "who mints these" will be. */
static long g_source_reads;

void concolic_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    g_source_reads = 0;   /* per document: see the counter's declaration above */
    if (g_concolic_class == 0) {
        JS_NewClassID(rt, &g_concolic_class);
        DCHECK(g_concolic_class != 0, "concolic: class id allocation returned 0 — runtime class table exhausted");
    }
    if (!JS_IsRegisteredClass(rt, g_concolic_class)) {
        JSClassDef def = { "Concolic", .finalizer = concolic_finalizer, .gc_mark = concolic_gc_mark,
                           .call = concolic_call, .exotic = &g_concolic_exotic };
        int r = JS_NewClass(rt, g_concolic_class, &def);
        CHECK(r == 0, "concolic: JS_NewClass failed — cannot register the solver's value type");
    }
}

/* THE HOLDER'S END OF THE SOURCE REGISTRY — solver/engine.h's release column, run after the whole platform.
 *
 * THE ROWS ARE NOT FREED HERE. Each is a CLAIM whose claimant gives it back at its own release, and every one
 * of those releases is a row on core/platform.h's third column — which platform_agent_free runs BEFORE the
 * solver's. So an empty registry here is a checked statement about the CLAIMANTS, and a non-empty one names
 * the one that did not finish rather than leaving a strdup'd row that answers a later agent's delivery
 * question for a document that no longer exists.
 *
 * IT NAMES THE COMPONENT OUT OF THE DATA, which is the whole reason the row carries its claimant. The obvious
 * message here lists the claimants and how many rows each owes, and that list is the stale-`DFAIL` failure
 * mode CLAUDE.md describes: it stays true about the CONTRACT and goes wrong about THIS TREE the first time a
 * component is added, renamed or moved, while reading as authoritative to whoever the abort sends looking. The
 * row knows who owns it, so the assert reads it and this file names nobody.
 *
 * WHAT THIS OWNS IS THE ARRAY UNDER THEM, and it is freed rather than merely emptied. They are two different
 * facts: `g_srcs_n` is what every lookup reads, and `g_srcs`/`g_srcs_cap` is a plain `malloc`'d buffer that no
 * detector this tree has can see — not gc_obj_list (not a GC object), not the atom walk (not an atom), not
 * JS_DUMP_LEAKS's malloc_count (it counts js_malloc_rt). A release that reset the count and kept the buffer is
 * exactly the fetch_free shape core/agent_state.h records, one layer down: the next agent's first declaration
 * grows a capacity a dead runtime's reclaim allocator sized. The COUNT goes with the buffer for a harder
 * reason than symmetry — a release build compiles the assert above out, and a count left standing over a freed
 * array is a use-after-free at the next lookup rather than a leak. */
void concolic_free(void)
{
    int i;
#if APICLIENT_DEV
    if (g_srcs_n != 0)
        DFAILF("%s did not give back the attacker SOURCE `%s` before the solver's agent state was released. "
               "Each row is a claim in this component's array whose claimant releases it at its own "
               "concolic_undeclare_sources, on core/platform.h's release column, which platform_agent_free "
               "runs before this call. A row left behind is not only a leaked pair of strings: it describes "
               "a browser delivery for a document that is gone, and it is what the next agent's declaration "
               "of the same source collides with", g_srcs[0].component, g_srcs[0].src);
#endif
    free(g_srcs);
    g_srcs = NULL;
    g_srcs_n = g_srcs_cap = 0;
    /* THE INSTALLED @S SUBSTITUTION IS THIS COMPONENT'S OWN COPY, not the flow's. concolic_set_candidate strdups
       both halves out of the running flow at every switch, so the last flow to run leaves a pair behind that
       the frontier's own release cannot reach — flow_registry_free frees the FLOW's strings, which were never
       these. Freed rather than asserted null, because nothing orders the last switch against this call. */
    free(g_cand_src);
    free(g_cand_payload);
    g_cand_src = g_cand_payload = NULL;
    /* AND THE DELIVERY FACT GOES WITH THE PAIR IT IS ABOUT, because the two are read together: a `1` standing
       beside a NULL payload is a claim that some flow's bytes are in a program this agent no longer has, and
       concolic_candidate_delivered asserts exactly that they cannot come apart. It is the running flow's live
       copy, so nothing else releases it — the parked copies went with their blobs. */
    g_cand_delivered = 0;
    /* AND THE NAMES SPENT ON UNKNOWN KEYS, for the reason the SOURCE ROWS above are given back here rather
       than for the candidate pair's. Each row describes a value of a document that is gone, and the shapes it
       is keyed by are composed out of sources the next agent declares afresh — so a row left standing would
       answer the NEXT document's enumeration with the last one's provenance, which is the mis-attribution
       keyname_record's own collision assert exists to prevent, arriving across an agent boundary instead of
       within one. */
    for (i = 0; i < g_keynames_n; i++) {
        free(g_keynames[i].shape); free(g_keynames[i].src);
        free(g_keynames[i].root);  free(g_keynames[i].ident);
    }
    free(g_keynames);      g_keynames = NULL;      g_keynames_n = g_keynames_cap = 0;
    free(g_keynames_hash); g_keynames_hash = NULL; g_keynames_hash_cap = 0;
    /* AND THE PUBLISHED-NAMESPACE PATHS, for the same reason the source rows above are given back here: a path
       names a record of a document that is gone, and the addresses it is keyed by are about to be reused. */
    absent_free();
    /* AND THE CONTRADICTED-EXAMPLE LATCH, which is agent state exactly as the two above are. It is a
       one-directional fast path over a constraint chain that dies with this agent, so a latch left set makes
       the NEXT agent probe a chain in which nothing has been contradicted — not a wrong answer, but a cost
       carried by a document that never earned it, and a fact about a session that is gone. */
    g_ex_contra_any = 0;
    /* §Solver's VALUE CLASS IS DELIBERATELY NOT GIVEN BACK HERE, and that is asserted rather than commented —
       the same statement core/dom/document.c makes about §gc's realm-mark hook, for the same reason. Every live
       Concolic OUTLIVES this call: the objects are freed by JS_FreeRuntime, whose finalizer reaches each
       record through this id, and the collection a host runs before it marks each example through it. An id
       cleared here would make both of those read NULL — every record's shape, provenance, identity and example
       retained with nothing naming them, and an example the collector stopped marking freed under a value that
       still points at it. The id belongs to the runtime for as long as the runtime holds objects branded with
       it, so this is where that is checked and not where it is undone. */
    DCHECK(g_concolic_class != 0,
           "the Concolic value class was given back before the runtime that issued it — it must NOT be, and "
           "this is where that is checked: every value of this class is finalized by JS_FreeRuntime, which "
           "reaches its record through this id, and the collection before it marks each example through it");
}

static JSValue concolic_alloc(JSContext *ctx, const char *shape, const char *src, const char *root,
                              char *ident, JSValue example)
{
    JSValue obj;
    Concolic *c;

    DCHECK(g_concolic_class != 0, "concolic_new before concolic_init — the class is unregistered");
    /* NOTHING IS MINTED INSIDE A SLOTS-ONLY SPAN, asserted at the ONE mint every concolic goes through.
       concolic_slots_only_begin brackets the COW delta's baseline read and its read-back, and a value MINTED
       during one is a value the delta would record as what a slot HELD — so the unapply would write a
       per-read derivation into real storage, where it shadows this class's [[Get]] for every sibling flow and
       takes the per-flow facts that live there (an @S substitution at that member, and concretize-on-pin) with
       it. The delta reads through JS_GetOwnSlotDesc, which answers from the ordinary layer and runs none of
       the page's code, so no read-time hook is reachable from it; this is where a route that changed that
       would say so, rather than after it had silently materialised one flow's unknown as everyone's slot. */
    DCHECK(!g_slots_only,
           "a concolic value was minted inside the COW delta's slots-only span — the delta records what a SLOT "
           "held, so a value synthesised during that read is put BACK by the unapply as a real own slot and "
           "shadows this class's [[Get]] for every sibling flow");
    /* THE TWO ARE PRESENT TOGETHER OR ABSENT TOGETHER, asserted at the ONE mint every value goes through. A
       value that has a provenance HAS a root: either it is a source read, whose root is itself, or it was
       derived from something that had one. A value with a `src` and no `root` is a derivation that dropped
       the fact on the way — which is not a crash anywhere, it is a report that names a source and then states
       that no navigation delivers it. And the reverse would be a root with nothing to inject at. */
    DCHECK(!!src == !!root,
           "a concolic value carries a provenance without a delivery ROOT, or a root with no provenance — the "
           "two are one fact about where the bytes came from and every derivation inherits the second while "
           "some of them re-mint the first, so a mismatch is a derivation that forgot to thread it");
    obj = JS_NewObjectClass(ctx, g_concolic_class);
    CHECK(!JS_IsException(obj), "concolic: the value object could not be allocated — a dropped concolic "
                                "collapses a branch to a concrete arm and deletes everything behind the other");
    c = reclaim_calloc(1, sizeof *c);
    CHECK(c, "concolic_new: OOM allocating value state — a dropped concolic corrupts the flow's domain");
    c->shape = strdup(shape ? shape : "{}");
    CHECK(c->shape, "concolic: OOM copying a display shape");
    c->src = src ? strdup(src) : NULL;
    CHECK(!src || c->src, "concolic: OOM copying a source's provenance");
    c->root = root ? strdup(root) : NULL;
    CHECK(!root || c->root, "concolic: OOM copying a value's delivery root");
    c->ident = ident;       /* consume — NULL means this engine cannot spell the value; see the struct */
    c->example = example;   /* consume */
    c->cmp_op = OPCMP_NONE;
    /* AND THE ALGORITHM IS THE ONE OF THE THREE THAT reclaim_calloc CANNOT SET, which is exactly why it is
       stated here rather than left to the allocator like `rel_op`: "not an equality" is CMP_ALGO_NONE and the
       zero it would otherwise hold is JS_CONCOLIC_EQ_LOOSE, so a value that is not a predicate at all would
       answer that the page wrote `==` over it. */
    c->cmp_algo = CMP_ALGO_NONE;
    /* NOBODY HAS ASKED FOR THIS VALUE'S PROTOTYPE YET, and that is a state rather than a zero — see the field.
       It is set at the ONE mint every concolic goes through, so there is no route to a record whose sentinel
       was never written. */
    c->proto = JS_UNINITIALIZED;
    c->rel_op = REL_NONE;   /* reclaim_calloc already zeroes it; stated beside cmp_op so the two stay one act */
    JS_SetOpaque(obj, c);
    return obj;
}

static JSValue concolic_derived(JSContext *ctx, const char *shape, const char *src, const char *root,
                                char *ident, JSValue example)
{
    /* A CANDIDATE RUN substitutes one source with a breakout. The check lived only in the field-read path, so a
       source installed as a plain property value — location.hash, document.cookie — was minted once at install
       and never passed through it: its candidate could not be delivered and the sink never fired. Minting is
       the one place every source goes through, whichever way it is reached. */
    if (cand_matches(src)) {
        /* THE EXAMPLE IS CONSUMED ON THIS PATH TOO. `example` is owned by this call whichever value comes back,
           and the candidate's payload REPLACES it — a source under substitution reads the attacker's bytes, not
           what the address concretely held. Returning without the free leaked it, and it stopped being a
           dormant leak the moment sources started carrying one: location.search/hash hand over the address's
           real query and fragment, so every candidate re-fire of a URL source leaked a string. */
        JS_FreeValue(ctx, example);
        free(ident);
        return concolic_deliver(ctx, src, root, g_cand_payload);
    }
    return concolic_alloc(ctx, shape, src, root, ident, example);
}

/* A SOURCE READ — the root of every identity. Its identity IS its provenance, because nothing derived it: this
   is where an unknown enters the program. A source with no provenance has no identity either, which is the
   honest answer and the one that keeps both arms of every branch over it. */
JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example) {
    const char *f[1];

    /* A SOURCE'S SHAPE NAMES A HOLE, AND THE BRACE IS WHAT MAKES IT ONE — asserted at the ONE mint every
       source goes through, because it was true of most components and silently false for six.
       concolic_hole_key is the only route there is from a shape to a domain, and its first line is
       `if (!strchr(shape, '{')) return NULL`. So an unbraced source has no hole key, and BOTH ends of the
       domain machinery then go quiet without failing: concolic_cmp_subject mints no subject, the ordering
       hook's `txt = subj ? literal_tok(...) : NULL` means pred_set_bound is never called at all, and
       endpoint.c looks a domain up under a name nothing was ever filed under. The constraint is OBSERVED and
       DROPPED — `if (screen.width < 768)` records nothing, `navigator.userAgent === x` records nothing — and
       the parameter is emitted carrying provenance and no domain, which §@S calls a WRONG report rather than
       a partial one, because its silence about the gate reads as "anything goes".
       ENDPOINT.c CANNOT USE A LAXER RULE, which is why the brace belongs here rather than there: kv_pairs
       hands concolic_hole_key an arbitrary query-value SUBSTRING, so `lang=en-US` would mint a hole named
       `en-US` and a concrete value would borrow a domain. The brace is what tells a hole from a literal once
       the concolic that carried it is gone, so it has to be in the shape from the start.
       AND IT MAKES ABSENCE MEAN ONE THING. pred_new documents a NULL subject as the honest answer for the
       unnameable `{}` — one fact. It was also the answer for every navigator, screen and viewport member,
       which is a different fact wearing the same NULL. With this, `{}` is the only one left. */
    DCHECK(shape != NULL && strchr(shape, '{') != NULL,
           "a SOURCE was minted with a display shape that names no hole — a shape carries its provenance in "
           "braces, because concolic_hole_key reads a hole by them and it is the only path from a shape to a "
           "domain. Without one this value's every gate is observed and discarded, and its parameter reports "
           "provenance with no constraint. Spell the shape as the src in braces (core/frame/location.h is the "
           "pattern); a shape that is not simply `{src}` is fine as long as it names its hole");
    /* CONCRETIZE-ON-PIN, AT THE MINT WHERE `src` IS THE VALUE'S OWN IDENTITY — which is what makes this the
       one derivation the pin may be read at. §solver: "once `x==='admin'` pins the value, a later READ of that
       source returns the pinned bytes, so a later branch on it is decided by RUNNING the real predicate on a
       real string and does not fork at all". A pin is a fact about THIS value, so it applies exactly where the
       value being minted IS the pinned one; concolic_derived must NOT ask it, because an arithmetic or builtin
       result carries its OPERAND's `src` and pinning `location.hash` would then hand `+location.hash` the
       operand's string instead of a number the run computed.
       IT IS WHAT A PER-READ MINT NEEDS AND A ONCE-INSTALLED SOURCE NEVER DID. A source held as a plain
       property is minted once, so a re-read finds the same object and no second mint could disagree with the
       flow's constraint; an INJECTED-STATE member is minted at every read, and without this the arm that
       PROVED `__FLAGS.role === "admin"` would keep re-reading the example the server sent a logged-out
       visitor and compose `/api/user` — an @H value the run contradicted, which §@H calls an invention rather
       than a partial answer.
       AFTER THE CANDIDATE AND NEVER BEFORE IT: a candidate re-fire is delivering the attacker's bytes at this
       exact source, and a pin the exploring run happened to take must not stand in front of them. */
    if (!cand_matches(src)) {
        JSValue pv = src ? pin_of(ctx, src) : JS_UNINITIALIZED;
        if (!JS_IsUninitialized(pv)) {
            JS_FreeValue(ctx, example);
            return pv;
        }
    }
    f[0] = src;
    /* A SOURCE READ IS ITS OWN ROOT — stated here, once, rather than as a second argument every one of the
       seventeen components that owns a source would have to spell the same way twice. */
    return concolic_derived(ctx, shape, src, src, concolic_ident_compose("s", f, 1), example);
}

int concolic_is(JSValueConst v) {
    return g_concolic_class != 0 && JS_GetOpaque(v, g_concolic_class) != NULL;
}

const char *concolic_shape_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->shape : NULL;
}

const char *concolic_src_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->src : NULL;
}

const char *concolic_root_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->root : NULL;
}

const char *concolic_ident_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->ident : NULL;
}

/* THE PREDICATE A BRANCH OVER THIS VALUE IS ASKING ABOUT — see the field's declaration. It falls back to the
   value's OWN identity, which is not a default filling a hole: every value that is not a negation IS the
   predicate a branch over it tests, so the fallback is the answer rather than a stand-in for a missing one. */
const char *concolic_branch_ident_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c) return NULL;
    DCHECK(c->br_key == NULL || c->ident != NULL,
           "a value names another predicate as its branch key while having no identity of its own — the two "
           "are composed from the same operand at one mint, so one present without the other is a negation "
           "whose own derivations would collide with something else's");
    return c->br_key ? c->br_key : c->ident;
}

/* …AND THE POLARITY BETWEEN THE TWO. 0 = this value IS that predicate, 1 = its complement. Read at the branch
   and applied to the ARM, never to the key, which is the whole of how `if (p)` and `if (!p)` become one
   constraint entry with two answers instead of two entries with one each. */
int concolic_branch_neg(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c) return 0;
    DCHECK(c->br_neg == 0 || c->br_neg == 1,
           "a value carries a branch polarity that is neither itself nor its complement — the field is one "
           "bit written at one mint, so a third value is a record some other write reached");
    DCHECK(c->br_neg == 0 || c->br_key != NULL,
           "a value carries a polarity against NO predicate — a complement is a fact about something, so a "
           "1 with no key would flip the arm of a branch keyed by this value's own identity and answer every "
           "later test of it backwards");
    return c->br_neg;
}


/* THE EXAMPLE THIS FLOW MAY STILL BELIEVE — §Learning-from-replies' "the forced sibling drops the contradicted
 * example, so only gate-DEPENDENT values degrade to a shape while gate-independent values stay concrete", and
 * this accessor is where "drops" happens because it is where an example flows into everything downstream.
 *
 * WHAT IS DROPPED IS THE VALUE THE BRANCH TESTED, AND NOTHING BEHIND IT. `if (cfg.admin)` over a loaded
 * `false`, taken TRUE, proves the example wrong about THIS path and about that value; it says nothing about
 * whatever `cfg.admin` was computed from, and deriving one from the other would be the chain-inversion
 * §Re-execution forbids by name. So the fact is keyed by the value's own IDENTITY (`ident`) rather than by its
 * source: a derived value carries its operand's `src`, so keying by source would silence the operand too —
 * an inversion arrived at by accident.
 *
 * TIME IS WHY THE CHECK IS HERE AND NOT AT THE MINT. A value built BEFORE the branch baked its example in on
 * a prefix both arms share, and that example is correct for both — nothing was contradicted when it was
 * computed. A value built AFTER reads through this accessor, on the arm that did the contradicting, and gets
 * nothing: `"/api/" + cfg.admin` composed past the gate degrades to a shape, which is exactly the sentence
 * above. Reaching back to rewrite what was already computed would be re-deciding the past from a constraint.
 *
 * A VALUE WHOSE IDENTITY THIS ENGINE CANNOT SPELL KEEPS ITS EXAMPLE, and that is the sound direction rather
 * than a gap: with no key there is nowhere to file a per-flow fact, and filing it anywhere else would make one
 * flow's proof silence another flow's value. Such a value's arm is still marked FORCED (decide.c does not need
 * a key for that), so the request it builds still says what it is. */
JSValue concolic_example(JSContext *ctx, JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || JS_IsUndefined(c->example)) return JS_UNDEFINED;
    if (g_ex_contra_any && c->ident) {
        const Cons *e = cons_lookup(c->ident);
        if (e && e->ex_contra) return JS_UNDEFINED;
    }
    return JS_DupValue(ctx, c->example);
}

/* THIS FLOW TOOK AN ARM THE VALUE'S OWN EXAMPLE SAYS A REAL SESSION DOES NOT TAKE — see the accessor above for
   what it costs the value, and decide.c for who says so and when.
   IT SHARES THIS MAP WITH THE PINS AND KEYS ITSELF DIFFERENTLY, WHICH IS STATED RATHER THAN ASSUMED SAFE. A
   pin, an exclusion and a bound are keyed by a plain source path; this is keyed by a concolic_ident_compose
   output, whose encoding is `<len>:<bytes>` per field — so the two are disjoint in every spelling this engine
   produces, but by an encoding that uses ASCII digits rather than by any byte a path cannot contain, and a
   claim of impossibility here would be stronger than the format supports. WHAT MAKES THAT ACCEPTABLE IS THE
   DIRECTION OF THE FAILURE: a collision would set `ex_contra` on the entry some other name reads, so a value
   would answer with NO example and degrade to a shape. It could never fabricate one, and it cannot touch
   `val`, `excl` or `bnd`, which this function does not write. A weaker report, never a wrong one.
   IDEMPOTENT, because a path cannot un-take an arm: a second contradiction of one value on one path says
   nothing the first did not, and there is no reading to merge. */
void concolic_contradict_example(const char *ident) {
    DCHECK(ident != NULL,
           "a contradicted example was recorded against a value with no identity — there would be nowhere to "
           "file a PER-FLOW fact, and filing it under anything else would silence a value in flows that never "
           "proved anything about it. The caller checks for the identity and leaves the example alone");
    cons_entry(ident)->ex_contra = 1;
    g_ex_contra_any = 1;
}

void concolic_set_example(JSContext *ctx, JSValueConst v, JSValue example) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c) { JS_FreeValue(ctx, example); return; }
    JS_FreeValue(ctx, c->example);
    c->example = example;   /* consume */
}

static char *cstr_dup(JSContext *ctx, JSValueConst v) {   /* concrete operand -> its string form (heap copy) */
    const char *s = JS_ToCString(ctx, v);
    char *r = strdup(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* §13.15.3 step 1.c's CONCLUSION, run on the two EXAMPLES: "Let leftString be ? ToString(leftPrimitive). Let
   rightString be ? ToString(rightPrimitive). Return the string-concatenation of leftString and rightString."
   `exa`/`exb` are borrowed; the result is a new owned JSValue, or JS_UNDEFINED for "no example". */
static JSValue example_string_concat(JSContext *ctx, JSValueConst exa, JSValueConst exb) {
    const char *pa, *pb;
    JSValue example = JS_UNDEFINED;

    pa = JS_ToCString(ctx, exa);
    pb = pa ? JS_ToCString(ctx, exb) : NULL;
    /* THE `if (e)` THAT USED TO STAND HERE WAS THE CONCEALMENT, not the safety. §Solver-half: the example
       propagates because the engine RUNS the real op, so a `+` that quietly produced no example turns a
       computed value into a shape — an @H row that says `{a}{b}` where the code determined `/api/us-east-1`
       — and nothing anywhere says a concatenation was dropped. With the refusal edge underneath it a NULL
       is the physical floor and nothing else, which is exactly what a CHECK is for. */
    if (pa && pb) {
        size_t l = strlen(pa) + strlen(pb) + 1;
        char *e = reclaim_malloc(l);
        CHECK(e, "concolic +: the concatenated example could not be allocated — the sum would carry a "
                 "shape where the code computed a value");
        snprintf(e, l, "%s%s", pa, pb);
        example = JS_NewString(ctx, e);
        free(e);
    }
    if (pa) JS_FreeCString(ctx, pa);
    if (pb) JS_FreeCString(ctx, pb);
    /* A CONVERSION THAT REFUSED LEFT ITS THROW STANDING and the operator is about to report SUCCESS, so the
       page's next statement would throw a TypeError it never wrote. §7.1.19 ToString refuses only for a Symbol
       (its step 2) and for an object whose ToPrimitive threw, and the caller's assert says neither operand is
       one — so in dev this drains nothing and in release it drains what a compiled-out assert stopped saying. */
    if (JS_IsUndefined(example) && JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    return example;
}

/* §13.15.3 steps 2-7 over the two EXAMPLES, once step 1.c has said neither is a String: step 2's "NOTE: At
   this point, it must be a numeric operation", then "Let leftNumber be ? ToNumeric(leftValue)" and the same
   for the right (§7.1.3 ToNumeric, which is §7.1.4 ToNumber for everything that is not a BigInt), then step
   7's table entry for `+`, which is §6.1.6.1.7 Number::add ( x, y ). Number::add's steps ARE IEEE 754 binary64
   addition — the NaN and infinity rows, the -0𝔽 + -0𝔽 row, and finally "Return 𝔽(ℝ(x) + ℝ(y))" — so the engine
   RUNS the real operation here rather than re-implementing its cases. Borrowed operands; owned result, or
   JS_UNDEFINED for "no example". */
static JSValue example_number_add(JSContext *ctx, JSValueConst exa, JSValueConst exb) {
    double da = 0, db = 0;
    int oka = JS_ToFloat64(ctx, &da, exa) == 0;
    int okb = oka && JS_ToFloat64(ctx, &db, exb) == 0;

    /* §7.1.4 ToNumber step 2 — "If arg is either a Symbol or a BigInt, throw a TypeError exception" — is the
       ONLY refusal a primitive can reach here, and no source or derivation in this engine mints either as an
       example. So an arrival is a PRODUCER having attached a value it never computed. It is also where the
       BigInt arm gets built when one does exist: §13.15.3 step 5's "If SameType(leftNumber, rightNumber) is
       false, throw a TypeError exception" and step 6's §6.1.6.2.7 BigInt::add ( x, y ). Deliberately NOT built
       on speculation — a BigInt example would be a fabricated observation with nothing producing it. */
    DCHECK(oka && okb,
           "a concolic's example refused §13.15.3 step 3/4's §7.1.3 ToNumeric — its only primitive refusals "
           "are §7.1.4 ToNumber step 2's Symbol and BigInt, and no producer in this engine mints either as an "
           "example; build §13.15.3 step 5's SameType test and step 6's §6.1.6.2.7 BigInt::add here when one "
           "does");
    if (!oka || !okb) {
        /* The example is dropped (@H never invents) and the throw is dropped with it: it belongs to a
           coercion the PROGRAM did not perform — the program's `+` is over the unknown, not over this
           engine's guess at its concrete value, and a concolic operand's own type is not the example's. */
        DCHECK(JS_HasException(ctx),
               "§7.1.3 ToNumeric reported a refusal and left no exception standing — the drain below would "
               "then take the NEXT operator's throw instead of this one's");
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return JS_NewFloat64(ctx, da + db);
}

/* A CONCATENATION WHERE EITHER OPERAND IS CONCOLIC -> a DERIVED concolic. `op` names which spec algorithm is
   concatenating (JSConcolicAddOp in quickjs.h); matches js_add_slow's stack effect — both operands freed,
   result in sp[-2].
   THE SHAPE IS display(a)++display(b) FOR BOTH ARMS, and that is deliberate rather than an oversight of the
   numeric one. The shape is a function of the OPERAND SHAPES ALONE, because .key_name spells an unknown
   property key with it and relies on it being stable: `obj[x+1] = v` must write the slot `obj[x+1]` later
   reads. Composing it from the arm instead would make one expression name two different slots depending on
   whether an example happened to be known in that flow, which is a miss with nothing to report it.
   THE EXAMPLE IS §13.15.3's OWN TEST, ASKED OF THE EXAMPLES. An example is a concrete value this engine
   COMPUTED, so running the real operator on the two of them is running it on concrete operands, and step
   1.c's test is over their types. Steps 1.a and 1.b are NOT performed here: js_add_slow calls this before any
   coercion, and §7.1.1 over a real object is the page's own code, so the assert inside names the trampoline
   that owes them. This hook reported `concolic(5) + 3` as `"53"` for as long as it had only
   one arm, and §@H emits a computed value as an OBSERVED fact, so that string was published as a measurement
   of an endpoint the code never addressed. The concolic RESULT is unchanged either way: provenance, shape and
   identity are the same and a later branch still forks, which is what keeps both arms of the gate. */
int concolic_add_hook(JSContext *ctx, JSValue *sp, JSConcolicAddOp op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    DCHECK(op == JS_CONCOLIC_ADD_PLUS || op == JS_CONCOLIC_ADD_CONCAT,
           "a concatenation reached the concolic derivation without naming which spec algorithm it is — "
           "13.15.3 and 22.1.3.5 disagree about the numeric arm, so an unnamed caller has no answer here");
    if (!ca && !cb) return 0;

    /* THE ONE SPELLER THIS SITE MUST NOT ROUTE TO, AND THE REASON IS THE PRODUCT. Every other shape in this
       file renders the EXPRESSION the page wrote, and derived_operand_shape therefore renders a String operand
       QUOTED so the display separates what the identity separates. A concatenation's shape is not an
       expression: it is the STRING the concatenation produced, which is what makes `"/api/" + cfg.region`
       render `/api/{cfg.region}` — the @H provenance the whole endpoint surface is composed of. Quoting here
       would spell it `"/api/"{cfg.region}` and there is no version of that worth having.
       SO THIS SITE IS DELIBERATELY COARSER THAN ITS IDENTITY, and it is the counterexample to the rule stated
       at keyname_record rather than an oversight: `x + 5` and `x + "5"` compose two identities and one shape,
       and no shape change can separate them without destroying the URL. See the NAMED RESIDUAL there — the
       repair for that pair is at what .key_name may SPEND, never here. */
    char *sha = ca ? strdup(concolic_shape_c(a) ? concolic_shape_c(a) : "{}") : cstr_dup(ctx, a);
    char *shb = cb ? strdup(concolic_shape_c(b) ? concolic_shape_c(b) : "{}") : cstr_dup(ctx, b);
    CHECK(sha && shb, "concolic +: OOM shape");
    size_t ln = strlen(sha) + strlen(shb) + 1;
    char *shape = reclaim_malloc(ln); CHECK(shape, "concolic +: OOM shape concat");
    snprintf(shape, ln, "%s%s", sha, shb);
    const char *src = ca ? concolic_src_c(a) : concolic_src_c(b);
    const char *root = ca ? concolic_root_c(a) : concolic_root_c(b);

    JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    /* A CONCRETE OPERAND ALWAYS HAS A VALUE, INCLUDING `undefined` — the same distinction cmp_example draws
       and the same defect this line carried. JS_UNDEFINED is concolic.h's spelling of "carries no example";
       it is ALSO an ordinary ECMAScript value, and `x + undefined` is a real §13.15.3 whose result this
       engine can compute exactly (`"a" + undefined` is `"aundefined"`, `1 + undefined` is NaN). Reading the
       second as the first dropped the example of every such sum, which by this file's own note above turns a
       computed value into a shape with nothing saying a concatenation was lost. Absence is asked of the
       CONCOLIC side only, where it is what the encoding means. */
    int hava = ca ? !JS_IsUndefined(exa) : 1;
    int havb = cb ? !JS_IsUndefined(exb) : 1;
    JSValue example = JS_UNDEFINED;
    if (hava && havb) {
        /* STEP 1.c TESTS PRIMITIVES, and both of the ways a non-primitive can arrive here are named because
           they are different bugs with different fixes. This one operand is either a CONCOLIC's example or a
           CONCRETE operand handed straight over by the operator, and each has its own producer. */
        DCHECK(!JS_IsObject(exa) && !JS_IsObject(exb) && !JS_IsSymbol(exa) && !JS_IsSymbol(exb),
               "§13.15.3 step 1.c tests PRIMITIVES — its steps 1.a and 1.b §7.1.1 ToPrimitive both operands "
               "first — and one operand here is not one. From the CONCOLIC side that is a producer having "
               "attached a value this engine never computed: an example rides the value the interpreter "
               "actually produced, so it is an operable primitive. From the CONCRETE side it is the operator "
               "handing over its RAW operand: §7.1.1 there runs the PAGE's valueOf/toString, so it belongs on "
               "the ToPrimitive trampoline (js_toprim_operand / do_toprim_tramp) BEFORE the arm is chosen — "
               "choosing from an unconverted object takes the string arm for `x + {valueOf(){return 5}}`, "
               "which §13.15.3 makes an addition");
        /* §13.15.3 step 1.c: "If leftPrimitive is a String or rightPrimitive is a String". 22.1.3.5 asks no
           such question — its pieces are already ToString'd — so it states the string arm and skips the test. */
        if (op == JS_CONCOLIC_ADD_CONCAT || JS_IsString(exa) || JS_IsString(exb))
            example = example_string_concat(ctx, exa, exb);
        else
            example = example_number_add(ctx, exa, exb);
    }
    JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);

    char *ident;
    { const char *f[2]; char *ia = ident_of_operand(ctx, a), *ib = ident_of_operand(ctx, b);
      f[0] = ia; f[1] = ib; ident = concolic_ident_compose("+", f, 2); free(ia); free(ib); }

    JSValue result = concolic_derived(ctx, shape, src, root, ident, example);   /* consumes example and ident */
    free(sha); free(shb); free(shape);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    sp[-2] = result;
    return 1;
}

/* TWO SETS, BECAUSE THEY ANSWER TWO DIFFERENT QUESTIONS. What a concolic value DOES once it exists — how it
   adds, compares, coerces, reports its type — is the value class's own semantics, and it must be installed
   wherever a concolic can be reached at all: without it every operator falls through to the ordinary-object
   path and `"x" + document.cookie` throws "toPrimitive" from an expression the page never wrote. Where a
   concolic COMES FROM is a different decision. absent_read_hook mints one out of a global that was never set,
   and out of a field the document's own record does not hold, which is §solver's "server-injected absent state
   is unknown input" — a deliberate exploration choice, and the opposite of what a conformance run wants, where
   an unset global is a ReferenceError and a missing field is `undefined` and the spec says so.
   They were one set, so a host that wanted the first was forced to take the second and every host that
   declined took neither. The conformance runner declined, and paid for it by growing a SECOND Location
   component out of the address to avoid the concolic it could not coerce — which is how a §7.10.5 stringifier
   fix landed in a file that runner does not use. */
static JSConcolicHooks g_hooks = {
    .add = concolic_add_hook, .cmp = concolic_cmp_hook, .is = concolic_is,
    .rel = concolic_rel_hook, .type_of = concolic_typeof_hook,
    .arith = concolic_arith_hook, .to_str = concolic_tostr_hook,
    .to_bool = concolic_tobool_hook,
    .key_read = concolic_key_read_hook,
    .key_name = concolic_key_name_hook,
    /* …AND THE INVERSE, WHICH IS NOT OPTIONAL BESIDE IT. `key_name` is the only mechanism that turns an
       unknown into bytes, so a host that installs it and not this one has an enumeration that hands a page a
       real string where its own model says an unknown stands — a de-tainting placeholder, which §solver bans
       by name in the JSON.stringify case for the reason it is banned here: the taint is what makes the sink
       behind it visible at all. */
    .key_value = concolic_key_value_hook,
    .builtin = concolic_builtin_hook,
    .example = concolic_example,
    /* §10.1.11 "[[OwnPropertyKeys]] ( )" ASKED, WITHOUT WHICH THE ARM THAT ANSWERS IT IS INERT. The record
       class hands its enumeration to decide_value_arm and the engine's step_ownkeys_run asks this on the
       BRANCH seam; with the member NULL nothing is asked, decide_value_arm answers -1 for ever, and
       concolic_exotic_own_names crashes naming a consumer that in fact exists. That is a producer with no
       consumer wearing the shape of an unbuilt capability, which is the one thing a reader cannot tell apart
       from the real absence — so the line is the mechanism, not its installation.
       ITS OWN MEMBER AND NOT `.builtin`, deliberately: this predicate composes
       concolic_ident_compose("[[OwnPropertyKeys]]", {operand}, 1) while the builtin hook composes
       ("b", {operand, op}, 2), so routing through it would file one question under a SECOND key — the
       second-name defect concolic.h warns about — and `.builtin` also carries src/root, which the header
       says this predicate must not. */
    .own_keys_pred = concolic_own_keys_pred,
    /* …AND ITS TRUE ARM, WHICH IS NOT OPTIONAL BESIDE IT. A host that asks whether the record holds a member
       and cannot perform the answer has an enumeration that decided one exists and then enumerated nothing —
       the empty-List fabrication the predicate exists to prevent, arriving one step later and with a decided
       arm on top of it. The engine says so at its own site rather than trusting the pair. */
    .own_key_mint = concolic_own_key_mint,
    .lead = concolic_lead_hook };

/* Concolic VALUE propagation stays installed across scheduling AND verification, because taint must flow
   during a candidate re-fire too; the EXPLORATION hooks (branch/fork/preempt) are the scheduler's. */
void concolic_install_hooks(void)
{
    JS_SetConcolicHooks(&g_hooks);
}

/* IS THIS HOST EXPLORING? One statement of it, because every consequence of the answer is the same decision:
   an unset global becomes unknown server-injected input rather than a ReferenceError, and a browser value the
   ATTACKER controls becomes a source rather than the plain string the address computed. A host that is not
   exploring gets the browser's own answers, which is what the spec defines and what a conformance run checks.
   It is not a "mode": the value semantics above are installed unconditionally, so a concolic that reaches this
   host still adds, compares and coerces. This decides only whether one is MINTED.

   AND "NOT ASKED YET" IS A THIRD ANSWER, NOT A DEFAULT — which is what this used to be and what it cost.
   A boolean starting at 0 says "this host does not explore" before the host has said anything, and the seam
   below reads it and hands the plain value back. That is harmless for a value minted PER READ (a later read
   mints again, under whatever answer is standing by then) and permanent for a value minted ONCE FOR A REALM'S
   LIFETIME, because the realm keeps whichever answer was standing when its intrinsics ran. Measured: a host
   built its agent's first realm and installed the overlay a hundred lines later, so every Navigator
   environment member of that realm — userAgent, platform, webdriver, hardwareConcurrency, deviceMemory,
   maxTouchPoints, language — was bare-concrete for the whole session, while screen.c's members (which mint
   through concolic_new and never ask) forked normally in the same document. Nothing crashed and nothing was
   missing: the UA-sniff gate and the touch gate simply DECIDED instead of forking, so one arm's endpoints were
   learned and the other arm's world was never created. The three states make that a crash instead: a host
   DECLARES, once, before it builds a realm, and a seam reached before the declaration says so by name. */
enum { SOURCE_OVERLAY_UNDECLARED = 0, SOURCE_OVERLAY_BROWSER_ONLY, SOURCE_OVERLAY_EXPLORING };
static int g_source_overlay;
long concolic_source_reads(void) { return g_source_reads; }
/* The same answer, for a component that mints a source of its OWN — see the header. Not folded into
   concolic_source_wrap, because that seam ALSO files the value in the attacker-delivery registry and counts it
   as attacker input acquired: a §4.12.1 data block is neither, and counting one there would report a page that
   read no attacker source as one that did, which is the direction that manufactures a measurement. */
int concolic_is_exploring(void) { return g_source_overlay == SOURCE_OVERLAY_EXPLORING; }
/* …AND WHETHER THE HOST HAS ANSWERED IT AT ALL, which is the half a caller cannot get from the answer. Read by
   core/realm.c at the one call every realm's intrinsics go through, because that is the moment a member minted
   for a realm's LIFETIME freezes whatever answer is standing. */
int concolic_source_overlay_declared(void) { return g_source_overlay != SOURCE_OVERLAY_UNDECLARED; }

/* A HOST THAT WANTS THE SPEC'S OWN ANSWERS SAYS SO, rather than getting them by not speaking. A conformance
   run reaches concolic values (a document has an address, so location.c's two sources exist) and needs the
   value semantics; what it must not have is an unset global becoming unknown input, because the corpus tests
   the ReferenceError. Declaring it is not decoration: it is what makes "nobody has decided" distinguishable
   from "decided: no", and therefore what lets the assert below exist at all. */
void concolic_declare_browser_only(void)
{
    DCHECK(g_source_overlay != SOURCE_OVERLAY_EXPLORING,
           "a host declared itself browser-only AFTER installing the source overlay — the two answers are one "
           "fact about one process, and a realm built between them holds whichever was standing, so the "
           "document would fork at some of its gates and decide at the others with nothing to say which");
    g_source_overlay = SOURCE_OVERLAY_BROWSER_ONLY;
}

void concolic_install_source_overlay(void)
{
    DCHECK(g_hooks.add != NULL, "the source overlay was installed over a hook set with no value semantics — a "
                                "source that cannot be added or coerced is a value the page's first expression "
                                "throws on");
    DCHECK(g_source_overlay != SOURCE_OVERLAY_BROWSER_ONLY,
           "a host installed the source overlay AFTER declaring itself browser-only — see the DCHECK in "
           "concolic_declare_browser_only for why the two answers cannot both stand in one process");
    g_source_overlay = SOURCE_OVERLAY_EXPLORING;
    g_hooks.absent = absent_read_hook;
    /* AND THE HIT HALF OF THAT SAME QUESTION, installed with it and never without it. A published record's
       members are unknown whether or not the record HOLDS them — the server chose its extent against this
       visitor's credentials either way — so a host that took one of these would answer one spelling of one
       read symbolically and the other concretely, which is worse than answering both concretely: the surface
       it lost would be the surface the other half reports as reached. */
    g_hooks.present = absent_present_hook;
    /* THE TWO ENDS OF ONE CHANNEL, INSTALLED TOGETHER. `.publish` is what makes the engine mark the records a
       document injects at all, and `.absent` is the only thing that reads those marks; a host that installed
       one without the other would either pay for marks nobody consults or ask about a graph nobody built. */
    g_hooks.publish = absent_publish_hook;
    JS_SetConcolicHooks(&g_hooks);
}

/* THE ONE SEAM between a value the BROWSER computed and the SOLVER's view of it. §CLAUDE splits them exactly
   here: the browser half computes what the spec says the member is — `location.search` IS the address's query,
   and every document has an address — and the solver half decides that an attacker controls it, so it is
   ALSO a symbolic source that forks control flow. Written as one call rather than each component minting a
   concolic itself, because the components that mint one directly are the ones a non-exploring host cannot
   use at all, which is what grew a second Location out of the address.
   `computed` is CONSUMED, and it becomes the source's EXAMPLE: the value is opaque for control flow and still
   knows what it concretely is, which is §solver's whole triple rather than a choice between the two. */
/* AND A DECLARED SOURCE'S TWO HALVES MUST AGREE, ASSERTED AT THE MINT — the invariant this seam exists to hold
   and the one nothing checked. A source is a PROVENANCE (`location.hash`, what an @S record names and what
   concolic_declare_source registers) and a DISPLAY SHAPE (`{location.hash}`, what the @H surface prints as a
   param's value), and every component that declares a browser delivery spells the second as the first in
   braces: location's two, document.cookie, document.referrer, and file_system's `{file:NAME}`. That was five
   independent string literals agreeing by hand.
   THE COST OF NOT ASSERTING IT IS A CONSUMER THAT CANNOT SPELL THE SOURCE, and it has been paid twice. The
   offscreen grew a `{hash}|{search}|{pm}|{reply}` taxonomy against a shape this engine has never emitted (see
   this file's header), and test_forced.c's `loc-hash-param` row asked whether `location.hash` reaches an @H
   param as `{hash}` — a spelling no producer writes — so it read 0 while the shape, the provenance, the `+`
   propagation and the emission were all intact. A row that can only be 0 is worse than a crash: it names a
   mechanism as broken and sends the next reader into it.
   IT IS SCOPED TO DECLARED SOURCES ON PURPOSE. An UNdeclared one legitimately carries a shape that is not its
   name — `{hidden|visible}` is a DOMAIN and `navigator.userAgent` is a member path — and §Solver's rule is that
   a shape states what the value can be. What a DECLARED source additionally owes is a hole the report and the
   PoC can both name, which is exactly `{` src `}`. */
JSValue concolic_source_wrap(JSContext *ctx, const char *shape, const char *src, JSValue computed)
{
    /* AND THE HOST MUST HAVE ANSWERED BEFORE THIS SEAM IS ASKED. The answer this call hands back is the whole
       of whether the value forks, and a caller that mints for a realm's lifetime cannot come back and ask
       again — see the three states above for the session that lost every Navigator gate to exactly that. */
    DCHECK(g_source_overlay != SOURCE_OVERLAY_UNDECLARED,
           "a browser component minted an attacker source before this host said whether it EXPLORES — the "
           "answer decides whether the value forks control flow, so a mint taken here silently gets the "
           "browser-only one. The host declares once, before it builds its agent's first realm: "
           "concolic_install_source_overlay for a solver host, concolic_declare_browser_only for a "
           "conformance one");
    if (g_source_overlay != SOURCE_OVERLAY_EXPLORING)
        return computed;
    /* THE ONE POINT AT WHICH THIS DOCUMENT'S RUN ACQUIRES ATTACKER-CONTROLLED INPUT, COUNTED THERE. Every
       component that owns an attacker source mints through this call, so this is the whole of "did the page
       read one", and it is the first of the two facts an empty @S surface collapses. An @S surface with no
       entries has at least four readings and they take opposite actions: the page never read an attacker
       source; it read one and nothing tainted ever reached a code-execution sink; something tainted reached one
       and was suppressed because the check on it was unforgeable; or no sink ran at all. The last three are
       counted at the arrival (solver/solve.h); this is the first, and without it "we looked and there is
       nothing" is spelled exactly like "we never looked".
       AFTER THE OVERLAY GATE, so it counts values MINTED and not calls made. A conformance host installs no
       overlay and this function is then the identity — counting above the gate would report a browser run as
       one that acquired attacker input, which is the direction that manufactures a measurement. */
    g_source_reads++;
#if APICLIENT_DEV
    if (src && concolic_source_encodes(src)) {
        char *hole = shapef("{%s}", src);

        DCHECKF(shape && !strcmp(shape, hole),
                "the attacker source `%s` was minted with the display shape `%s`, and the one its own "
                "declaration spells is `%s`. A declared source's shape is its provenance in braces — that is "
                "what makes it a hole an @H param and an @S envelope can both name — so this is a second "
                "spelling of one fact, and the consumer that reads the other one reports a mechanism as "
                "broken forever. Spell both halves from the component's own token (core/frame/location.h)",
                src, shape ? shape : "(none)", hole);
        free(hole);
    }
#endif
    return concolic_new(ctx, shape, src, computed);
}

/* The permutation that sorts `srcs`, so the composed key is a property of the set. Insertion sort: `n` is a
   component's fact count and never a page's data, and the sort must be over the same order the shapes are then
   joined in or the display and the key would name their members in two different orders. */
static char *concolic_joint_join(const char *const *parts, const int *order, int n)
{
    size_t seplen = strlen(CONCOLIC_JOINT_SEP), len = 1, at = 0;
    char *out;
    int i;

    for (i = 0; i < n; i++) len += strlen(parts[order[i]]) + (i ? seplen : 0);
    /* SIZED FROM THE MEMBERS RATHER THAN INTO A FIXED BUFFER: a truncated identity is two different domains
       under one key, so a later branch over one would be decided by a branch over the other. */
    out = reclaim_malloc(len);
    CHECK(out, "concolic: OOM composing a JOINT source identity — a value whose domain could not be spelled "
               "would cross to the page as a bare number with every arm behind it deleted");
    for (i = 0; i < n; i++) {
        if (i) { memcpy(out + at, CONCOLIC_JOINT_SEP, seplen); at += seplen; }
        memcpy(out + at, parts[order[i]], strlen(parts[order[i]]));
        at += strlen(parts[order[i]]);
    }
    out[at] = '\0';
    DCHECK(at + 1 == len, "a joint identity was composed to a different length than it was measured for");
    return out;
}

JSValue concolic_source_wrap_joint(JSContext *ctx, const char *const *shapes, const char *const *srcs,
                                   int n, JSValue computed)
{
    int *order;
    char *shape, *src;
    JSValue out;
    int i;

    DCHECK(n >= 1 && shapes != NULL && srcs != NULL,
           "a joint domain was minted over NO members — a value derived from nothing is one the cascade and "
           "the layout determined, and its caller must hand back the computed value rather than ask for a "
           "domain over an empty set");
    if (!g_source_overlay)
        return computed;
    for (i = 0; i < n; i++) {
        DCHECK(shapes[i] != NULL && srcs[i] != NULL,
               "a member of a joint domain arrived with no display shape or no source identity — every member "
               "is one row of its component's own seam, so a NULL is a row that was never filled in");
        DCHECK(strstr(srcs[i], CONCOLIC_JOINT_SEP) == NULL && strstr(shapes[i], CONCOLIC_JOINT_SEP) == NULL,
               "a member's own identity contains the separator a joint identity is composed with, so two "
               "DIFFERENT sets of facts would compose to the same key — and a branch over one would then be "
               "decided by a branch the flow took over the other. Give the composition a separator this "
               "component's identities cannot contain");
    }
    order = reclaim_malloc((size_t)n * sizeof *order);
    CHECK(order, "concolic: OOM ordering a joint domain's members");
    ident_set_order(srcs, n, order);
    for (i = 1; i < n; i++)
        DCHECK(strcmp(srcs[order[i - 1]], srcs[order[i]]) < 0,
               "the SAME source appears twice in one joint domain. A set holds each member once, so this key "
               "would count how many times the arithmetic touched a fact rather than name which facts the "
               "value is a function of — and the same dependence assembled by a different route would carry a "
               "different identity and fork a predicate this flow has already decided");
    shape = concolic_joint_join(shapes, order, n);
    src = concolic_joint_join(srcs, order, n);
    out = concolic_source_wrap(ctx, shape, src, computed);
    free(order); free(shape); free(src);
    return out;
}
